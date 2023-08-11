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
#include "trace_io.h"

#define ENTRY_MAX 10000 /* number of trace_io_entry */

struct ctrlr_entry {
    struct spdk_nvme_ctrlr *ctrlr;
    TAILQ_ENTRY(ctrlr_entry) link;
    char name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns	*ns;
	TAILQ_ENTRY(ns_entry) link;
	struct spdk_nvme_qpair *qpair;
};

struct io_task {
    struct spdk_nvme_qpair *qpair;
    uint16_t opc;
    uint64_t slba;
    uint32_t nlb;
    void *buf;
};

/* variables for init nvme */
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};
static uint32_t g_queue_depth = 0;
/* variables for parse_args */
static bool g_input_file = false;
static bool g_report_zone = false;
static uint64_t g_zone_report_limit = 0;
static bool g_spdk_trace = false;
static const char *g_tpoint_group_name = NULL;
/* variables for io request */
static uint64_t g_num_io = 0;
static uint32_t outstanding_commands = 0;
/* info about nvme device */
static uint64_t g_ns_blk = 0;
static uint32_t g_block_byte = 0;
/* info about zone */
static bool g_zone = false;
static uint64_t g_num_zone = 0;
static uint64_t g_zone_sz_blk = 0;
static uint32_t g_max_open_zone = 0;
static uint32_t g_max_active_zone = 0;
static uint32_t g_max_append_byte = 0;

/* Underline a "line" with the given marker, e.g. print_uline("=", printf(...)); */
static void
print_uline(char marker, int line_len)
{
    for (int i = 1; i < line_len; ++i) {
        putchar(marker);
    }   
    putchar('\n');
}

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

    return ns_entry;
}
/* allocate io qpair & free io qpair end */

/* identify namespace start */
static void
identify_ns(struct ns_entry *ns_entry)
{
    if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
        g_zone = true;
    }

    print_uline('=', printf("\nNVMe Namespace Information\n"));
    g_block_byte = spdk_nvme_ns_get_sector_size(ns_entry->ns);
    g_ns_blk =  spdk_nvme_ns_get_num_sectors(ns_entry->ns);
    printf("%-20s: %lu (blocks)\n", "Size of namespace", g_ns_blk);
    printf("%-20s: %u (bytes)\n", "Size of LBA", g_block_byte);

    if (g_zone) {
        g_num_zone = spdk_nvme_zns_ns_get_num_zones(ns_entry->ns);
        g_zone_sz_blk = spdk_nvme_zns_ns_get_zone_size_sectors(ns_entry->ns);
        g_max_append_byte = spdk_nvme_zns_ctrlr_get_max_zone_append_size(ns_entry->ctrlr);
        g_max_open_zone = spdk_nvme_zns_ns_get_max_open_zones(ns_entry->ns);
        g_max_active_zone = spdk_nvme_zns_ns_get_max_active_zones(ns_entry->ns);
        printf("%-20s: %lu\n", "Number of Zone", g_num_zone);
        printf("%-20s: 0x%lx (blocks)\n", "Size of Zone",g_zone_sz_blk);
        printf("%-20s: %u (blocks)\n", "Max Zone Append Size", g_max_append_byte / g_block_byte);
        printf("%-20s: %u\n", "Max Open Zone", g_max_open_zone);
        printf("%-20s: %u\n", "Max Active Zone", g_max_active_zone);
    }
}
/* identify namespace end */

/* reset namespace start */
static void
reset_ns_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Reset namespace error - opc = 0x%x, slba = 0x%lx, nlb = %d, status = %s\n",
                 task->opc, task->slba, task->nlb, spdk_nvme_cpl_get_status_string(&cpl->status));
    }
    outstanding_commands--;
}

static void
reset_all_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
    struct io_task task;
    task.qpair = qpair;
    task.opc = SPDK_NVME_OPC_ZONE_MGMT_SEND;
    task.slba = 0;
    task.nlb = 0;

    outstanding_commands++;
    int err = spdk_nvme_zns_reset_zone(ns, qpair, 0, true, reset_ns_complete, &task);
    if (err) {
            fprintf(stderr, "Reset all zones failed\n");
            exit(1);
    }

    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
}

static void
write_all_zero(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
    struct io_task task;
    task.qpair = qpair;
    task.opc = SPDK_NVME_OPC_WRITE_ZEROES;

    uint64_t remain_ns_blk = g_ns_blk;
    uint64_t xfer_blk = spdk_nvme_ns_get_max_io_xfer_size(ns) / g_block_byte;
    uint64_t slba = 0;
    uint32_t lba_count = 0;

    for (slba = 0; slba < g_ns_blk ; slba += lba_count) {
        if (remain_ns_blk > xfer_blk) {
            lba_count = xfer_blk;
        } else {
            lba_count = remain_ns_blk;
        }

        task.slba = slba;
        task.nlb = lba_count;
        outstanding_commands++;
        int err = spdk_nvme_ns_cmd_write_zeroes(ns, qpair, slba, lba_count, reset_ns_complete, &task, 0);
        if (err) {
            fprintf(stderr, "Write all zeros failed\n");
            exit(1);
        }

        while (outstanding_commands) {
            spdk_nvme_qpair_process_completions(qpair, 0);
        }

        remain_ns_blk -= lba_count;
    }
}

static void
reset_ns(struct ns_entry *ns_entry)
{
    if (g_zone) {
        reset_all_zone(ns_entry->ns, ns_entry->qpair);
    } else {
        write_all_zero(ns_entry->ns, ns_entry->qpair);
    }

    printf("\nReset namespace complete.\n");
}
/* reset namespace end */

/* report zone start */
static void
zone_report_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;

    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Zone report error - slba = 0x%lx, status = %s\n", 
                task->slba, spdk_nvme_cpl_get_status_string(&cpl->status));
    }
    outstanding_commands--;
}

static void print_zns_zone(uint8_t *report, uint32_t index, uint32_t zdes)
{
    uint32_t zrs = sizeof(struct spdk_nvme_zns_zone_report);
    uint32_t zds = sizeof(struct spdk_nvme_zns_zone_desc);
    uint32_t zd_index = zrs + index * (zds + zdes);
    struct spdk_nvme_zns_zone_desc *desc = (struct spdk_nvme_zns_zone_desc *)(report + zd_index);

    printf("ZSLBA: 0x%-18" PRIx64 " ZCAP: 0x%-18" PRIx64 " WP: 0x%-18" PRIx64 " ZS: ", desc->zslba,
           desc->zcap, desc->wp);
    switch (desc->zs)
    {
    case SPDK_NVME_ZONE_STATE_EMPTY:
        printf("%-20s", "Empty");
        break;
    case SPDK_NVME_ZONE_STATE_IOPEN:
        printf("%-20s", "Implicit open");
        break;
    case SPDK_NVME_ZONE_STATE_EOPEN:
        printf("%-20s", "Explicit open");
        break;
    case SPDK_NVME_ZONE_STATE_CLOSED:
        printf("%-20s", "Closed");
        break;
    case SPDK_NVME_ZONE_STATE_RONLY:
        printf("%-20s", "Read only");
        break;
    case SPDK_NVME_ZONE_STATE_FULL:
        printf("%-20s", "Full");
        break;
    case SPDK_NVME_ZONE_STATE_OFFLINE:
        printf("%-20s", "Offline");
        break;
    default:
        printf("%-20s", "Reserved");
    }
    printf(" ZT: %-20s", (desc->zt == SPDK_NVME_ZONE_TYPE_SEQWR) ? "SWR" : "Reserved");
    // printf(" ZA: 0x%-18x\n", desc->za.raw);
    printf("\n");

    if (!desc->za.bits.zdev) {
        return;
    }
    for (int i = 0; i < (int)zdes; i += 8) {
        printf("zone_desc_ext[%d] : 0x%" PRIx64 "\n", i,
               *(uint64_t *)(report + zd_index + zds + i));
    }
}

static void report_zone(void)
{
    int rc = 0;
    struct ns_entry *ns_entry = TAILQ_FIRST(&g_namespaces);
    
    if (spdk_nvme_ns_get_csi(ns_entry->ns) != SPDK_NVME_CSI_ZNS) {
        return;
    }

    uint64_t zone_size_lba = spdk_nvme_zns_ns_get_zone_size_sectors(ns_entry->ns);
    uint64_t total_zones = spdk_nvme_zns_ns_get_num_zones(ns_entry->ns);

    /* specify namespace and allocate io qpair for the namespace */
    ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
    if (ns_entry->qpair == NULL) {
        printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
        return;
    }

    const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns_entry->ns);
    const struct spdk_nvme_zns_ns_data *nsdata_zns = spdk_nvme_zns_ns_get_data(ns_entry->ns);

    //uint32_t zrs = sizeof(struct spdk_nvme_zns_zone_report);
    //uint32_t zds = sizeof(struct spdk_nvme_zns_zone_desc);

    uint32_t format_index = spdk_nvme_ns_get_format_index(nsdata);
    size_t zdes = nsdata_zns->lbafe[format_index].zdes * 64;

    size_t report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns_entry->ns);
    uint8_t *report_buf = calloc(1, report_bufsize);
    if (!report_buf) {
        printf("Zone report allocation failed!\n");
        exit(1);
    }

    uint64_t zones_to_print = g_zone_report_limit ? spdk_min(total_zones, (uint64_t)g_zone_report_limit) : \
	                total_zones;
    
    print_uline('=', printf("\nNVMe ZNS Zone Report (first %zu of %zu)\n", zones_to_print, total_zones));

    outstanding_commands = 0;
    uint64_t handled_zones = 0;
    uint64_t slba = 0;
    struct io_task task;
    while (handled_zones < zones_to_print) {
        task.qpair = ns_entry->qpair;
        task.opc = SPDK_NVME_OPC_ZONE_MGMT_RECV;    
        task.slba = slba;
        task.nlb = 0;

        memset(report_buf, 0, report_bufsize);

        if (zdes) {
            outstanding_commands++;
            //uint64_t max_zones_per_buf = (report_bufsize - zrs) / (zds + zdes);
            rc = spdk_nvme_zns_ext_report_zones(ns_entry->ns, ns_entry->qpair, report_buf, report_bufsize,
                            slba, SPDK_NVME_ZRA_LIST_ALL, true, zone_report_completion, &task);
        } else {
            outstanding_commands++;
            //uint64_t max_zones_per_buf = (report_bufsize - zrs) / zds;
            rc = spdk_nvme_zns_report_zones(ns_entry->ns, ns_entry->qpair, report_buf, report_bufsize,
                            slba, SPDK_NVME_ZRA_LIST_ALL, true, zone_report_completion, &task);
        }

        if (rc) {
            fprintf(stderr, "Report zones failed\n");
            exit(1);
        }

        while (outstanding_commands) {
            spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
        }

        for (int i = 0; handled_zones < zones_to_print; i++) {
            print_zns_zone(report_buf, i, zdes);
            slba += zone_size_lba;
            handled_zones++;
        }
        printf("\n");
    }

    free(report_buf);
    spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
}
/* report zone end */

/* replay workload start */
static void
replay_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)cb_arg;
    if (spdk_nvme_cpl_is_error(cpl)) {
        spdk_nvme_qpair_print_completion(task->qpair, (struct spdk_nvme_cpl *)cpl);
        fprintf(stderr, "Replay error - opc = 0x%x, slba = 0x%lx, nlb = %d, status = %s\n",
                 task->opc, task->slba, task->nlb, spdk_nvme_cpl_get_status_string(&cpl->status));
    }

    if (task) {
        if (task->buf) {
            spdk_free(task->buf);
        }
        free(task);
    }

    outstanding_commands--;
}

static int
process_zns_replay(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, struct trace_io_entry *d)
{
    int err = 0;
    uint64_t slba = (uint64_t)d->cdw10 | ((uint64_t)d->cdw11 & UINT32BIT_MASK) << 32;
    uint32_t nlb = (uint32_t)(d->cdw12 & UINT16BIT_MASK) + 1;
    uint64_t zslba = (slba / g_zone_sz_blk) * g_zone_sz_blk;
    
    /* allocate data buffers for SPDK NVMe I/O operations */
    char *replay_buf = (char *)spdk_zmalloc(nlb * g_block_byte, g_block_byte,
                             NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (!replay_buf) {
        perror("Fail to malloc replay_buf");
        exit(1);
    }   
    
    /* read write replay */
    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->opc = d->opc;
    task->nlb = nlb;
    task->buf = replay_buf;

    switch (d->opc) {
    case SPDK_NVME_OPC_READ:
    case SPDK_NVME_OPC_COMPARE:
        task->slba = slba;
        memset(replay_buf, 0, (size_t)nlb * g_block_byte);
        g_num_io++;
        outstanding_commands++;
        err = spdk_nvme_ns_cmd_read(ns, qpair, replay_buf, slba, nlb, replay_complete, task, 0);
        break;
    case SPDK_NVME_OPC_WRITE:
    case SPDK_NVME_OPC_ZONE_APPEND:
        task->slba = zslba;
        snprintf(replay_buf, (size_t)nlb * g_block_byte, "%s", "Hello World!\n");
        g_num_io++;
        outstanding_commands++;
        err = spdk_nvme_zns_zone_append(ns, qpair, replay_buf, zslba, nlb, replay_complete, task, 0);
        break;
    case SPDK_NVME_OPC_ZONE_MGMT_SEND:
        task->slba = zslba;
        bool select_all = (d->cdw13 & (uint32_t)1 << 8) ? true : false;
        uint8_t zone_action = (uint8_t)(d->cdw13 & UINT8BIT_MASK);
        if (zone_action == SPDK_NVME_ZONE_OPEN) {
            g_num_io++;
            outstanding_commands++;
            err = spdk_nvme_zns_open_zone(ns, qpair, zslba, select_all, replay_complete, task);
        } else if (zone_action == SPDK_NVME_ZONE_CLOSE) {
            g_num_io++;
            outstanding_commands++;
            err = spdk_nvme_zns_close_zone(ns, qpair, zslba, select_all, replay_complete, task);
        } else if (zone_action == SPDK_NVME_ZONE_FINISH) {
            g_num_io++;
            outstanding_commands++;
            err = spdk_nvme_zns_finish_zone(ns, qpair, zslba, select_all, replay_complete, task);
        } else if (zone_action == SPDK_NVME_ZONE_RESET) {
            g_num_io++;
            outstanding_commands++;
            err = spdk_nvme_zns_reset_zone(ns, qpair, zslba, select_all, replay_complete, task);
        } else if (zone_action == SPDK_NVME_ZONE_OFFLINE) {
            g_num_io++;
            outstanding_commands++;
            err = spdk_nvme_zns_offline_zone(ns, qpair, zslba, select_all, replay_complete, task);
        }
        break;
    default:
        break;
    }

    if (err) {
            fprintf(stderr, "Replay failed, err = %d\n", err);
    }

/*
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
    //printf("opc = 0x%x g_num_io = %ld\n", d->opc, g_num_io);
    spdk_free(replay_buf);
*/
    return err;
}

static int
process_replay(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, struct trace_io_entry *d)
{
    int err = 0;
    uint64_t slba = (uint64_t)d->cdw10 | ((uint64_t)d->cdw11 & UINT32BIT_MASK) << 32;
    uint32_t nlb = (uint32_t)(d->cdw12 & UINT16BIT_MASK) + 1;

    /* allocate data buffers for SPDK NVMe I/O operations */
    char *replay_buf = (char *)spdk_zmalloc(nlb * g_block_byte, g_block_byte,
                             NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (!replay_buf) {
        perror("Fail to malloc replay_buf");
        exit(1); 
    }

    /* read write replay */
    struct io_task *task = (struct io_task *)malloc(sizeof(struct io_task));
    task->qpair = qpair;
    task->opc = d->opc;
    task->slba = slba;
    task->nlb = nlb;
    task->buf = replay_buf;

    switch (d->opc) {
    case SPDK_NVME_OPC_READ:
    case SPDK_NVME_OPC_COMPARE:
        memset(replay_buf, 0, (size_t)nlb * g_block_byte);
        g_num_io++;
        outstanding_commands++;
        err = spdk_nvme_ns_cmd_read(ns, qpair, replay_buf, slba, nlb, replay_complete, task, 0);
        break;
    case SPDK_NVME_OPC_WRITE:
        snprintf(replay_buf, (size_t)nlb * g_block_byte, "%s", "Hello World!\n");
        g_num_io++;
        outstanding_commands++;
        err = spdk_nvme_ns_cmd_write(ns, qpair, replay_buf, slba, nlb, replay_complete, task, 0);
        break;
    case SPDK_NVME_OPC_WRITE_ZEROES:
        g_num_io++;
        outstanding_commands++;
        err = spdk_nvme_ns_cmd_write_zeroes(ns, qpair, slba, nlb, replay_complete, task, 0);
        break;
    default:
        break; 
    }

    if (err) {
            fprintf(stderr, "Replay failed, err = %d\n", err);
    }
/*
    while (outstanding_commands) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
    
    spdk_free(replay_buf);
*/
    return err;
}
/* replay workload end */

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("%s <options>\n", program_name);
    printf("\n");
    printf(" -f, specify the input file which generated by trace_io_record\n");
    printf(" -z, to display zone. 0 indicate displaying all zone\n");
    printf(" -q, Queue depth between 1 to 256. If non specify, default queue depth is 256.\n");
    spdk_trace_mask_usage(stdout, "-e");
}

static int
parse_args(int argc, char **argv, char *file_name, size_t file_name_size)
{
    int op;

    while ((op = getopt(argc, argv, "f:z:e:q:")) != -1) {
        switch (op) {
        case 'f':
            g_input_file = true;
            snprintf(file_name, file_name_size, "%s", optarg);
            break;
        case 'z':
            g_report_zone = true;
            g_zone_report_limit = atoi(optarg);
            break;
        case 'e':
            g_spdk_trace = true;
            g_tpoint_group_name = optarg;
            break;
        case 'q':
            g_queue_depth = atoi(optarg);
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
    char input_file_name[68];

    /* Get the input file name */
    int rc = parse_args(argc, argv, input_file_name, sizeof(input_file_name));
    if (rc != 0) {
        return rc;
    }

    if (input_file_name == NULL || !g_input_file) {
        fprintf(stderr, "-f input file must be specified\n");
        exit(1);
    }

    /* Get trid */
    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

    /* Initialize env */
    spdk_env_opts_init(&env_opts);
    env_opts.name = "trace_replayer";
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

    /* Register ctrlr & ns */
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
    struct ns_entry *ns_entry = alloc_qpair(&env_opts);
    if (!ns_entry) {
        fprintf(stderr, "Failed to alloc_qpair()\n");
        return -1;
    }

    /* Identify namespace */
    identify_ns(ns_entry);
    
    /* Reset namespace */
    reset_ns(ns_entry);

    /* Read file and replay workload */
    FILE *fptr = fopen(input_file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open input file %s\n", input_file_name);
        return -1; 
    }
    fseek(fptr, 0, SEEK_END);
    size_t file_size = ftell(fptr);
    rewind(fptr);
    size_t total_entry = file_size / sizeof(struct trace_io_entry); /* Get number of requests */
    //printf("Total number of request = %ld\n", total_entry >> 1);
   
    /* Workload repaly start */
    print_uline('=', printf("\nWorkload Replay Information\n"));
    uint64_t start_tsc = spdk_get_ticks();
 
    size_t remain_entry = total_entry;
    while (!feof(fptr) && remain_entry) {
        size_t buffer_entry = (remain_entry > ENTRY_MAX) ? ENTRY_MAX : remain_entry;
        remain_entry -= buffer_entry;
        struct trace_io_entry buffer[buffer_entry]; /* Allocate buffer for read file */
        size_t read_entry = fread(&buffer, sizeof(struct trace_io_entry), buffer_entry, fptr);
        if (buffer_entry != read_entry) {
                fprintf(stderr, "Fail to read input file\n");
                free_qpair(ns_entry->qpair);
                fclose(fptr);
                break;
        }

        for (size_t i = 0; i < read_entry; i++) {
            if (strcmp(buffer[i].tpoint_name, "NVME_IO_COMPLETE") == 0) {
                continue;
            }

            for (; outstanding_commands >= g_queue_depth; spdk_nvme_qpair_process_completions(ns_entry->qpair, 0));

            if (g_zone) {
                rc = process_zns_replay(ns_entry->ns, ns_entry->qpair, &buffer[i]);
            } else {
                rc = process_replay(ns_entry->ns, ns_entry->qpair, &buffer[i]);
            }

            if (rc != 0) {
                fprintf(stderr, "Replay workload failed\n");
                free_qpair(ns_entry->qpair);
                fclose(fptr);
                return rc;
            }
        }
        for (; outstanding_commands; spdk_nvme_qpair_process_completions(ns_entry->qpair, 0));
    }
    /* Workload repaly finish */
    uint64_t end_tsc = spdk_get_ticks();

    fclose(fptr);

    uint64_t tsc_diff = end_tsc - start_tsc;
    uint64_t tsc_rate = spdk_get_ticks_hz();
    float sec_diff = tsc_diff * 1000 / tsc_rate;

    printf("%-16s: %15ld \n", "Requests number", g_num_io);
    printf("%-16s: %15.3f (ms) \n", "Total time", sec_diff);
    
    /* Free io qpair after workload replay */
    free_qpair(ns_entry->qpair);  

    /* Report zone */
    if (g_report_zone) {
        report_zone();
    }
 
    exit:
    cleanup();
    spdk_env_fini();
    return rc;
}
