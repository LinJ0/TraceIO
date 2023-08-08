#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/nvme_spec.h"
#include "../include/trace_io.h"

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
/* variables for init nvme */
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};
/* variables for parse_args */
static bool g_print_tsc = false;
static bool g_print_trace = false;
static bool g_input_file = false;
static bool g_print_rwblock = false;
static bool g_print_rwzone = false;
/* info about nvme device & zone*/
static bool g_zone = false;     /* namespace is ZNS */
static uint64_t g_ns_block = 0; /* number of blocks in a namespace */
static uint64_t g_ns_zone = 0;  /* number of zones in a namespace */
static size_t g_max_transfer_block = 0;
static uint64_t g_zone_size_lba = 0;

static float
get_us_from_tsc(uint64_t tsc, uint64_t tsc_rate)
{
    return ((float)tsc) * 1000 * 1000 / tsc_rate;
}

/* Underline a "line" with the given marker, e.g. print_uline("=", printf(...)); */
static void
print_uline(char marker, int line_len)
{
    for (int i = 1; i < line_len; ++i) {
        putchar(marker);
    }   
    putchar('\n');
}

/* initialize NVMe controllers start */
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
/* initialize NVMe controllers end */

/* trace analysis start */
static uint64_t g_read_cnt = 0, g_write_cnt = 0;

static float
rw_ratio(uint64_t read, uint64_t write)
{
    float ratio = 0.0;
    return ratio = (read + write) ? (read * 100) / (read + write) : 0;
}

static int
iosize_rw_counter(uint8_t opc, uint32_t nlb, uint32_t *r_iosize, uint32_t *w_iosize)
{
    switch (opc) {
    case SPDK_NVME_OPC_READ:
    case SPDK_NVME_OPC_COMPARE: 
        g_read_cnt++;
        r_iosize[nlb]++;
        break;
    case SPDK_NVME_OPC_WRITE:
    case SPDK_NVME_OPC_ZONE_APPEND:
    case SPDK_NVME_OPC_WRITE_ZEROES:
        g_write_cnt++;
        w_iosize[nlb]++;
        break;
    case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
    case SPDK_NVME_OPC_COPY:
    case SPDK_NVME_OPC_VERIFY:
    case SPDK_NVME_OPC_DATASET_MANAGEMENT:
    case SPDK_NVME_OPC_FLUSH:
    case SPDK_NVME_OPC_ZONE_MGMT_RECV:
    case SPDK_NVME_OPC_ZONE_MGMT_SEND:
    case SPDK_NVME_OPC_RESERVATION_REGISTER: 
    case SPDK_NVME_OPC_RESERVATION_REPORT:
    case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
    case SPDK_NVME_OPC_RESERVATION_RELEASE:
        break;
    default:
        return -1;
    }
    return 0;
}

struct latency {
	uint64_t val;
	TAILQ_ENTRY(latency) link;
};

static TAILQ_HEAD(latency_list, latency) g_latency_sum = TAILQ_HEAD_INITIALIZER(g_latency_sum);
static uint64_t g_tsc_rate = 0;
static uint64_t g_latency_tsc_min = 0, g_latency_tsc_max = 0, g_latency_tsc_avg = 0;
static float g_latency_us_min = 0.0, g_latency_us_max = 0.0, g_latency_us_avg = 0.0;

static void
latency_min_max(uint64_t tsc_sc_time, uint64_t tsc_rate)
{
    g_latency_tsc_max = (tsc_sc_time > g_latency_tsc_max) ? tsc_sc_time : g_latency_tsc_max;

    if (!g_latency_tsc_min || tsc_sc_time < g_latency_tsc_min) {
        g_latency_tsc_min = tsc_sc_time;
    } else {
        g_latency_tsc_min = g_latency_tsc_min;
    }
    g_latency_us_max = get_us_from_tsc(g_latency_tsc_max, tsc_rate);
    g_latency_us_min = get_us_from_tsc(g_latency_tsc_min, tsc_rate);
}

static void
latency_total(uint64_t tsc_sc_time)
{
    if (TAILQ_EMPTY(&g_latency_sum)) {
        struct latency *latency_entry = (struct latency *)malloc(sizeof(struct latency));
        if (latency_entry == NULL) {
            fprintf(stderr, "Fail to allocate memory for latency entry\n");
            return;
        }
        TAILQ_INSERT_TAIL(&g_latency_sum, latency_entry, link);
        TAILQ_LAST(&g_latency_sum, latency_list)->val = 0;
    }
    if (tsc_sc_time < UINT64_MAX - TAILQ_LAST(&g_latency_sum, latency_list)->val) {
        TAILQ_LAST(&g_latency_sum, latency_list)->val += tsc_sc_time;
    } else {
        struct latency *latency_entry = (struct latency *)malloc(sizeof(struct latency));
        if (latency_entry == NULL) {
            fprintf(stderr, "Fail to allocate memory for latency entry\n");
            struct latency *cur, *tmp;
            TAILQ_FOREACH_SAFE(cur, &g_latency_sum, link, tmp) {
                TAILQ_REMOVE(&g_latency_sum, cur, link);
                free(cur);
            }
            return;
        }
        TAILQ_INSERT_TAIL(&g_latency_sum, latency_entry, link);
        TAILQ_LAST(&g_latency_sum, latency_list)->val += tsc_sc_time;
    }
}

static void
latency_avg(int number_of_io)
{
    if (number_of_io == 0) 
        return;
    
    if (TAILQ_EMPTY(&g_latency_sum)) {
        fprintf(stderr, "No latency entry\n");
        return;
    }

    struct latency *cur, *tmp;
    TAILQ_FOREACH_SAFE(cur, &g_latency_sum, link, tmp) {
        g_latency_tsc_avg += (float)cur->val / number_of_io;
        TAILQ_REMOVE(&g_latency_sum, cur, link);
        free(cur);
    }
    
    g_latency_us_avg = get_us_from_tsc(g_latency_tsc_avg, g_tsc_rate);
}

static int
block_counter(uint8_t opc, uint64_t slba, uint16_t nlb, uint16_t *r_block, uint16_t *w_block)
{
    int rc = 0;
    uint64_t idx = slba;

    switch (opc) {
    case SPDK_NVME_OPC_READ:
    case SPDK_NVME_OPC_COMPARE: 
        for (int i = 0; i < nlb; i++) {
            r_block[idx + i]++;
        }
        break;        
    case SPDK_NVME_OPC_WRITE:
    case SPDK_NVME_OPC_ZONE_APPEND:
    case SPDK_NVME_OPC_WRITE_ZEROES:
        for (int i = 0; i < nlb; i++) {
            w_block[idx + i]++;
        }
        break;
    case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
    case SPDK_NVME_OPC_COPY:
    case SPDK_NVME_OPC_VERIFY:
    case SPDK_NVME_OPC_DATASET_MANAGEMENT:
    case SPDK_NVME_OPC_FLUSH:
    case SPDK_NVME_OPC_ZONE_MGMT_RECV:
    case SPDK_NVME_OPC_ZONE_MGMT_SEND:
    case SPDK_NVME_OPC_RESERVATION_REGISTER: 
    case SPDK_NVME_OPC_RESERVATION_REPORT:
    case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
    case SPDK_NVME_OPC_RESERVATION_RELEASE:
        break;
    default:
        rc = 1;
        break; 
    }   
    return rc;    
}

static int
zone_counter(uint8_t opc, uint64_t slba, uint16_t *r_zone, uint16_t *w_zone)
{
    int rc = 0;
    uint64_t zidx = slba / g_zone_size_lba;

    switch (opc) {
    case SPDK_NVME_OPC_READ:
    case SPDK_NVME_OPC_COMPARE:
        r_zone[zidx]++;
        break;
    case SPDK_NVME_OPC_WRITE:
    case SPDK_NVME_OPC_ZONE_APPEND:
    case SPDK_NVME_OPC_WRITE_ZEROES:
        w_zone[zidx]++;
        break;
    case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
    case SPDK_NVME_OPC_COPY:
    case SPDK_NVME_OPC_VERIFY:
    case SPDK_NVME_OPC_DATASET_MANAGEMENT:
    case SPDK_NVME_OPC_FLUSH:
    case SPDK_NVME_OPC_ZONE_MGMT_RECV:
    case SPDK_NVME_OPC_ZONE_MGMT_SEND:
    case SPDK_NVME_OPC_RESERVATION_REGISTER:
    case SPDK_NVME_OPC_RESERVATION_REPORT:
    case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
    case SPDK_NVME_OPC_RESERVATION_RELEASE:
        break;
    default:
        rc = 1;
        break;
    }
    return rc;
}

static uint64_t g_end_tsc = 0;
static uint64_t g_req_num = 0;
static float
iops(uint64_t end_tsc, uint64_t req_num)
{
    float IOPS = 0.0;
    if (req_num ==0 || end_tsc == 0) {
        return IOPS;
    }
    float end_sec = get_us_from_tsc(end_tsc, g_tsc_rate) / (1000 * 1000);
    return IOPS = (float)req_num / end_sec;
}

static int
process_analysis_round1(struct trace_io_entry *d, uint32_t *r_iosize, uint32_t *w_iosize)
{
    if (!g_tsc_rate) { /* for calculate g_latency_us_avg */
        g_tsc_rate = d->tsc_rate;
    }

    int rc;
    uint32_t nlb = d->cdw12 & UINT16BIT_MASK;
    if (strcmp(d->tpoint_name, "NVME_IO_SUBMIT") == 0) {
        rc = iosize_rw_counter(d->opc, nlb, r_iosize, w_iosize);    /* for calculate request size */
        if (rc) {
            printf("Unknown Opcode\n");
            return rc;
        }
    }

    if (strcmp(d->tpoint_name, "NVME_IO_COMPLETE") == 0) {
        g_end_tsc = d->tsc_timestamp;                               /* for calculate IOPS */
        latency_min_max(d->tsc_sc_time, d->tsc_rate);               /* for calculate latency (min & max) */
        latency_total(d->tsc_sc_time);                              /* for calculate latency (avg) */
    }

    return rc;
}

static int
process_analysis_round2(struct trace_io_entry *d, uint16_t *r_blk, uint16_t *w_blk,  uint16_t *r_zone, uint16_t *w_zone)
{
    int rc;
    uint64_t slba = 0;    
    if (strcmp(d->tpoint_name, "NVME_IO_SUBMIT") == 0 && d->opc != SPDK_NVME_OPC_DATASET_MANAGEMENT) {
        slba = (uint64_t)d->cdw10 | ((uint64_t)d->cdw11 & UINT32BIT_MASK) << 32;

        if (d->opc != SPDK_NVME_OPC_ZONE_MGMT_RECV && 
            d->opc != SPDK_NVME_OPC_ZONE_MGMT_SEND &&
            d->opc != SPDK_NVME_OPC_COPY) {
            uint32_t nlb = (d->cdw12 & UINT16BIT_MASK) + 1;
            rc = block_counter(d->opc, slba, nlb, r_blk, w_blk);      /* for calculate r/w # in a block */
            if (rc) {
                printf("Count block read / write fail\n");
                return rc;
            }  
            
        }

        if (g_zone) {
            rc = zone_counter(d->opc, slba, r_zone, w_zone);          /* for calculate r/w # in a zone */
            if (rc) {
                printf("Count block read / write fail\n");
                return rc;
            }
        }
    }
    return rc;
}
/* trace analysis end */

/* print trace start */
static const char *
format_argname(const char *name)
{
    static char namebuf[16];

    snprintf(namebuf, sizeof(namebuf), "%s: ", name);
    return namebuf;
}

static void
print_ptr(const char *arg_string, uint64_t arg)
{
    printf("%-7.7s0x%-16jx ", format_argname(arg_string), arg);
}

static void
print_uint64(const char *arg_string, uint64_t arg)
{
	printf("%-7.7s%-16jd ", format_argname(arg_string), arg);
}

static void
print_float(const char *arg_string, float arg)
{
    printf("%-7.7s%-13.3f ", format_argname(arg_string), arg);
}

static void
set_zone_act_name (uint8_t opc, uint64_t zone_act, const char **zone_act_name) 
{
    if (opc ==  SPDK_NVME_OPC_ZONE_MGMT_SEND) {
        switch (zone_act) {
        case SPDK_NVME_ZONE_CLOSE:
            *zone_act_name = "CLOSE ZONE";
            break;
        case SPDK_NVME_ZONE_FINISH:
            *zone_act_name = "FINISH ZONE";
            break;
        case SPDK_NVME_ZONE_OPEN:
            *zone_act_name = "OPEN ZONE";
            break;
        case SPDK_NVME_ZONE_RESET:
            *zone_act_name = "RESET ZONE";
            break;
        case SPDK_NVME_ZONE_OFFLINE:
            *zone_act_name = "OFFLINE ZONE";
            break;
        case SPDK_NVME_ZONE_SET_ZDE:
            *zone_act_name = "SET ZONE DESC";
            break;
        default:
            break;
        }
    } else if (opc == SPDK_NVME_OPC_ZONE_MGMT_RECV){
        switch (zone_act) {
        case SPDK_NVME_ZONE_REPORT:
            *zone_act_name = "REPORT ZONE";
            break;
        case SPDK_NVME_ZONE_EXTENDED_REPORT:
            *zone_act_name = "EXT REPORT ZONE";
            break;
        default:
            break;
        }
    } else {
        *zone_act_name = "unknown";
    }
}

static void
set_opc_name(uint64_t opc, const char **opc_name)
{
    switch (opc) {
    case SPDK_NVME_OPC_FLUSH:
        *opc_name = "FLUSH";
        break;
    case SPDK_NVME_OPC_WRITE:
        *opc_name = "WRITE";
        break;
    case SPDK_NVME_OPC_READ:
        *opc_name = "READ";
        break;
    case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
        *opc_name = "WRITE UNCORRECTABLE";
        break;
    case SPDK_NVME_OPC_COMPARE:
        *opc_name = "COMPARE";
        break;
    case SPDK_NVME_OPC_WRITE_ZEROES:
        *opc_name = "WRITE ZEROES";
        break;
    case SPDK_NVME_OPC_DATASET_MANAGEMENT:
        *opc_name = "DATASET MGMT";
        break;
    case SPDK_NVME_OPC_VERIFY:
        *opc_name = "VERIFY";
        break;
    case SPDK_NVME_OPC_RESERVATION_REGISTER: 
        *opc_name = "RESERVATION REGISTER";
        break; 
    case SPDK_NVME_OPC_RESERVATION_REPORT: 
        *opc_name = "RESERVATION REPORT";
        break;
    case SPDK_NVME_OPC_RESERVATION_ACQUIRE: 
        *opc_name = "RESERVATION ACQUIRE";
        break;
    case SPDK_NVME_OPC_RESERVATION_RELEASE:
        *opc_name = "RESERVATION RELEASE";
        break;
    case SPDK_NVME_OPC_COPY:
        *opc_name = "COPY";
        break;
    case SPDK_NVME_OPC_ZONE_APPEND:
        *opc_name = "ZONE APPEND";
        break;
    case SPDK_NVME_OPC_ZONE_MGMT_SEND:
        *opc_name = "ZONE MGMT SEND";
        break;
    case SPDK_NVME_OPC_ZONE_MGMT_RECV:
        *opc_name = "ZONE MGMT RECV";
        break;
    default:
        *opc_name = "unknown";
        break;
    }
}

static void
set_opc_flags(uint8_t opc, bool *cdw10, bool *cdw11, bool *cdw12, bool *cdw13)
{
    switch (opc) {
    case SPDK_NVME_OPC_ZONE_MGMT_RECV:
        *cdw10 = true;
        *cdw11 = true;
        *cdw12 = true;
        *cdw13 = true; 
        break;
    case SPDK_NVME_OPC_WRITE:
    case SPDK_NVME_OPC_READ:
    case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
    case SPDK_NVME_OPC_COMPARE:
    case SPDK_NVME_OPC_WRITE_ZEROES:
    case SPDK_NVME_OPC_VERIFY:
    case SPDK_NVME_OPC_COPY:
    case SPDK_NVME_OPC_ZONE_APPEND:
        *cdw10 = true;
        *cdw11 = true;
        *cdw12 = true;
        break;
    case SPDK_NVME_OPC_DATASET_MANAGEMENT:
        *cdw10 = true;
        break;
    case SPDK_NVME_OPC_ZONE_MGMT_SEND:
        *cdw10 = true;
        *cdw11 = true;
        *cdw13 = true;
        break;
    case SPDK_NVME_OPC_FLUSH:
    case SPDK_NVME_OPC_RESERVATION_REGISTER: 
    case SPDK_NVME_OPC_RESERVATION_REPORT:
    case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
    case SPDK_NVME_OPC_RESERVATION_RELEASE:
        break;
    default:
        break;
    }
}

static int
process_print_trace(struct trace_io_entry *d)
{
    int     rc = 0;
    const char *opc_name;
    const char *zone_act_name;
    bool cdw10 = false;
    bool cdw11 = false;
    bool cdw12 = false;
    bool cdw13 = false;
    uint64_t slba = 0;

    /* print lcore & tsc_base (us) & tpoint name & object id */
    float timestamp_us = get_us_from_tsc(d->tsc_timestamp, d->tsc_rate);
    printf("core%2d: %16.3f  ", d->lcore, timestamp_us);
    
    if (g_print_tsc) {
        printf("(%10ju)  ", d->tsc_timestamp);
    }
    printf("%-20s ", d->tpoint_name);
    print_ptr("object", d->obj_id);

    
    /* print process nvme submit / complete */
    if (strcmp(d->tpoint_name, "NVME_IO_SUBMIT") && strcmp(d->tpoint_name, "NVME_IO_COMPLETE")) {
        rc = 1;
    }

    if (strcmp(d->tpoint_name, "NVME_IO_SUBMIT") == 0) {
        set_opc_name(d->opc, &opc_name);
        set_opc_flags(d->opc, &cdw10, &cdw11, &cdw12, &cdw13);
        printf("%-20s ", opc_name);
        print_uint64("cid", d->cid);
        print_ptr("nsid", d->nsid);

        if (cdw10) { /* slba_l64b | nr_8b (dataset_mgmt) */
            if (d->opc != SPDK_NVME_OPC_DATASET_MANAGEMENT)
                slba = (uint64_t)d->cdw10;
            else 
                print_ptr("nr", d->cdw10 & UINT8BIT_MASK);
        }

        if (cdw11) { /* slba_h64b */
            slba |= ((uint64_t)d->cdw11 & UINT32BIT_MASK) << 32;
            
            if (d->opc != SPDK_NVME_OPC_ZONE_APPEND) {
                print_ptr("slba", slba);
            } else {
                print_ptr("zslba", slba);
            }
        }

        if (cdw12) { /* nlb_16b | nr_8b (copy) | ndw_32b (z_mgmt_recv) */
            if (d->opc == SPDK_NVME_OPC_COPY)
                print_uint64("range", (d->cdw12 & UINT8BIT_MASK) + 1);
            else if (d->opc == SPDK_NVME_OPC_ZONE_MGMT_RECV)
                print_uint64("dword", (d->cdw12 & UINT32BIT_MASK) + 1);
            else
                print_uint64("block", (d->cdw12 & UINT16BIT_MASK) + 1);
        }

        if (cdw13) { /* zsa_8b || zra_8b */
            set_zone_act_name(d->opc, d->cdw13 & UINT8BIT_MASK, &zone_act_name);
            printf("%-20.20s ", zone_act_name);
        }
        printf("\n");
    }
    
    if (strcmp(d->tpoint_name, "NVME_IO_COMPLETE") == 0) {
        if (d->tsc_sc_time) {
            float sctime_us = get_us_from_tsc(d->tsc_sc_time, d->tsc_rate);
            print_float("time", sctime_us);
        }

        print_uint64("cid", d->cid);
        print_ptr("comp", d->cpl & (uint64_t)0x1);
        print_ptr("status", (d->cpl >> 1) & (uint64_t)0x7FFF);
        printf("\n");
    }

    return rc;
}
/* print trace end */

/* Get namespace data start */
static void
get_ns_info(void)
{
    struct ns_entry *ns_entry = TAILQ_FIRST(&g_namespaces);
    
    if (spdk_nvme_ns_get_csi(ns_entry->ns) != SPDK_NVME_CSI_ZNS) {
        return;
    }

    const struct spdk_nvme_ns_data *ndata = spdk_nvme_ns_get_data(ns_entry->ns);
    g_ns_block = ndata->ncap;
    g_max_transfer_block = spdk_nvme_ns_get_max_io_xfer_size(ns_entry->ns);

    if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
        g_zone = true;
        g_zone_size_lba = spdk_nvme_zns_ns_get_zone_size_sectors(ns_entry->ns);
        g_ns_zone = spdk_nvme_zns_ns_get_num_zones(ns_entry->ns);
    }

}
/* Get namespace data end */

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("   %s <options>\n", program_name);
    printf("\n");
    printf("         '-f' specify the input file which generated by trace_io_record\n");
    printf("         '-d' to display each event\n");
    printf("         '-t' to display TSC for each event\n");
    printf("         '-b' to display anzlysis result of r/w in a block\n");
    printf("         '-z' to display anzlysis result of r/w in a zone\n");
}

static int
parse_args(int argc, char **argv, char *file_name, size_t file_name_size)
{
    int op;

    while ((op = getopt(argc, argv, "f:dtbz")) != -1) {
        switch (op) {
        case 'f':
            g_input_file = true;
            snprintf(file_name, file_name_size, "%s", optarg);
            break;
        case 'd':
            g_print_trace = true;
            break;
        case 'b':
            g_print_rwblock = true;
            break;
        case 'z':
            g_print_rwzone = true;
            break;
        case 't':
            g_print_tsc = true;
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
    int rc = 0;    
    char input_file_name[68];
    rc = parse_args(argc, argv, input_file_name, sizeof(input_file_name));
    if (rc != 0) {
        return rc;
    }

    if (!g_print_trace && g_print_tsc) {
        fprintf(stderr, "-t must be used with -d \n");
        exit(1);
    }

    if (input_file_name == NULL || !g_input_file) {
        fprintf(stderr, "-f input file must be specified\n");
        exit(1);
    }

    /* Initialize env */
    struct spdk_env_opts env_opts;
    spdk_env_opts_init(&env_opts);
    env_opts.name = "trace_analyzer";
    rc = spdk_env_init(&env_opts);
    if (rc < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        return rc; 
    }   

    /* Read input file */
    FILE *fptr = NULL;
    fptr = fopen(input_file_name, "rb");
    if (fptr == NULL) {
            fprintf(stderr, "Failed to open input file %s\n", input_file_name);
            return -1; 
    }
    fseek(fptr, 0, SEEK_END);
    size_t file_size = ftell(fptr);
    rewind(fptr);
    size_t total_entry = file_size / sizeof(struct trace_io_entry);
    g_req_num = total_entry >> 1; /* Get number of requests */
    //printf("Total number of request = %ld\n", g_req_num);

    /* Print trace */
    if (g_print_trace) {
        print_uline('=', printf("\nPrint I/O Trace\n"));

        size_t remain_entry = total_entry;
        
        while (!feof(fptr) && remain_entry) {

            size_t buffer_entry = (remain_entry > ENTRY_MAX) ? ENTRY_MAX : remain_entry;
            remain_entry -= buffer_entry;
            struct trace_io_entry buffer[buffer_entry]; /* Allocate buffer for read file */
            size_t read_entry = fread(&buffer, sizeof(struct trace_io_entry), buffer_entry, fptr);
            if (buffer_entry != read_entry) {
                fprintf(stderr, "Fail to read input file\n");
            }

            for (size_t i = 0; i < read_entry; i++) {
                rc = process_print_trace(&buffer[i]);
                if (rc != 0) {
                    fprintf(stderr, "Parse error\n");
                    fclose(fptr);
                    return rc;
                 }
            }
        }
    }
    printf("\n");

    /* In order to get namespace information, we need to initialize NVMe controller */
    /* Get trid */
    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

    /* Register ctrlr & register ns */
    printf("Initializing NVMe Controllers\n");
    
    rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed\n");
        cleanup();
    }
    
    if (TAILQ_EMPTY(&g_controllers)) {
        fprintf(stderr, "no NVMe controllers found\n");
        cleanup();
    }
    printf("Initialization complete.\n");

    /* Get namespace data */
    get_ns_info();
    cleanup();

    /*
     * Trace analysis round 1: 
     * 1. Latency in tsc (time stamp counter) and in us
     * 2. IOPS
     * 3. Total number of read write
     * 4. IO request size
     */
    uint32_t *r_iosize = (uint32_t *)malloc(g_max_transfer_block * sizeof(uint32_t));
    if (!r_iosize) {
        fprintf(stderr, "Fall to allocate memory for r_iosize\n");
        rc = 1;
        return rc;
    }
    uint32_t *w_iosize = (uint32_t *)malloc(g_max_transfer_block * sizeof(uint32_t));
    if (!w_iosize) {
        fprintf(stderr, "Fall to allocate memory for w_iosize\n");
        rc = 1;
        free(r_iosize);
        return rc;
    }

    memset(r_iosize, 0, g_max_transfer_block * sizeof(uint32_t));
    memset(w_iosize, 0, g_max_transfer_block * sizeof(uint32_t));
    
    rewind(fptr);
    size_t remain_entry = total_entry;
    while (!feof(fptr) && remain_entry) {
        size_t buffer_entry = (remain_entry > ENTRY_MAX) ? ENTRY_MAX : remain_entry;
        remain_entry -= buffer_entry;
        struct trace_io_entry buffer[buffer_entry]; /* Allocate buffer for read file */
        size_t read_entry = fread(&buffer, sizeof(struct trace_io_entry), buffer_entry, fptr);
        if (buffer_entry != read_entry) {
                fprintf(stderr, "Fail to read input file\n");
        }

        for (size_t i = 0; i < read_entry; i++) {
            rc = process_analysis_round1(&buffer[i], r_iosize, w_iosize);
            if (rc != 0) {
                fprintf(stderr, "Analysis error\n");
                free(r_iosize);
                free(w_iosize);
                fclose(fptr);
                return rc;
            }
        }
    }

    /* Calculate average latency after process all entry */
    latency_avg(g_req_num);

    print_uline('=', printf("\nTrace Analysis\n"));

    printf("%-20s:  ", "IOPS");
    printf("%-20.3f \n", iops(g_end_tsc, g_req_num));

    printf("%-20s:  ", "Latency (us)");
    printf("MIN   %-20.3f MAX   %-20.3f AVG %-20.3f\n", g_latency_us_min, g_latency_us_max, g_latency_us_avg);
    
    printf("%-20s:  ", "Number of R/W");
    printf("READ  %-20jd WRITE %-20jd R/W %6.3f %%\n", g_read_cnt, g_write_cnt, rw_ratio(g_read_cnt, g_write_cnt));

    printf("%-20s:\n", "R/W Request size");
    for (uint64_t i = 0; i < g_max_transfer_block; i++) {
        if (!r_iosize[i] && !w_iosize[i])
            continue;
        printf("%ld blocks  ", i + 1); 
        printf("r %-5d ", r_iosize[i]);
        printf("w %-5d ", w_iosize[i]);
        printf("r+w %-5d ", r_iosize[i] + w_iosize[i]);
        printf("\n");
    }
    free(r_iosize);
    free(w_iosize);

    /*
     * Trace analysis round 2: 
     * 4. The number of R/W in a block
     * 5. The number of R/W in a zone (if the block device is ZNS SSD)
     */

    uint16_t *r_blk = (uint16_t *)malloc(g_ns_block * sizeof(uint16_t));
    if (!r_blk) {
        fprintf(stderr, "Fall to allocate memory for r_blk\n");
        rc = 1;
        return rc;
    }
    uint16_t *w_blk = (uint16_t *)malloc(g_ns_block * sizeof(uint16_t));
    if (!w_blk) {
        fprintf(stderr, "Fall to allocate memory for w_blk\n");
        free(r_blk);
        rc = 1;
        return rc;
    }
    memset(r_blk, 0, g_ns_block * sizeof(uint16_t));
    memset(w_blk, 0, g_ns_block * sizeof(uint16_t)); 

    uint16_t *r_zone = (uint16_t *)malloc(g_ns_zone * sizeof(uint16_t));
    if (!r_zone) {
        fprintf(stderr, "Fall to allocate memory for r_zone\n");
        free(r_blk);
        free(w_blk);
        rc = 1;
        return rc;
    }
    uint16_t *w_zone = (uint16_t *)malloc(g_ns_zone * sizeof(uint16_t));
    if (!w_blk) {
        fprintf(stderr, "Fall to allocate memory for w_zone\n");
        free(r_blk);
        free(w_blk);
        free(r_zone);
        rc = 1;
        return rc;
    }    
    memset(r_zone, 0, g_ns_zone * sizeof(uint16_t));
    memset(w_zone, 0, g_ns_zone * sizeof(uint16_t));

    rewind(fptr);
    remain_entry = total_entry;
    while (!feof(fptr) && remain_entry) {
        size_t buffer_entry = (remain_entry > ENTRY_MAX) ? ENTRY_MAX : remain_entry;
        remain_entry -= buffer_entry;
        struct trace_io_entry buffer[buffer_entry]; /* Allocate buffer for read file */
        size_t read_entry = fread(&buffer, sizeof(struct trace_io_entry), buffer_entry, fptr);
        if (buffer_entry != read_entry) {
                fprintf(stderr, "Fail to read input file\n");
        }

        for (size_t i = 0; i < read_entry; i++) {
            rc = process_analysis_round2(&buffer[i], r_blk, w_blk, r_zone, w_zone);
            if (rc != 0) {
                fprintf(stderr, "Analysis error\n");
                free(r_blk);
                free(w_blk);
                free(r_zone);
                free(w_zone);
                fclose(fptr);
                return rc; 
            }
        }
    }

    fclose(fptr);

    if (g_print_rwblock) {
        printf("\nNumber of R/W in a block:\n");
        //uint64_t cnt = 0; 
        for (uint64_t i = 0; i < g_ns_block; i++) {
            if (!r_blk[i] && !w_blk[i])
                continue;
            //cnt++;
            printf("0x%013lx  ", i);
            printf("r %-7d ", r_blk[i]);
            printf("w %-7d ", w_blk[i]);
            printf("r+w %-7d ", r_blk[i] + w_blk[i]);

            //if (cnt % 4 == 0)
            //    printf("\n");
            printf("\n");
        }
        printf("\n");
    }

    if (g_zone && g_print_rwzone) {
        printf("\nNumber of R/W in a zone:\n");
        //uint64_t cnt = 0;
        for (uint64_t i = 0; i < g_ns_zone; i++) {
            if (!r_zone[i] && !w_zone[i])
                continue;
            //cnt++;
            printf("ZSLBA 0x%08lx  ", i * g_zone_size_lba); 
            printf("r %-7d ", r_zone[i]);
            printf("w %-7d ", w_zone[i]);
            printf("r+w %-7d ", r_zone[i] + w_zone[i]);

            //if (cnt % 4 == 0)
            //    printf("\n");
            printf("\n");
        }
        printf("\n");
    }

    free(r_blk);
    free(w_blk);
    free(r_zone);
    free(w_zone);
    spdk_env_fini();
    return rc;
}
