#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "../include/trace_io.h"

static bool g_print_tsc = false;
static bool g_print_io = false;
static bool g_input_file = false;

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

/* trace analysis start */
static uint64_t g_read_cnt = 0, g_write_cnt = 0;

static float
rw_ratio(uint64_t *read, uint64_t *write)
{
    float ratio = 0.0;
    return ratio = (*read + *write) ? (*read * 100) / (*read + *write) : 0;
}

static int
rw_counter(uint8_t opc, uint64_t *read, uint64_t *write)
{
    switch (opc) {
    case NVME_OPC_READ:
    case NVME_OPC_COMPARE: 
        (*read)++;
        break;
    case NVME_OPC_WRITE:
    case NVME_ZNS_OPC_ZONE_APPEND:
        (*write)++;
        break;
    case NVME_OPC_WRITE_UNCORRECTABLE:
    case NVME_OPC_WRITE_ZEROES:
    case NVME_OPC_COPY:
    case NVME_OPC_VERIFY:
    case NVME_OPC_DATASET_MANAGEMENT:
    case NVME_OPC_FLUSH:
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
    case NVME_OPC_RESERVATION_REGISTER: 
    case NVME_OPC_RESERVATION_REPORT:
    case NVME_OPC_RESERVATION_ACQUIRE:
    case NVME_OPC_RESERVATION_RELEASE:
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
static uint64_t g_latency_tsc_min = 0, g_latency_tsc_max = 0;
static float g_latency_us_min = 0.0, g_latency_us_max = 0.0, g_latency_us_avg = 0.0;
static float g_latency_tsc_avg = 0;

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
    printf("\n");
    
    g_latency_us_avg = get_us_from_tsc(g_latency_tsc_avg, g_tsc_rate);
}
/* trace analysis end */

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

static int
process_entry (struct bin_file_data *d)
{
    int     rc;
    float   timestamp_us;
    float   sctime_us;
    const char *opc_name;
    const char *zone_act_name;
    bool cdw10 = false;
    bool cdw11 = false;
    bool cdw12 = false;
    bool cdw13 = false;
    uint64_t slba = 0;

    if (!g_tsc_rate) {
        g_tsc_rate = d->tsc_rate;
    }

    if (strcmp(d->tpoint_name, "NVME_IO_COMPLETE") == 0) {
        latency_min_max(d->tsc_sc_time, d->tsc_rate);
        latency_total(d->tsc_sc_time);
    }

    rc = rw_counter(d->opc, &g_read_cnt, &g_write_cnt);
    if (rc) {
        return rc;
    }

    /* 
     * print lcore & tsc_base (us) & tpoint name & object id
     */
    if (g_print_io) {
        timestamp_us = get_us_from_tsc(d->tsc_timestamp, d->tsc_rate);
        printf("core%2d: %16.3f  ", d->lcore, timestamp_us);
    
        if (g_print_tsc) {
            printf("(%10ju)  ", d->tsc_timestamp);
        }
        printf("%-20s ", d->tpoint_name);
        print_ptr("object", d->obj_id);
    }
    
    /* 
     * print process nvme submit / complete
     */
    if (g_print_io && strcmp(d->tpoint_name, "NVME_IO_SUBMIT") == 0) {
        set_opc_name(d->opc, &opc_name);
        set_opc_flags(d->opc, &cdw10, &cdw11, &cdw12, &cdw13);
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
        printf("\n");
        rc = 0;
    }
    
    if (g_print_io && strcmp(d->tpoint_name, "NVME_IO_COMPLETE") == 0) {
        if (d->tsc_sc_time) {
            sctime_us = get_us_from_tsc(d->tsc_sc_time, d->tsc_rate);
            print_float("time", sctime_us);
        }

        print_uint64("cid", d->cid);
        print_ptr("comp", d->cpl & (uint64_t)0x1);
        print_ptr("status", (d->cpl >> 1) & (uint64_t)0x7FFF);
        printf("\n");
        rc = 0;
    }

    if (strcmp(d->tpoint_name, "NVME_IO_SUBMIT") && strcmp(d->tpoint_name, "NVME_IO_COMPLETE")) {
        rc = 1;
    }
    return rc;
}

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("   %s <options>\n", program_name);
    printf("\n");
    printf("         '-f' specify the input file which generated by trace_io_record\n");
    printf("         '-d' to display each event\n");
    printf("         '-t' to display TSC for each event\n");
}

static int
parse_args(int argc, char **argv, char *file_name, size_t file_name_size)
{
    int op;

    while ((op = getopt(argc, argv, "f:dt")) != -1) {
        switch (op) {
        case 'f':
            g_input_file = true;
            snprintf(file_name, file_name_size, "%s", optarg);
            break;
        case 'd':
            g_print_io = true;
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
    char input_file_name[68];

    int rc = parse_args(argc, argv, input_file_name, sizeof(input_file_name));
    if (rc != 0) {
        return rc;
    }

    if (!g_print_io && g_print_tsc) {
        fprintf(stderr, "-t must be used with -d \n");
        exit(1);
    }

    if (input_file_name == NULL || !g_input_file) {
        fprintf(stderr, "-f input file must be specified\n");
        exit(1);
    }

    FILE *fptr = fopen(input_file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open input file %s\n", input_file_name);
        return -1; 
    }

    fseek(fptr, 0, SEEK_END);
    int file_size = ftell(fptr);
    rewind(fptr);
    int entry_cnt = file_size / sizeof(struct bin_file_data);

    struct bin_file_data buffer[entry_cnt];    
 
    size_t read_val = fread(&buffer, sizeof(struct bin_file_data), entry_cnt, fptr);
    if (read_val != (size_t)entry_cnt)
        fprintf(stderr, "Fail to read input file\n");
    fclose(fptr);

    spdk_env_opts_init(&env_opts);
    env_opts.name = "trace_io_analysis";
    if (spdk_env_init(&env_opts) < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        return 1;
    }

    for (int i = 0; i < entry_cnt; i++) {
        rc = process_entry(&buffer[i]);
        if (rc != 0) {
            fprintf(stderr, "Parse error\n");
            return 1;
        }
    }

    print_uline('=', printf("\nTrace Analysis\n")); 
    printf("%-15s  ", "Access pattern");
    printf("READ:  %-20jd WRITE: %-20jd R/W: %18.3f %%\n", 
            g_read_cnt, g_write_cnt, rw_ratio(&g_read_cnt, &g_write_cnt)); 
    
    latency_avg(entry_cnt >> 1);
    printf("%-15s  ", "Latency (tsc)");
    printf("MIN:   %-20ld MAX:   %-20ld AVG: %-20.3f\n", 
            g_latency_tsc_min, g_latency_tsc_max, g_latency_tsc_avg);

    printf("%-15s  ", "Latency (us)");
    printf("MIN:   %-20.3f MAX:   %-20.3f AVG: %-20.3f\n", 
            g_latency_us_min, g_latency_us_max, g_latency_us_avg);

    spdk_env_fini();
    return 0;
}
