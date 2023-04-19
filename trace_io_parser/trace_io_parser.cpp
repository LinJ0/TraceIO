#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include <map>

#define UINT8BIT_MASK 0xFF
#define UINT16BIT_MASK 0xFFFF
#define UINT32BIT_MASK 0xFFFFFFFF

extern "C" {
#include "spdk/trace_parser.h"
#include "spdk/util.h"
}

static struct spdk_trace_parser *g_parser;
static const struct spdk_trace_flags *g_flags;
static bool g_print_tsc = false;
static uint64_t read_cnt = 0, write_cnt = 0;
static float rw_ratio = 0.0;

enum nvme_io_cmd_opc {
    NVME_OPC_FLUSH = 0x00,
    NVME_OPC_WRITE = 0x01,
    NVME_OPC_READ = 0x02,
    NVME_OPC_WRITE_UNCORRECTABLE = 0x04,
    NVME_OPC_COMPARE = 0x05,
    NVME_OPC_WRITE_ZEROES = 0x08,
    NVME_OPC_DATASET_MANAGEMENT = 0x09,
    NVME_OPC_VERIFY = 0x0C,
    NVME_OPC_RESERVATION_REGISTER = 0x0D,
    NVME_OPC_RESERVATION_REPORT = 0x0E,
    NVME_OPC_RESERVATION_ACQUIRE = 0x11,
    NVME_OPC_RESERVATION_RELEASE = 0x15,
    NVME_OPC_COPY = 0x19,
    NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND = 0x79,
    NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV = 0x7A,
    NVME_ZNS_OPC_ZONE_APPEND = 0x7D,
};

enum nvme_zns_mgmt_send_action {
    NVME_ZNS_MGMT_SEND_ACTION_OPEN = 0x01,
    NVME_ZNS_MGMT_SEND_ACTION_CLOSE = 0x02,
    NVME_ZNS_MGMT_SEND_ACTION_FINISH = 0x03,
    NVME_ZNS_MGMT_SEND_ACTION_RESET = 0x04,
    NVME_ZNS_MGMT_SEND_ACTION_OFFLINE = 0x05,
};

enum nvme_zns_mgmt_recv_action {
    NVME_ZNS_MGMT_RECV_ACTION_REPORT_ZONES = 0x00,
    NVME_ZNS_MGMT_RECV_ACTION_EXTENDED_REPORT_ZONES = 0x01,
};

/* This is a bit ugly, but we don't want to include env_dpdk in the app, while spdk_util, which we
 * do need, uses some of the functions implemented there.  We're not actually using the functions
 * that depend on those, so just define them as no-ops to allow the app to link.
 */
extern "C" {
    void *
    spdk_realloc(void *buf, size_t size, size_t align)
    {
        assert(false);

        return NULL;
    }

    void
    spdk_free(void *buf)
    {
        assert(false);
    }

    uint64_t
	spdk_get_ticks(void)
    {
        return 0;
    }
} /* extern "C" */

static void usage(void);

static char *g_exe_name;

static float
get_us_from_tsc(uint64_t tsc, uint64_t tsc_rate)
{
    return ((float)tsc) * 1000 * 1000 / tsc_rate;
}

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
    printf("%-7.7s0x%-14jx ", format_argname(arg_string), arg);
}


static void
print_uint64(const char *arg_string, uint64_t arg)
{
	
	/*
     *  Print arg as signed, since -1 is a common value especially
	 *  for FLUSH WRITEBUF when writev() returns -1 due to full
	 *  socket buffer.
	 */
	printf("%-7.7s%-16jd ", format_argname(arg_string), arg);
}

static void
print_string(const char *arg_string, const char *arg)
{
	printf("%-7.7s%-16.16s ", format_argname(arg_string), arg);
}

static void
print_float(const char *arg_string, float arg)
{
    printf("%-7s%-13.3f ", format_argname(arg_string), arg);
}


static void
print_object_id(const struct spdk_trace_tpoint *d, struct spdk_trace_parser_entry *entry)
{
    /* Set size to 128 and 256 bytes to make sure we can fit all the characters we need */
    char related_id[128] = {'\0'};
    char ids[256] = {'\0'};

    if (entry->related_type != OBJECT_NONE) {
        snprintf(related_id, sizeof(related_id), " (%c%jd)",
             g_flags->object[entry->related_type].id_prefix,
             entry->related_index);
	}

    snprintf(ids, sizeof(ids), "%c%jd%s", g_flags->object[d->object_type].id_prefix,
        entry->object_index, related_id);
    printf("id:    %-17s", ids);
}

/* 
 * Print the zone action
 */
static void
print_zone_action (uint8_t opc, uint64_t zone_act) 
{
    if (opc ==  NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND) {
        switch (zone_act) {
        case NVME_ZNS_MGMT_SEND_ACTION_OPEN:
            printf("%-20.20s ", "OPEN ZONE");
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_CLOSE:
            printf("%-20.20s ", "CLOSE ZONE");
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_FINISH:
            printf("%-20.20s ", "FINISH ZONE");
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_RESET:
            printf("%-20.20s ", "RESET ZONE");
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_OFFLINE:
            printf("%-20.20s ", "OFFLINE ZONE");
            break;
        default:
            break;
        }
    } else if (opc == NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV){
        switch (zone_act) {
        case NVME_ZNS_MGMT_RECV_ACTION_REPORT_ZONES:
            printf("%-20.20s ", "REPORT ZONE");
            break;
        case NVME_ZNS_MGMT_RECV_ACTION_EXTENDED_REPORT_ZONES:
            printf("%-20.20s ", "EXT REPORT ZONE");
            break;
        default:
            break;
        }
    } else {
        printf("%-20.20s ", "unknown");
    }
}

/* 
 * Print the opcode of the IO command
 */
static uint8_t
print_nvme_io_op(uint64_t arg)
{
    switch (arg) {
    case NVME_OPC_FLUSH:
        printf("%-20.20s ", "FLUSH");
        break;
    case NVME_OPC_WRITE:
        printf("%-20.20s ", "WRITE");
        break;
    case NVME_OPC_READ:
        printf("%-20.20s ", "READ");
        break;
    case NVME_OPC_WRITE_UNCORRECTABLE:
        printf("%-20.20s ", "WRITE UNCORRECTABLE");
        break;
    case NVME_OPC_COMPARE:
        printf("%-20.20s ", "COMPARE");
        break;
    case NVME_OPC_WRITE_ZEROES:
        printf("%-20.20s ", "WRITE ZEROES");
        break;
    case NVME_OPC_DATASET_MANAGEMENT:
        printf("%-20.20s ", "DATASET MGMT");
        break;
    case NVME_OPC_VERIFY:
        printf("%-20.20s ", "VERIFY");
        break;
    case NVME_OPC_RESERVATION_REGISTER: 
        printf("%-20.20s ", "RESERVATION REGISTER");
        break; 
    case NVME_OPC_RESERVATION_REPORT: 
        printf("%-20.20s ", "RESERVATION REPORT");
        break;
    case NVME_OPC_RESERVATION_ACQUIRE: 
        printf("%-20.20s ", "RESERVATION ACQUIRE");
        break;
    case NVME_OPC_RESERVATION_RELEASE:
        printf("%-20.20s ", "RESERVATION RELEASE");
        break;
    case NVME_OPC_COPY:
        printf("%-20.20s ", "COPY");
        break;
    case NVME_ZNS_OPC_ZONE_APPEND:
        printf("%-20.20s ", "ZONE APPEND");
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
        printf("%-20.20s ", "ZONE MGMT SEND");
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
        printf("%-15.15s ", "ZONE MGMT RECV");
        break;
    default:
        printf("%-20.20s ", "unknown");
        break;
    }
    return (uint8_t)arg;
}

static void
set_op_flags(uint8_t opc, bool *cdw10, bool *cdw11, bool *cdw12, bool *cdw13)
{
    switch (opc) {
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
        *cdw10 = true;
        *cdw11 = true;
        *cdw12 = true;
        *cdw13 = true; // not sure if this is needed
        break;
    case NVME_OPC_WRITE:
    case NVME_OPC_READ:
    case NVME_OPC_WRITE_UNCORRECTABLE:
    case NVME_OPC_COMPARE:
    case NVME_OPC_WRITE_ZEROES:
    case NVME_OPC_VERIFY:
    case NVME_OPC_COPY:
    case NVME_ZNS_OPC_ZONE_APPEND:
        *cdw10 = true;
        *cdw11 = true;
        *cdw12 = true;
        break;
    case NVME_OPC_DATASET_MANAGEMENT:
        *cdw10 = true;
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
        *cdw10 = true;
        *cdw11 = true;
        *cdw13 = true;
        break;
    case NVME_OPC_FLUSH:
    case NVME_OPC_RESERVATION_REGISTER: 
    case NVME_OPC_RESERVATION_REPORT:
    case NVME_OPC_RESERVATION_ACQUIRE:
    case NVME_OPC_RESERVATION_RELEASE:
        break;
    default:
        break;
    }
}

static void
rw_counter(uint8_t opc, uint64_t *read, uint64_t *write)
{
    switch (opc) {
    case NVME_OPC_READ:
    case NVME_OPC_COMPARE: 
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
        (*read)++;
        break;
    case NVME_OPC_WRITE:
    case NVME_OPC_WRITE_UNCORRECTABLE:
    case NVME_OPC_WRITE_ZEROES:
    case NVME_OPC_COPY:
    case NVME_ZNS_OPC_ZONE_APPEND:
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
        (*write)++;
        break;
    case NVME_OPC_VERIFY:
    case NVME_OPC_DATASET_MANAGEMENT:
    case NVME_OPC_FLUSH:
    case NVME_OPC_RESERVATION_REGISTER: 
    case NVME_OPC_RESERVATION_REPORT:
    case NVME_OPC_RESERVATION_ACQUIRE:
    case NVME_OPC_RESERVATION_RELEASE:
        break;
    default:
        break;
    }
}

static void
process_submit_entry(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_offset)
{
    struct spdk_trace_entry *e = entry->entry;
    const struct spdk_trace_tpoint  *d;
    float	us;
    size_t	i;
    static uint8_t opc;
    const char *slba_str = "slba";
    const char *nlb_str = "nlb";
    const char *nr_str = "nr";
    const char *ndw_str = "ndw";
    /* cdw flag */
    bool    cdw10 = false;
    bool    cdw11 = false;
    bool    cdw12 = false;
    bool    cdw13 = false;
    /* cdw value */
    uint64_t    slba;

    d = &g_flags->tpoint[e->tpoint_id];
    us = get_us_from_tsc(e->tsc - tsc_offset, tsc_rate);

    /* 
     * print lcore & tsc_diff (us) 
     */
    printf("core%2d: %10.3f ", entry->lcore, us);
    if (g_print_tsc) {
        printf("(%9ju) ", e->tsc - tsc_offset);
    }

    if (d->new_object) {
        print_object_id(d, entry);
    } else if (d->object_type != OBJECT_NONE) {
        if (entry->object_index != UINT64_MAX) {
            us = get_us_from_tsc(e->tsc - entry->object_start, tsc_rate);
            print_object_id(d, entry);
            print_float("time", us);
        } else {
            printf("id:    N/A");
        }
    } else if (e->object_id != 0) {
        print_ptr("object", e->object_id);
    }
    /* 
     * process tracepoint args
     */
    for (i = 1; i < d->num_args; ++i) {
        if (i == 1) { /* print opcode & set cdw flags*/ 
            opc = print_nvme_io_op(entry->args[i].integer);
            set_op_flags(opc, &cdw10, &cdw11, &cdw12, &cdw13);
            rw_counter(opc, &read_cnt, &write_cnt);
            if (opc == NVME_OPC_FLUSH || 
                opc == NVME_OPC_RESERVATION_REGISTER ||
                opc == NVME_OPC_RESERVATION_REPORT ||
                opc == NVME_OPC_RESERVATION_ACQUIRE ||
                opc == NVME_OPC_RESERVATION_RELEASE) {
                break;
            }
        } else if (i == 3) { /* nsid */
            print_ptr(d->args[i].name, (uint64_t)entry->args[i].pointer);
        } else if (i == 4 && cdw10) { /* slba_l64b | nr_8b (dataset_mgmt) */
            if (opc != NVME_OPC_DATASET_MANAGEMENT)
                slba = (uint64_t)entry->args[i].integer;
            else 
                print_ptr(nr_str, entry->args[i].integer & UINT8BIT_MASK);
        } else if (i == 5 && cdw11) { /* slba_h64b */
            slba |= ((uint64_t)entry->args[i].integer & UINT32BIT_MASK) << 32;
            print_ptr(slba_str, entry->args[i].integer);
        } else if (i == 6 && cdw12) { /* nlb_16b | nr_8b (copy) | ndw_32b (z_mgmt_recv) */
            if (opc == NVME_OPC_COPY)
                print_ptr(nr_str, entry->args[i].integer & UINT8BIT_MASK);
            else if (opc == NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV)
                print_ptr(ndw_str, entry->args[i].integer & UINT32BIT_MASK);
            else
                print_ptr(nlb_str, entry->args[i].integer & UINT16BIT_MASK);
        } else if (i == 7 && cdw13) { /* zsa_8b || zra_8b */
            print_zone_action (opc, entry->args[i].integer & UINT8BIT_MASK);
        } else {
            continue;
        }
    }
    printf("\n");
}

static void
process_complete_entry(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_offset)
{
    struct spdk_trace_entry		*e = entry->entry;
    const struct spdk_trace_tpoint	*d;
    float				us;
    size_t				i;

    d = &g_flags->tpoint[e->tpoint_id];
    us = get_us_from_tsc(e->tsc - tsc_offset, tsc_rate);

    /* 
     * print lcore & tsc_diff (us) 
     */
    printf("core%2d: %10.3f ", entry->lcore, us);
    if (g_print_tsc) {
        printf("(%9ju) ", e->tsc - tsc_offset);
    }

    if (d->new_object) {
        print_object_id(d, entry);
    } else if (d->object_type != OBJECT_NONE) {
        if (entry->object_index != UINT64_MAX) {
            us = get_us_from_tsc(e->tsc - entry->object_start, tsc_rate);
            print_object_id(d, entry);
            print_float("time", us);
        } else {
            printf("id:    N/A");
        }
    } else if (e->object_id != 0) {
        print_ptr("object", e->object_id);
    }

    for (i = 1; i < d->num_args; ++i) {
        switch (d->args[i].type) {
        case SPDK_TRACE_ARG_TYPE_PTR:
            print_ptr(d->args[i].name, (uint64_t)entry->args[i].pointer);
            break;
        case SPDK_TRACE_ARG_TYPE_INT:
            print_uint64(d->args[i].name, entry->args[i].integer);
            break;
        case SPDK_TRACE_ARG_TYPE_STR:
            print_string(d->args[i].name, entry->args[i].string);
            break;
        }
    }
    printf("\n");
}

static void
usage(void)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "   %s <option> <lcore#>\n", g_exe_name);
    fprintf(stderr, "                 '-c' to display single lcore history\n");
    fprintf(stderr, "                 '-t' to display TSC offset for each event\n");
    fprintf(stderr, "                 '-s' to specify spdk_trace shm name for a\n");
    fprintf(stderr, "                      currently running process\n");
    fprintf(stderr, "                 '-i' to specify the shared memory ID\n");
    fprintf(stderr, "                 '-p' to specify the trace PID\n");
    fprintf(stderr, "                      (If -s is specified, then one of\n");
    fprintf(stderr, "                       -i or -p must be specified)\n");
    fprintf(stderr, "                 '-f' to specify a tracepoint file name\n");
    fprintf(stderr, "                      (-s and -f are mutually exclusive)\n");
}

int
main(int argc, char **argv)
{
    struct spdk_trace_parser_opts	opts;
    struct spdk_trace_parser_entry	entry;
    int				    lcore = SPDK_TRACE_MAX_LCORE;
    uint64_t			tsc_offset, entry_count;
    const char			*app_name = NULL;
    const char			*file_name = NULL;
    int				    op, i;
    char				shm_name[64];
    int				    shm_id = -1, shm_pid = -1;
    struct spdk_trace_entry     *e;
    const struct spdk_trace_tpoint	*d;
    const char tp_io_submit[] = "NVME_IO_SUBMIT";
    const char tp_io_complete[] = "NVME_IO_COMPLETE";


    g_exe_name = argv[0];
    while ((op = getopt(argc, argv, "c:f:i:jp:s:t")) != -1) {
        switch (op) {
        case 'c':
            lcore = atoi(optarg);
            if (lcore > SPDK_TRACE_MAX_LCORE) {
                fprintf(stderr, "Selected lcore: %d "
                    "exceeds maximum %d\n", lcore,
                    SPDK_TRACE_MAX_LCORE);
                exit(1);
            }
            break;
        case 'i':
            shm_id = atoi(optarg);
            break;
        case 'p':
            shm_pid = atoi(optarg);
            break;
        case 's':
            app_name = optarg;
            break;
        case 'f':
            file_name = optarg;
            break;
        case 't':
            g_print_tsc = true;
            break;
        default:
            usage();
            exit(1);
        }
    }

    if (file_name != NULL && app_name != NULL) {
        fprintf(stderr, "-f and -s are mutually exclusive\n");
        usage();
        exit(1);
    }

    if (file_name == NULL && app_name == NULL) {
        fprintf(stderr, "One of -f and -s must be specified\n");
        usage();
        exit(1);
    }


    if (!file_name) {
        if (shm_id >= 0) {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
        } else {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
        }
        file_name = shm_name;
    }

    opts.filename = file_name;
    opts.lcore = lcore;
    opts.mode = app_name == NULL ? SPDK_TRACE_PARSER_MODE_FILE : SPDK_TRACE_PARSER_MODE_SHM;
    g_parser = spdk_trace_parser_init(&opts);
    if (g_parser == NULL) {
        fprintf(stderr, "Failed to initialize trace parser\n");
        exit(1);
    }

    g_flags = spdk_trace_parser_get_flags(g_parser);
    printf("TSC Rate: %ju\n", g_flags->tsc_rate);

    for (i = 0; i < SPDK_TRACE_MAX_LCORE; ++i) {
        if (lcore == SPDK_TRACE_MAX_LCORE || i == lcore) {
            entry_count = spdk_trace_parser_get_entry_count(g_parser, i);
            if (entry_count > 0) {
                printf("Trace Size of lcore (%d): %ju\n", i, entry_count);
            }
        }
    }

    tsc_offset = spdk_trace_parser_get_tsc_offset(g_parser);
	
    while (spdk_trace_parser_next_entry(g_parser, &entry)) {
        if (entry.entry->tsc < tsc_offset) {
            continue;
        }
        e = entry.entry;
        d = &g_flags->tpoint[e->tpoint_id];
    
        if (strcmp(d->name, tp_io_submit) != 0 && strcmp(d->name, tp_io_complete) != 0) {
            continue;
        } else if ((strcmp(d->name, tp_io_submit) == 0 || strcmp(d->name, tp_io_complete) == 0)
                    && entry.args[0].integer) { 
            continue;   
        }

        if (strcmp(d->name, tp_io_submit) == 0)
            process_submit_entry(&entry, g_flags->tsc_rate, tsc_offset);
        else if (strcmp(d->name, tp_io_complete) == 0)
            process_complete_entry(&entry, g_flags->tsc_rate, tsc_offset);
    }   
    printf("============================================================");
    printf("   TRACE ANALYSIS   ");
    printf("============================================================\n");
    rw_ratio = (read_cnt + write_cnt) ? (read_cnt * 100) / (read_cnt + write_cnt) : 0;
    printf("READ: %-20jd  WRITE: %-20jd  R/W: %.3f%%\n", read_cnt, write_cnt, rw_ratio);
    spdk_trace_parser_cleanup(g_parser);

    return (0);
}
