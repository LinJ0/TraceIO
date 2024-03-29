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
    uint64_t slba;
    uint32_t nlb;
    void *buf;
};

/* variables for init nvme */
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};
static uint32_t g_queue_depth = 0;
/* variables for enable spdk trace tool */
static bool g_spdk_trace = false;
static bool g_spdk_trace_record = false;
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
    task.slba = 0;
    task.nlb = 0;

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

    struct spdk_nvme_io_qpair_opts qpair_opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &qpair_opts, sizeof(opts));
    if (g_queue_depth == 0) {
        g_queue_depth = qpair_opts.io_queue_size;
    }
    printf("Queue depth is %d.\n", g_queue_depth);

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

/* info about bdev device */
uint32_t g_block_byte = 0;
/* info about zone */
uint64_t g_num_zone = 0;
uint64_t g_zone_capacity = 0;
uint64_t g_zone_sz_blk = 0;
uint32_t g_max_open_zone = 0;
uint32_t g_max_active_zone = 0;
uint32_t g_max_append_byte = 0;

/* Get namespace & zns information start */
static void
report_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Report zone error - status = %s\n",
                spdk_nvme_cpl_get_status_string(&cpl->status));
        outstanding_commands--;
        exit(1);
    }
    outstanding_commands--;
}

static void
report_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba)
{
    struct io_task task;
    task.qpair = qpair;
    task.slba = zslba;
    task.nlb = 0;

    size_t report_bufsize = sizeof(struct spdk_nvme_zns_zone_report) + sizeof(struct spdk_nvme_zns_zone_desc);
    uint8_t *report_buf = calloc(1, report_bufsize);
    if (!report_buf) {
        printf("Zone report allocation failed!\n");
        exit(1);
    }

    outstanding_commands++;
    int err = spdk_nvme_zns_report_zones(ns, qpair, report_buf, report_bufsize,
                                        zslba, SPDK_NVME_ZRA_LIST_ALL, true, report_complete, &task);
    if (err) {
            fprintf(stderr, "Report zone failed, err = %d.\n", err);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }

    uint32_t zrs = sizeof(struct spdk_nvme_zns_zone_report);
    struct spdk_nvme_zns_zone_desc *zdesc = (struct spdk_nvme_zns_zone_desc *)(report_buf + zrs);
    g_zone_capacity = zdesc->zcap;

    free(report_buf);
}

static void
zns_info(struct ns_entry *ns_entry)
{
    if (spdk_nvme_ns_get_csi(ns_entry->ns) != SPDK_NVME_CSI_ZNS) {
        return;
    }
    
    report_zone(ns_entry->ns, ns_entry->qpair, 0);
    
    g_block_byte = spdk_nvme_ns_get_sector_size(ns_entry->ns);
    g_num_zone = spdk_nvme_zns_ns_get_num_zones(ns_entry->ns);
    g_zone_sz_blk = spdk_nvme_zns_ns_get_zone_size_sectors(ns_entry->ns);
    g_max_append_byte = spdk_nvme_zns_ctrlr_get_max_zone_append_size(ns_entry->ctrlr);
    g_max_open_zone = spdk_nvme_zns_ns_get_max_open_zones(ns_entry->ns);
    g_max_active_zone = spdk_nvme_zns_ns_get_max_active_zones(ns_entry->ns);

    printf("\nNVMe ZNS Zone Information\n");
    printf("%-20s: %u (bytes)\n", "Size of LBA", g_block_byte);
    printf("%-20s: %lu\n", "Number of Zone", g_num_zone);
    printf("%-20s: 0x%lx (blocks)\n", "Size of Zone",g_zone_sz_blk);
    printf("%-20s: 0x%lx (blocks)\n", "Zone capacity", g_zone_capacity);
    printf("%-20s: %u (blocks)\n", "Max Zone Append Size", g_max_append_byte / g_block_byte);
    printf("%-20s: %u\n", "Max Open Zone", g_max_open_zone);
    printf("%-20s: %u\n", "Max Active Zone", g_max_active_zone);
    printf("\n");
}
/* Get namespace & zns information end */

/* zone mgmt send start */
static void
open_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Open zone error - zslba = 0x%lx, status = %s\n",
                task->slba, spdk_nvme_cpl_get_status_string(&cpl->status));
    }

    if (task) {
        if (task->buf) {
            spdk_free(task->buf);
        }
        free(task);
    }
    outstanding_commands--;
}

static void
open_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba)
{
    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->slba = zslba;
    task->nlb = 1;
    task->buf = NULL;

    outstanding_commands++;
    int err = spdk_nvme_zns_open_zone(ns, qpair, zslba, false, open_complete, task);
    if (err) {
        fprintf(stderr, "Open zone failed, err = %d.\n", err);
        exit(1);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
}

static void
close_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Close zone error - zslba = 0x%lx, status = %s\n",
                task->slba, spdk_nvme_cpl_get_status_string(&cpl->status));
    }

    if (task) {
        if (task->buf) {
            spdk_free(task->buf);
        }
        free(task);
    }
    outstanding_commands--;
}

static void
close_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba)
{
    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->slba = zslba;
    task->nlb = 1;
    task->buf = NULL;

    outstanding_commands++;
    int err = spdk_nvme_zns_close_zone(ns, qpair, zslba, false, close_complete, task);
    if (err) {
        fprintf(stderr, "Close zone failed, err = %d.\n", err);
        exit(1);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
}

static void
finish_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Finish zone error - zslba = 0x%lx, status = %s\n",
                task->slba, spdk_nvme_cpl_get_status_string(&cpl->status));
    }
    
    if (task) {
        if (task->buf) {
            spdk_free(task->buf);
        }
        free(task);
    }
    outstanding_commands--;
}

static void
finish_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba)
{
    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->slba = zslba;
    task->nlb = 1;
    task->buf = NULL;

    outstanding_commands++;
    int err = spdk_nvme_zns_finish_zone(ns, qpair, zslba, false, finish_complete, task);
    if (err) {
        fprintf(stderr, "Finish zone failed, err = %d.\n", err);
        exit(1);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
}

static void
append_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Append zone error - zslba = 0x%lx, nlb = %d, status = %s\n",
                task->slba, task->nlb, spdk_nvme_cpl_get_status_string(&cpl->status));
    }

    if (task) {
        if (task->buf) {
            spdk_free(task->buf);
        }
        free(task);
    }
    outstanding_commands--;
}

static void
append_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba, uint32_t lba_count)
{
    /* malloc R/W buffer */
    char *buf = (char *)spdk_zmalloc(lba_count * g_block_byte, g_block_byte,
             NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (!buf) {
        fprintf(stderr, "Fail to malloc buf\n");
        exit(1);
    }
    snprintf(buf, (size_t)lba_count * g_block_byte, "%s", "Hello World!\n");
    
    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->slba = zslba;
    task->nlb = lba_count;
    task->buf = buf;

    outstanding_commands++;
    int err = spdk_nvme_zns_zone_append(ns, qpair, buf, zslba, lba_count, append_complete, task, 0);
    if (err) {
        fprintf(stderr, "Append zone failed, err = %d.\n", err);
        exit(1);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
    //printf("outstanding = %u\n", outstanding_commands);
}

static void
read_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;
    
    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Read block error - slba = 0x%lx, nlb = %d, status = %s\n",
                task->slba, task->nlb, spdk_nvme_cpl_get_status_string(&cpl->status));
    }

    if (task) {
        if (task->buf) {
            spdk_free(task->buf);
        }
        free(task);
    }
    outstanding_commands--;
}

static void
read_block(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba, uint32_t lba_count)
{
    /* malloc R/W buffer */
    char *buf = (char *)spdk_zmalloc(lba_count * g_block_byte, g_block_byte,
             NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (!buf) {
        fprintf(stderr, "Fail to malloc buf\n");
        exit(1);
    }    
    memset(buf, 0, (size_t)lba_count * g_block_byte);

    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->slba = slba;
    task->nlb = lba_count;
    task->buf = buf;

    outstanding_commands++;
    int err = spdk_nvme_ns_cmd_read(ns, qpair, buf, slba, lba_count, read_complete, task, 0);
    if (err) {
            fprintf(stderr, "Append zone failed, err = %d.\n", err);
            exit(1);
    }
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
}
/* zone mgmt send end */

/* 
uint32_t g_block_byte = 0;
struct spdk_nvme_zns_zone_report *report;
uint64_t g_num_zone = 0;
uint64_t g_zone_capacity = 0;
uint64_t g_zone_sz_blk = 0;
uint32_t g_max_open_zone = 0;
uint32_t g_max_active_zone = 0;
uint32_t g_max_append_blk = 0;

void finish_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba);
void append_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, uint64_t zslba, uint32_t lba_count);
void read_block(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, uint64_t slba, uint32_t lba_count)
*/

static void
send_req(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{ 
    /* Use the runtime PID to set the random seed */
    srand(getpid());
	
    // R/W ratio = 34%
    for (int i = 0; i < 198; i++) {
        uint32_t blksz = 1 << (rand() % 6); // block 1, 2, 4, 8, 16, 32
        uint64_t zone =  rand() % 16; // zone 0 ~ zone 15
        uint64_t zslba = zone * g_zone_sz_blk;
        append_zone(ns, qpair, zslba, blksz);
    }
	
    for (int j = 0; j < 102; j++) {
        uint32_t blksz = 1 << (rand() % 6); // block 1, 2, 4, 8, 16, 32
        uint64_t zone =  rand() % 16; // zone 0 ~ zone 15
        uint64_t lba = rand() % 16352; //16384 - 32 blocks to avoid out of boundary
        uint64_t slba = zone * g_zone_sz_blk + lba;
        read_block(ns, qpair, slba, blksz);
    }
	
    for (int k = 0; k < 12; k++) {
        uint64_t zone =  rand() % 16; // zone 0 ~ zone 15
        uint64_t zslba = zone * g_zone_sz_blk;
        if (k < 6) {
            open_zone(ns, qpair, zslba);
        } else if (k >= 6 && k < 10) {
            close_zone(ns, qpair, zslba);
        } else {
            finish_zone(ns, qpair, zslba);
        }
    }  
}

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("%s <options>\n", program_name);
    printf("\n");
    spdk_trace_mask_usage(stdout, "-e");
    printf(" -t, enable spdk_trace_record to capture more trace.\n");
    printf("     (-t must be used with -e)\n");
}

static int
parse_args(int argc, char **argv)
{
    int op;

    while ((op = getopt(argc, argv, "e:t")) != -1) {
        switch (op) {
        case 'e':
            g_spdk_trace = true;
            g_tpoint_group_name = optarg;
            break;
        case 't':
            g_spdk_trace_record = true;
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
    env_opts.name = "demo";
    if (spdk_env_init(&env_opts) < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        return 1;
    }

    /* Enable spdk trace */
    if (!g_spdk_trace && g_spdk_trace_record) {
        usage(argv[0]);
        return 1;
    }

    if (g_spdk_trace) {
        rc = enable_spdk_trace(env_opts.name, g_tpoint_group_name);
        if (rc != 0) {
            fprintf(stderr, "Invalid tpoint group name\n");
            goto exit;
        }
    }

    static pid_t spdk_pid = 0;
    if (g_spdk_trace && g_spdk_trace_record) {
        spdk_pid = enable_spdk_trace_record(env_opts.name, getpid());
        if (spdk_pid == 0) {
            fprintf(stderr, "Fail to exec spdk_trace_record\n");
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

    /* Get namespace & zns information */
    zns_info(ns_entry);

    /* Send I/O request */
    send_req(ns_entry->ns, ns_entry->qpair);

    /* Free io qpair after send request */
    free_qpair(ns_entry->qpair);

    exit:
    cleanup();
    spdk_env_fini();
    sleep(1); /* waiting for spdk_trace_record capture whole trace */
    if (g_spdk_trace && g_spdk_trace_record && spdk_pid != 0) {
        disable_spdk_trace_record(spdk_pid);
    }
    return 0;
}
