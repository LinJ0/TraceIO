#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/nvme_spec.h"
#include "spdk/log.h"
#include "trace_io.h"

struct ctrlr_entry {
    struct spdk_nvme_ctrlr *ctrlr;
    TAILQ_ENTRY(ctrlr_entry) link;
    char name[1024];
};

struct ns_entry {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_ns *ns;
    TAILQ_ENTRY(ns_entry) link;
    struct spdk_nvme_qpair *qpair;
};

struct io_task {
    struct spdk_nvme_qpair *qpair;
};

/* variables for init nvme */
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};
/* variables for enable spdk trace tool */
static bool g_spdk_trace = false;
static const char *g_tpoint_group_name = NULL;
/* variables for pool command complete */
static uint32_t outstanding_commands = 0;

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
    struct ns_entry *entry;

    if (!spdk_nvme_ns_is_active(ns)) {
        return;
    }

    entry = (struct ns_entry *)malloc(sizeof(struct ns_entry));
    if (entry == NULL) {
        perror("ns_entry malloc");
        exit(1);
    }

    entry->ctrlr = ctrlr;
    entry->ns = ns;
    TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

    printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
            spdk_nvme_ns_get_size(ns) / 1000000000);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
     struct spdk_nvme_ctrlr_opts *opts)
{
    printf("Attaching to %s\n", trid->traddr);

    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
      struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    int nsid;
    struct ctrlr_entry *entry;
    struct spdk_nvme_ns *ns;
    const struct spdk_nvme_ctrlr_data *cdata;

    /* register ctrlr */
    entry = (struct ctrlr_entry *)malloc(sizeof(struct ctrlr_entry));
    if (entry == NULL) {
        perror("ctrlr_entry malloc");
        exit(1);
    }

    printf("Attached to %s\n", trid->traddr);

    cdata = spdk_nvme_ctrlr_get_data(ctrlr);

    snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

    entry->ctrlr = ctrlr;
    TAILQ_INSERT_TAIL(&g_controllers, entry, link);

    /*
     * Each controller has one or more namespaces.
     * Note that in NVMe, namespace IDs start at 1, not 0.
     */
    for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
        nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        if (ns == NULL) {
            continue;
        }
        register_ns(ctrlr, ns);
    }
}

static void
cleanup(void)
{
    struct ns_entry *ns_entry, *tmp_ns_entry;
    struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
    struct spdk_nvme_detach_ctx *detach_ctx = NULL;

    TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
        TAILQ_REMOVE(&g_namespaces, ns_entry, link);
        free(ns_entry);
    }

    TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
        TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
        spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
        free(ctrlr_entry);
    }

    if (detach_ctx) {
        spdk_nvme_detach_poll(detach_ctx);
    }
}

/* allocate io qpair & free io qpair start */
static void
reset_zone_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;
    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Reset all zone error - status = %s\n",
                spdk_nvme_cpl_get_status_string(&cpl->status));
        outstanding_commands--;
        exit(1);
    }
    outstanding_commands--;
}

static void
reset_all_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
    struct io_task task;
    task.qpair = qpair;

    outstanding_commands++;
    int err = spdk_nvme_zns_reset_zone(ns, qpair, 0, true, reset_zone_complete, &task);
    if (err) {
        fprintf(stderr, "Reset all zones failed, err = %d.\n", err);
        exit(1);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
}

static void
free_qpair(struct spdk_nvme_qpair *qpair)
{
    spdk_nvme_ctrlr_free_io_qpair(qpair);
}

static struct ns_entry *
alloc_qpair(struct spdk_env_opts *opts)
{
    struct ns_entry *ns_entry;
    
    /* specify namespace and allocate io qpair for the namespace */
    ns_entry = TAILQ_FIRST(&g_namespaces);
    ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
    if (ns_entry->qpair == NULL) {
        printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
        return NULL;
    }

    if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
        /* reset zone before write */
        reset_all_zone(ns_entry->ns, ns_entry->qpair);
        printf("Reset all zone complete.\n");
    } else {
        printf("Not ZNS namespace\n");
    }

    return ns_entry;
}
/* allocate io qpair & free io qpair end */

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("%s <options>\n", program_name);
    printf("\n");
    spdk_trace_mask_usage(stdout, "-e");
}

static int
parse_args(int argc, char **argv)
{
    int op;

    while ((op = getopt(argc, argv, "e:")) != -1) {
        switch (op) {
        case 'e':
            g_spdk_trace = true;
            g_tpoint_group_name = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    return 0;
}

int
main(int argc, char **argv)
{
    struct spdk_env_opts env_opts;

    int rc = parse_args(argc, argv);
    if (rc != 0) {
        return rc;
    }

    /* Initialize env */
    spdk_env_opts_init(&env_opts);
    env_opts.name = "reset_zns";
    if (spdk_env_init(&env_opts) < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        return 1;
    }

    /* Enable spdk trace */
    if (g_spdk_trace) {
        rc = enable_spdk_trace(env_opts.name, g_tpoint_group_name);
        if (rc != 0) {
            fprintf(stderr, "Invalid tpoint group name\n");
            goto exit;
        }
    }

    /* Get trid */
    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

    /* Register ctrlr & register ns */
    printf("Initializing NVMe Controllers\n");

    rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed\n");
        goto exit;
    }

    if (TAILQ_EMPTY(&g_controllers)) {
        fprintf(stderr, "no NVMe controllers found\n");
        goto exit;
    }
    printf("Initialization complete.\n");
    /* Get namspace entry & allocate io qpair */
    /* If namespace is ZNS, it would reset all zone before send request */
    struct ns_entry *ns_entry = alloc_qpair(&env_opts);
    if (!ns_entry) {
        fprintf(stderr, "Failed to alloc_qpair()\n");
        goto exit;
    }

    /* Free io qpair after send request */
    free_qpair(ns_entry->qpair);

    exit:
    cleanup();
    spdk_env_fini();
    return 0;
}
