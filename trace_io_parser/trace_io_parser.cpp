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
static int output_file_entry = 0;

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
set_zone_act_name (uint8_t opc, uint64_t zone_act, const char **zone_act_name) 
{
    if (opc ==  NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND) {
        switch (zone_act) {
        case NVME_ZNS_MGMT_SEND_ACTION_OPEN:
            *zone_act_name = "OPEN ZONE";
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_CLOSE:
            *zone_act_name = "CLOSE ZONE";
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_FINISH:
            *zone_act_name = "FINISH ZONE";
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_RESET:
            *zone_act_name = "RESET ZONE";
            break;
        case NVME_ZNS_MGMT_SEND_ACTION_OFFLINE:
            *zone_act_name = "OFFLINE ZONE";
            break;
        default:
            break;
        }
    } else if (opc == NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV){
        switch (zone_act) {
        case NVME_ZNS_MGMT_RECV_ACTION_REPORT_ZONES:
            *zone_act_name = "REPORT ZONE";
            break;
        case NVME_ZNS_MGMT_RECV_ACTION_EXTENDED_REPORT_ZONES:
            *zone_act_name = "EXT REPORT ZONE";
            break;
        default:
            break;
        }
    } else {
        *zone_act_name = "unknown";
    }
}

/* 
 * Print the opcode of the IO command
 */
static uint8_t
set_opc_name(uint64_t arg, const char **opc_name)
{
    switch (arg) {
    case NVME_OPC_FLUSH:
        *opc_name = "FLUSH";
        break;
    case NVME_OPC_WRITE:
        *opc_name = "WRITE";
        break;
    case NVME_OPC_READ:
        *opc_name =  "READ";
        break;
    case NVME_OPC_WRITE_UNCORRECTABLE:
        *opc_name =  "WRITE UNCORRECTABLE";
        break;
    case NVME_OPC_COMPARE:
        *opc_name =  "COMPARE";
        break;
    case NVME_OPC_WRITE_ZEROES:
        *opc_name =  "WRITE ZEROES";
        break;
    case NVME_OPC_DATASET_MANAGEMENT:
        *opc_name =  "DATASET MGMT";
        break;
    case NVME_OPC_VERIFY:
        *opc_name =  "VERIFY";
        break;
    case NVME_OPC_RESERVATION_REGISTER: 
        *opc_name =  "RESERVATION REGISTER";
        break; 
    case NVME_OPC_RESERVATION_REPORT: 
        *opc_name =  "RESERVATION REPORT";
        break;
    case NVME_OPC_RESERVATION_ACQUIRE: 
        *opc_name =  "RESERVATION ACQUIRE";
        break;
    case NVME_OPC_RESERVATION_RELEASE:
        *opc_name =  "RESERVATION RELEASE";
        break;
    case NVME_OPC_COPY:
        *opc_name =  "COPY";
        break;
    case NVME_ZNS_OPC_ZONE_APPEND:
        *opc_name =  "ZONE APPEND";
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
        *opc_name =  "ZONE MGMT SEND";
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
        *opc_name =  "ZONE MGMT RECV";
        break;
    default:
        *opc_name = "unknown";
        break;
    }
    return (uint8_t)arg;
}

static void
set_opc_flags(uint8_t opc, bool *cdw10, bool *cdw11, bool *cdw12, bool *cdw13)
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

struct output_file_data {
    uint32_t lcore;
    uint64_t tsc_rate;
    uint64_t tsc_timestamp;
    uint32_t obj_idx;
    uint64_t obj_id;
    uint64_t tsc_sc_time; /* object from submit to complete (us) */
    char tpoint_name[32]; 
    char opc_name[32];
    char zone_act_name[32];
    uint32_t nsid; /* namespace id*/
    uint64_t slba; /* start logical block address */
    uint32_t nlb; /* nlb_16b | nr_8b (copy) | ndw_32b (z_mgmt_recv) */
    uint32_t nr;
    uint32_t ndw;
    uint32_t cpl; /* status code */
};

static void
process_output_file(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_base, FILE *fptr)
{
    struct spdk_trace_entry *e = entry->entry;
    const struct spdk_trace_tpoint  *d;
    size_t	i;
    struct output_file_data buffer;
    static int8_t opc;
    const char *opc_name;
    const char *zone_act_name;
    bool    cdw10 = false;
    bool    cdw11 = false;
    bool    cdw12 = false;
    bool    cdw13 = false;

    d = &g_flags->tpoint[e->tpoint_id];
    
    buffer.lcore = entry->lcore;
    buffer.tsc_rate = tsc_rate;
    buffer.tsc_timestamp = e->tsc - tsc_base;    
    buffer.obj_idx = entry->object_index;
    buffer.obj_id = e->object_id;
    if (!d->new_object && d->object_type != OBJECT_NONE && entry->object_index != UINT64_MAX) {
        buffer.tsc_sc_time = e->tsc - entry->object_start;
    }

    strcpy(buffer.tpoint_name, d->name);
    if (strcmp(buffer.tpoint_name, "NVME_IO_SUBMIT") == 0) {
        for (i = 1; i < d->num_args; ++i) {
            if (i == 1) { /* print opcode & set cdw flags*/ 
                opc = set_opc_name(entry->args[i].integer, &opc_name);
                strcpy(buffer.opc_name, opc_name);
                set_opc_flags(opc, &cdw10, &cdw11, &cdw12, &cdw13);
                if (opc == NVME_OPC_FLUSH || 
                    opc == NVME_OPC_RESERVATION_REGISTER ||
                    opc == NVME_OPC_RESERVATION_REPORT ||
                    opc == NVME_OPC_RESERVATION_ACQUIRE ||
                    opc == NVME_OPC_RESERVATION_RELEASE) {
                    break;
                }
            } else if (i == 3) { /* nsid */
                buffer.nsid = (uint64_t)entry->args[i].pointer;
            } else if (i == 4 && cdw10) { /* slba_l64b | nr_8b (dataset_mgmt) */
                if (opc != NVME_OPC_DATASET_MANAGEMENT)
                    buffer.slba = (uint64_t)entry->args[i].integer;
                else 
                    buffer.nr = entry->args[i].integer & UINT8BIT_MASK;
            } else if (i == 5 && cdw11) { /* slba_h64b */
                buffer.slba |= ((uint64_t)entry->args[i].integer & UINT32BIT_MASK) << 32;
            } else if (i == 6 && cdw12) { /* nlb_16b | nr_8b (copy) | ndw_32b (z_mgmt_recv) */
                if (opc == NVME_OPC_COPY)
                    buffer.nr = entry->args[i].integer & UINT8BIT_MASK;
                else if (opc == NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV)
                    buffer.ndw = entry->args[i].integer & UINT32BIT_MASK;
                else
                    buffer.nlb = entry->args[i].integer & UINT16BIT_MASK;
            } else if (i == 7 && cdw13) { /* zsa_8b || zra_8b */
                set_zone_act_name(opc, entry->args[i].integer & UINT8BIT_MASK, &zone_act_name);
                strcpy(buffer.zone_act_name, zone_act_name);
            } else {
                continue;
            }
        }
    } else if (strcmp(buffer.tpoint_name, "NVME_IO_COMPLETE") == 0) {
        for (i = 1; i < d->num_args; ++i) {
            if (strcmp(d->args[i].name, "cpl") == 0) {
                buffer.cpl = entry->args[i].integer;
            } else 
                continue;
        }
    }
    
    fwrite(&buffer, sizeof(struct output_file_data), 1, fptr);
    output_file_entry++;
}

static void
process_submit_entry(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_base)
{
    struct spdk_trace_entry *e = entry->entry;
    const struct spdk_trace_tpoint  *d;
    float	us;
    size_t	i;
    static int8_t opc;
    const char *opc_name = NULL;
    const char *zone_act_name = NULL;
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
    us = get_us_from_tsc(e->tsc - tsc_base, tsc_rate);

    /* 
     * print lcore & tsc_diff (us) 
     */
    printf("core%2d: %10.3f ", entry->lcore, us);
    if (g_print_tsc) {
        printf("(%9ju) ", e->tsc - tsc_base);
    }

    printf("  %-*s ", (int)sizeof(d->name), d->name);

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
            opc = set_opc_name(entry->args[i].integer, &opc_name);
            printf("%-20.20s ", opc_name);
            set_opc_flags(opc, &cdw10, &cdw11, &cdw12, &cdw13);
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
            set_zone_act_name(opc, entry->args[i].integer & UINT8BIT_MASK, &zone_act_name);
            printf("%-20.20s ", zone_act_name);
        } else {
            continue;
        }
    }
    printf("\n");
}

static void
process_complete_entry(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_base)
{
    struct spdk_trace_entry		*e = entry->entry;
    const struct spdk_trace_tpoint	*d;
    float				us;
    size_t				i;

    d = &g_flags->tpoint[e->tpoint_id];
    us = get_us_from_tsc(e->tsc - tsc_base, tsc_rate);

    /* 
     * print lcore & tsc_base (us) 
     */
    printf("core%2d: %10.3f ", entry->lcore, us);
    if (g_print_tsc) {
        printf("(%9ju) ", e->tsc - tsc_base);
    }
    
    printf("  %-*s ", (int)sizeof(d->name), d->name);
    
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
    fprintf(stderr, "                 '-t' to display TSC base for each event\n");
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
    uint64_t			tsc_base, entry_count;
    const char			*app_name = NULL;
    const char			*file_name = NULL;
    int				    op, i;
    FILE                *fptr;
    char                output_file_name[68];    
    char				shm_name[64];
    int				    shm_id = -1, shm_pid = -1;
    const struct spdk_trace_tpoint	*d;

    g_exe_name = argv[0];
    while ((op = getopt(argc, argv, "c:f:i:p:s:t")) != -1) {
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

    /* 
     * output file name in ./ 
     */       
    if (!file_name) {
        if (shm_id >= 0)
            snprintf(output_file_name, sizeof(output_file_name), "%s_%d.bin", app_name, shm_id);
        else
            snprintf(output_file_name, sizeof(output_file_name), "%s_pid%d.bin", app_name, shm_pid);
    } else
        snprintf(output_file_name, sizeof(output_file_name), "%s.bin", file_name);

    /* 
     * file name in /dev/shm/ 
     */
    if (!file_name) {
        if (shm_id >= 0) {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
        } else {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
        }
        file_name = shm_name;
    }

    fptr = fopen(output_file_name, "wb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open output file %s\n", output_file_name);
        return -1; 
    }
    printf("Output .bin file: %s\n", output_file_name);   

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

	tsc_base = 0;
    while (spdk_trace_parser_next_entry(g_parser, &entry)) {
        d = &g_flags->tpoint[entry.entry->tpoint_id];
    
        if (strcmp(d->name, "NVME_IO_SUBMIT") != 0 && strcmp(d->name, "NVME_IO_COMPLETE") != 0) {
            continue;
        } else if ((strcmp(d->name, "NVME_IO_SUBMIT") == 0 || strcmp(d->name, "NVME_IO_COMPLETE") == 0)
                    && entry.args[0].integer) { 
            continue;   
        }
        /* tsc_base = first io cmd entry */
        if (!tsc_base) {
            tsc_base = entry.entry->tsc;
        }
        
        /* process io submit or io complete */
        if (strcmp(d->name, "NVME_IO_SUBMIT") == 0)
            process_submit_entry(&entry, g_flags->tsc_rate, tsc_base);
        else if (strcmp(d->name, "NVME_IO_COMPLETE") == 0)
            process_complete_entry(&entry, g_flags->tsc_rate, tsc_base);
        
        /* process output file */
        if (output_file_name)
            process_output_file(&entry, g_flags->tsc_rate, tsc_base, fptr);

    }
    printf("\noutput file entry: %d\n", output_file_entry);
    fclose(fptr);

    /*******/
    size_t read_val;
    struct output_file_data buffer[output_file_entry];
    fptr = fopen(output_file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open output file %s\n", output_file_name);
        return -1; 
    }
    
    read_val = fread(&buffer, sizeof(struct output_file_data), output_file_entry, fptr);
    if (read_val != (size_t)output_file_entry)
        fprintf(stderr, "Fail to read output file\n");

    for (i = 0; i < output_file_entry; i++) {
        printf("lcore: %d  ", buffer[i].lcore);
        printf("tsc_rate: %ld  ", buffer[i].tsc_rate);
        printf("tsc_timestamp: %ld  ", buffer[i].tsc_timestamp);
        printf("obj_idx: %d  ", buffer[i].obj_idx);
        printf("obj_id: %ld  ", buffer[i].obj_id);
        printf("tsc_sc_time: %ld  ", buffer[i].tsc_sc_time);
        printf("tpoint_name: %s  ", buffer[i].tpoint_name);
        printf("opc_name: %s  ", buffer[i].opc_name);
        printf("zone_act_name: %s  ", buffer[i].zone_act_name);
        printf("nsid: %d  ", buffer[i].nsid);
        printf("slba: %ld  ", buffer[i].slba);
        printf("nlb: %d  ", buffer[i].nlb);
        printf("nr: %d  ", buffer[i].nr);
        printf("ndw: %d  ", buffer[i].ndw);
        printf("cpl: %d  ", buffer[i].cpl);
        printf("\n");
    }
    fclose(fptr);
    /********/
    
    printf("============================================================");
    printf("   TRACE ANALYSIS   ");
    printf("============================================================\n");
    rw_ratio = (read_cnt + write_cnt) ? (read_cnt * 100) / (read_cnt + write_cnt) : 0;
    printf("READ: %-20jd  WRITE: %-20jd  R/W: %.3f%%\n", read_cnt, write_cnt, rw_ratio);
    spdk_trace_parser_cleanup(g_parser);

    return (0);
}
