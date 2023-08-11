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
/* variable to specify workload type */
static float g_rw_ratio = 1.0;
static bool g_access_rand = false;
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

void append_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, uint64_t zslba, uint32_t lba_count);
void read_block(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, uint64_t slba, uint32_t lba_count)
*/

/* variables for io request */
static uint64_t g_num_rw = 0;
static uint32_t g_num_r = 0; // for sequential
static uint32_t g_num_w = 0; // for sequential
static uint32_t g_num_zone_r = 0; // for random
static uint32_t g_num_zone_w = 0; // for random
static uint32_t g_num_io_block = 1;
static uint64_t g_start_tsc = 0;
static uint64_t g_end_tsc = 0;

static void
send_seq(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
    if (g_num_io_block > g_max_append_byte / g_block_byte) {
        fprintf(stderr, "Number of blocks to access greater than zone append size limit.\n");
        return exit(1);
    }

    g_start_tsc = spdk_get_ticks();
    for (uint64_t zone = 0; zone < g_max_open_zone; zone++) {
        uint64_t zslba = zone * g_zone_sz_blk;
        /* append one zone*/
        for (uint64_t slba = zslba; slba < zslba + g_zone_capacity; slba = slba + g_num_io_block) {
            if (slba + g_num_io_block > zslba + g_zone_capacity) { // if out of range
                break;
            }
            //printf("zslba = 0x%lx, slba = 0x%lx \n", zslba, slba);
            for (; outstanding_commands >= g_queue_depth; spdk_nvme_qpair_process_completions(qpair, 0)); // qd
            if (g_num_w > 0) {
                append_zone(ns, qpair, zslba, g_num_io_block);
                g_num_w--;
                //printf("write ");
            } else if (!g_num_w && g_num_r) {
                read_block(ns, qpair, slba, g_num_io_block);
                g_num_r--;
                //printf("read  ");
            }
            //printf("g_num_w = %u g_num_r = %u\n", g_num_w, g_num_r);
        }
        for (; outstanding_commands; spdk_nvme_qpair_process_completions(qpair, 0)); // qd
    }
    g_end_tsc = spdk_get_ticks();
}


static void
send_rand(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{   
    if (g_num_io_block > g_max_append_byte / g_block_byte) {
        fprintf(stderr, "Number of blocks to access greater than zone append size limit.\n");
        return exit(1);
    }

    g_start_tsc = spdk_get_ticks();
    uint32_t write = g_num_zone_w;
    uint32_t read = g_num_zone_r;
		
    for (uint64_t lba = 0; lba < g_zone_capacity; lba += g_num_io_block) {
        if (g_zone_capacity - lba < g_num_io_block) { // if out of range
            break;
        }
		
        for (uint64_t zone = 0; zone < g_max_open_zone; zone++) {
            uint64_t zslba = zone * g_zone_sz_blk;
            uint64_t slba = zone * g_zone_sz_blk + lba;
            for (; outstanding_commands >= g_queue_depth; spdk_nvme_qpair_process_completions(qpair, 0)); // qd
            if (write) {
                //printf("write zslba = 0x%lx\n", zslba); 
                append_zone(ns, qpair, zslba, g_num_io_block);
            } else if (!write && read) {
                //printf("read slba = 0x%lx \n", slba);
                read_block(ns, qpair, slba, g_num_io_block);
            }
        }
        if (write) {
            write--;
            //printf("write = %d\n", write); 
        } else {
            read--;
            //printf("read = %d\n", read); 
        }
        for (; outstanding_commands; spdk_nvme_qpair_process_completions(qpair, 0)); // qd
    }

    g_end_tsc = spdk_get_ticks();
}

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("%s <options>\n", program_name);
    printf("\n");
    printf(" -r, Random access mode.\n");
    printf(" -b, Number of blocks to access. It must be power of 2 and not greater than zone append size limit.\n");
    printf(" -q, Queue depth between 1 to 256. If non specify, default queue depth is 256.\n");
    printf(" -m, read/write ratio must be the value between 0 to 1. If non specify, default is read 100%%.\n");
    spdk_trace_mask_usage(stdout, "-e");
    printf(" -t, enable spdk_trace_record to capture more trace.\n");
    printf("     (-t must be used with -e)\n");
}

static int
parse_args(int argc, char **argv)
{
    int op;

    while ((op = getopt(argc, argv, "e:rtb:q:m:")) != -1) {
        switch (op) {
        case 'e':
            g_spdk_trace = true;
            g_tpoint_group_name = optarg;
            break;
        case 'r':
            g_access_rand = true;
            break;
        case 't':
            g_spdk_trace_record = true;
            break;
        case 'b':
            g_num_io_block = atoi(optarg);
            if(g_num_io_block == 0) {
                g_num_io_block = 1;
            }else if (g_num_io_block & (g_num_io_block - 1)) {
                fprintf(stderr, "Number of blocks must be power of 2.\n");
                return 1;
            }
            break;
        case 'q':
            g_queue_depth = atoi(optarg);
            break;
        case 'm':
            g_rw_ratio = atof(optarg);
            if(g_rw_ratio < 0.0 || g_rw_ratio > 1.0) {
                fprintf(stderr, "r/w ratio must be the value between 0 to 1.\n");
                return 1;
            }
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
    env_opts.name = "testreplay_nvme";
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
    // for sequential
    g_num_rw = g_max_open_zone * (g_zone_capacity / g_num_io_block);
    g_num_r = g_num_rw * g_rw_ratio;
    g_num_w = g_num_rw - g_num_r;
    // for random
    g_num_zone_r = (g_zone_capacity / g_num_io_block) * g_rw_ratio;
    g_num_zone_w = (g_zone_capacity / g_num_io_block) - g_num_zone_r;

    /* Send request */
    if (!g_access_rand) {
        send_seq(ns_entry->ns, ns_entry->qpair);
    } else {
        send_rand(ns_entry->ns, ns_entry->qpair);
    }    

    uint64_t tsc_diff = g_end_tsc - g_start_tsc;
    uint64_t tsc_rate = spdk_get_ticks_hz();
    float sec_diff = tsc_diff / tsc_rate;

    printf("%-16s: %15s \n", "Access mode", g_access_rand ? "Randomness" : "Sequence");
    printf("%-16s: %15ld \n", "Requests number", g_num_rw + g_num_zone);
    printf("%-16s: %15.3f (s) \n", "Total time", sec_diff);
    printf("%-16s: %15.3f \n", "IOPS" , (g_num_rw + g_num_zone) / sec_diff);

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
