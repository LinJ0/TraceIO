#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"

#define UINT8BIT_MASK 0xFF
#define UINT16BIT_MASK 0xFFFF
#define UINT32BIT_MASK 0xFFFFFFFF

struct bin_file_data {
    uint32_t lcore;
    uint64_t tsc_rate;
    uint64_t tsc_timestamp;
    uint32_t obj_idx;
    uint64_t obj_id;
    uint64_t tsc_sc_time; /* object from submit to complete (us) */
    char     tpoint_name[32];       
    uint16_t opc;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cpl;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
};

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

static bool g_print_tsc = false;

/* This is a bit ugly, but we don't want to include env_dpdk in the app, while spdk_util, which we
 * do need, uses some of the functions implemented there.  We're not actually using the functions
 * that depend on those, so just define them as no-ops to allow the app to link.
 */
/*
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
}*/ 
/* extern "C" */

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

static void
set_opc_name(uint64_t opc, const char **opc_name)
{
    switch (opc) {
    case NVME_OPC_FLUSH:
        *opc_name = "FLUSH";
        break;
    case NVME_OPC_WRITE:
        *opc_name = "WRITE";
        break;
    case NVME_OPC_READ:
        *opc_name = "READ";
        break;
    case NVME_OPC_WRITE_UNCORRECTABLE:
        *opc_name = "WRITE UNCORRECTABLE";
        break;
    case NVME_OPC_COMPARE:
        *opc_name = "COMPARE";
        break;
    case NVME_OPC_WRITE_ZEROES:
        *opc_name = "WRITE ZEROES";
        break;
    case NVME_OPC_DATASET_MANAGEMENT:
        *opc_name = "DATASET MGMT";
        break;
    case NVME_OPC_VERIFY:
        *opc_name = "VERIFY";
        break;
    case NVME_OPC_RESERVATION_REGISTER: 
        *opc_name = "RESERVATION REGISTER";
        break; 
    case NVME_OPC_RESERVATION_REPORT: 
        *opc_name = "RESERVATION REPORT";
        break;
    case NVME_OPC_RESERVATION_ACQUIRE: 
        *opc_name = "RESERVATION ACQUIRE";
        break;
    case NVME_OPC_RESERVATION_RELEASE:
        *opc_name = "RESERVATION RELEASE";
        break;
    case NVME_OPC_COPY:
        *opc_name = "COPY";
        break;
    case NVME_ZNS_OPC_ZONE_APPEND:
        *opc_name = "ZONE APPEND";
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
        *opc_name = "ZONE MGMT SEND";
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
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
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
        *cdw10 = true;
        *cdw11 = true;
        *cdw12 = true;
        *cdw13 = true; 
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

static uint64_t read_cnt = 0, write_cnt = 0;
static float rw_ratio = 0.0;

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

static int
process_entry (struct bin_file_data *d)
{
    float   timestamp_us;
    float   sctime_us;
    const char *opc_name;
    const char *zone_act_name;
    bool cdw10 = false;
    bool cdw11 = false;
    bool cdw12 = false;
    bool cdw13 = false;
    uint64_t slba = 0;

    /* 
     * print lcore & tsc_base (us) & tpoint name & object id
     */
    timestamp_us = get_us_from_tsc(d->tsc_timestamp, d->tsc_rate);
    printf("core%2d: %16.3f  ", d->lcore, timestamp_us);
    
    if (g_print_tsc) {
        printf("(%10ju)  ", d->tsc_timestamp);
    }
    printf("%-20s ", d->tpoint_name);
    print_ptr("object", d->obj_id);
    
    /* 
     * print process nvme submit / complete
     */
    if (strcmp(d->tpoint_name, "NVME_IO_SUBMIT") == 0) {
        set_opc_name(d->opc, &opc_name);
        set_opc_flags(d->opc, &cdw10, &cdw11, &cdw12, &cdw13);
        rw_counter(d->opc, &read_cnt, &write_cnt);
        printf("%-20s ", opc_name);
        print_uint64("cid", d->cid);
        print_ptr("nsid", d->nsid);

        if (cdw10) { /* slba_l64b | nr_8b (dataset_mgmt) */
            if (d->opc != NVME_OPC_DATASET_MANAGEMENT)
                slba = (uint64_t)d->cdw10;
            else 
                print_ptr("nr", d->cdw10 & UINT8BIT_MASK);
        }

        if (cdw11) { /* slba_h64b */
            slba |= ((uint64_t)d->cdw11 & UINT32BIT_MASK) << 32;
            
            if (d->opc != NVME_ZNS_OPC_ZONE_APPEND) {
                print_ptr("slba", slba);
            } else {
                print_ptr("zslba", slba);
            }
        }

        if (cdw12) { /* nlb_16b | nr_8b (copy) | ndw_32b (z_mgmt_recv) */
            if (d->opc == NVME_OPC_COPY)
                print_uint64("range", (d->cdw12 & UINT8BIT_MASK) + 1);
            else if (d->opc == NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV)
                print_uint64("dword", (d->cdw12 & UINT32BIT_MASK) + 1);
            else
                print_uint64("block", (d->cdw12 & UINT16BIT_MASK) + 1);
        }

        if (cdw13) { /* zsa_8b || zra_8b */
            set_zone_act_name(d->opc, d->cdw13 & UINT8BIT_MASK, &zone_act_name);
            printf("%-20.20s ", zone_act_name);
        }
        return 0;
    } else if (strcmp(d->tpoint_name, "NVME_IO_COMPLETE") == 0) {
        if (d->tsc_sc_time) {
            sctime_us = get_us_from_tsc(d->tsc_sc_time, d->tsc_rate);
            print_float("time", sctime_us);
        }

        print_uint64("cid", d->cid);
        print_ptr("comp", d->cpl & (uint64_t)0x1);
        print_ptr("status", (d->cpl >> 1) & (uint64_t)0x7FFF);
        return 0;
    }
    return 1;
}

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("   %s <options>\n", program_name);
    printf("\n");
    printf("         '-i' The input file which generated by trace_io_parser\n");
    printf("         '-t' to display TSC base for each event\n");
}

static int
parse_args(int argc, char **argv, char *file_name, size_t file_name_size)
{
    int op;

    while ((op = getopt(argc, argv, "f:t")) != -1) {
        switch (op) {
        case 'f':
            snprintf(file_name, file_name_size, "%s", optarg);
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
    struct spdk_env_opts env_opts;
    int rc;
    char input_file_name[68];
    FILE *fptr;
    int file_size;
    int entry_cnt;
    size_t read_val;

    rc = parse_args(argc, argv, input_file_name, sizeof(input_file_name));
    if (rc != 0) {
        return rc;
    }

    fptr = fopen(input_file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open input file %s\n", input_file_name);
        return -1; 
    }

    fseek(fptr, 0, SEEK_END);
    file_size = ftell(fptr);
    rewind(fptr);
    entry_cnt = file_size / sizeof(struct bin_file_data);

    struct bin_file_data buffer[entry_cnt];    
 
    read_val = fread(&buffer, sizeof(struct bin_file_data), entry_cnt, fptr);
    if (read_val != (size_t)entry_cnt)
        fprintf(stderr, "Fail to read input file\n");
    fclose(fptr);

    spdk_env_opts_init(&env_opts);
    env_opts.name = "trace_io_parser";
    if (spdk_env_init(&env_opts) < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        return 1;
    }

    for (int i = 0; i < entry_cnt; i++) {
        rc = process_entry(&buffer[i]);
        printf("\n");

        if (rc != 0) {
            fprintf(stderr, "Parse error\n");
            return 1;
        }
    }

    printf("============================================================");
    printf("   TRACE ANALYSIS   ");
    printf("============================================================\n");
    rw_ratio = (read_cnt + write_cnt) ? (read_cnt * 100) / (read_cnt + write_cnt) : 0;
    printf("READ: %-20jd  WRITE: %-20jd  R/W: %20.3f%%\n", read_cnt, write_cnt, rw_ratio);

    spdk_env_fini();
    return 0;
}

