#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "../include/trace_io.h"

#include <map>

extern "C" {
#include "spdk/trace_parser.h"
#include "spdk/util.h"
}

#define ENTRY_MAX 10000 /* number of sizeof(struct bin_file_data) */

static struct spdk_trace_parser *g_parser;
static const struct spdk_trace_flags *g_flags;
static char *g_exe_name;
static bool g_debug_enable = false;
static uint64_t g_tsc_base = 0;
static uint64_t g_tsc_rate = 0;

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

static void
process_output_file(struct spdk_trace_parser_entry *entry, FILE *fptr)
{
    struct spdk_trace_entry *e = entry->entry;
    const struct spdk_trace_tpoint *d = &g_flags->tpoint[e->tpoint_id];
    struct bin_file_data buffer; 
    buffer.lcore = entry->lcore;
    buffer.tsc_rate = g_tsc_rate;
    buffer.tsc_timestamp = e->tsc - g_tsc_base;    
    buffer.obj_id = e->object_id;
    
    if (!d->new_object && d->object_type != OBJECT_NONE) {
        buffer.tsc_sc_time = e->tsc - entry->object_start;
        buffer.obj_start = entry->object_start - g_tsc_base;
    } else {
        buffer.tsc_sc_time = 0;
        buffer.obj_start = entry->object_start - g_tsc_base;
    }

    //snprintf(buffer.tpoint_name, sizeof(buffer.tpoint_name), d->name);
    strcpy(buffer.tpoint_name, d->name);
    
    if (strcmp(buffer.tpoint_name, "NVME_IO_SUBMIT") == 0) {
        for (size_t i = 1; i < d->num_args; ++i) {
            if (strcmp(d->args[i].name, "opc") == 0) {
                buffer.opc = (uint16_t)(entry->args[i].integer & UINT8BIT_MASK);
            } else if (strcmp(d->args[i].name, "cid") == 0) {
                buffer.cid = (uint16_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "nsid") == 0) { 
                buffer.nsid = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw10") == 0) { 
                buffer.cdw10 = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw11") == 0) { 
                buffer.cdw11 = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw12") == 0) { 
                buffer.cdw12 = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw13") == 0) { 
                buffer.cdw13 = (uint32_t)entry->args[i].integer;
            } else {
                buffer.cpl = 0;
            }
        }
    } else if (strcmp(buffer.tpoint_name, "NVME_IO_COMPLETE") == 0) {
        for (size_t i = 1; i < d->num_args; ++i) {
            if (strcmp(d->args[i].name, "cid") == 0) {
                buffer.cid = entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cpl") == 0) {
                buffer.cpl = entry->args[i].integer;
            } else {
                buffer.opc = 0;
                buffer.nsid = 0;
                buffer.cdw10 = 0;
                buffer.cdw11 = 0;
                buffer.cdw12 = 0;
                buffer.cdw13 = 0;
            }
        }
    } else {
        fprintf(stderr, "parse trace fail\n");
        exit(1);
    }
    fwrite(&buffer, sizeof(struct bin_file_data), 1, fptr);
}

static void
usage(void)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "   %s <option> <lcore#>\n", g_exe_name);
    fprintf(stderr, "   '-c' to display single lcore history\n");
    fprintf(stderr, "   '-s' to specify spdk_trace shm name for a currently running process\n");                
    fprintf(stderr, "   '-i' to specify the shared memory ID\n");
    fprintf(stderr, "   '-p' to specify the trace PID\n");
    fprintf(stderr, "        If -s is specified, then one of\n");
    fprintf(stderr, "        -i or -p must be specified)\n");
    fprintf(stderr, "   '-f' to specify a tracepoint file name\n");
    fprintf(stderr, "        (-s and -f are mutually exclusive)\n");
    fprintf(stderr, "   '-o' to produce output file and specify output file name.\n");
    fprintf(stderr, "   '-d' debug to view the content of output file.\n");
}

int
main(int argc, char **argv)
{
    int op;
    const char *app_name = NULL;
    const char *input_file_name = NULL;
    char shm_name[64];
    int shm_id = -1, shm_pid = -1;
    int lcore = SPDK_TRACE_MAX_LCORE;

    g_exe_name = argv[0];
    while ((op = getopt(argc, argv, "c:f:i:p:s:td")) != -1) {
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
            input_file_name = optarg;
            break;
        case 'd':
            g_debug_enable = true;
            break;
        default:
            usage();
            exit(1);
        }
    }

    if (input_file_name != NULL && app_name != NULL) {
        fprintf(stderr, "-f and -s are mutually exclusive\n");
        usage();
        exit(1);
    }

    if (input_file_name == NULL && app_name == NULL) {
        fprintf(stderr, "One of -f and -s must be specified\n");
        usage();
        exit(1);
    }

    /* 
     * input file in /dev/shm/ 
     */
    if (!input_file_name) {
        if (shm_id >= 0) {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
        } else {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
        }
        input_file_name = shm_name;
    }

    /*  
     * output file in current path
     */   
    char output_file_name[68] = {0}; 
    if (!input_file_name) {
        if (shm_id >= 0)
            snprintf(output_file_name, sizeof(output_file_name), "/%s_%d.bin", app_name, shm_id);
        else
            snprintf(output_file_name, sizeof(output_file_name), "/%s_pid%d.bin", app_name, shm_pid);
    } else {
        snprintf(output_file_name, sizeof(output_file_name), "%s.bin", input_file_name);
    }   
    
    FILE *fptr;
    if (output_file_name) {
        fptr = fopen(output_file_name, "wb");
        if (fptr == NULL) {
            fprintf(stderr, "Failed to open output file %s\n", output_file_name);
            return -1; 
        }
        printf("Output .bin file: %s\n", output_file_name);
    }   

    struct spdk_trace_parser_opts opts;
    opts.filename = input_file_name;
    opts.lcore = lcore;
    opts.mode = app_name == NULL ? SPDK_TRACE_PARSER_MODE_FILE : SPDK_TRACE_PARSER_MODE_SHM;
    g_parser = spdk_trace_parser_init(&opts);
    if (g_parser == NULL) {
        fprintf(stderr, "Failed to initialize trace parser\n");
        exit(1);
    }

    g_flags = spdk_trace_parser_get_flags(g_parser);
    g_tsc_rate = g_flags->tsc_rate;
    printf("TSC Rate: %ju\n", g_tsc_rate);

    uint64_t entry_count;
    for (int i = 0; i < SPDK_TRACE_MAX_LCORE; ++i) {
        if (lcore == SPDK_TRACE_MAX_LCORE || i == lcore) {
            entry_count = spdk_trace_parser_get_entry_count(g_parser, i);
            if (entry_count > 0) {
                printf("Trace Size of lcore (%d): %ju\n", i, entry_count);
            }
        }
    }

    const struct spdk_trace_tpoint *d;
    struct spdk_trace_parser_entry entry;
    while (spdk_trace_parser_next_entry(g_parser, &entry)) {
        d = &g_flags->tpoint[entry.entry->tpoint_id];
        if (strcmp(d->name, "NVME_IO_SUBMIT") != 0 && strcmp(d->name, "NVME_IO_COMPLETE") != 0) {
            continue;
        } else if (entry.args[0].integer != 0) { 
            continue;   
        } else if (entry.object_start & (uint64_t)1 << 63) {
            continue;
        }

        /* g_tsc_base = tsc of first io cmd entry */
        if (g_tsc_base == 0) {
            g_tsc_base = entry.entry->tsc;
        }   
        if (entry.entry->tsc < g_tsc_base) {
            continue;
        }   

        /* write trace to output file */
        process_output_file(&entry, fptr);
    }
    fclose(fptr);

    if (g_debug_enable) {
        printf("Debug mode enabled\n");
        fptr = fopen(output_file_name, "rb");
        if (fptr == NULL) {
            fprintf(stderr, "Failed to open output file %s\n", output_file_name);
            return -1; 
        }

        fseek(fptr, 0, SEEK_END);
        size_t file_size = ftell(fptr);
        rewind(fptr);
        size_t total_entry = file_size / sizeof(struct bin_file_data);
        size_t remain_entry = total_entry;
        printf("total_entry = %ld\n", total_entry);

        while (!feof(fptr) && remain_entry) {
            //printf("remain_entry = %ld\n", remain_entry);
            size_t buffer_entry = (remain_entry > ENTRY_MAX) ? ENTRY_MAX : remain_entry;
            remain_entry -= buffer_entry;
            struct bin_file_data buffer[buffer_entry];

            size_t read_entry = fread(&buffer, sizeof(struct bin_file_data), buffer_entry, fptr);
            //printf("read_entry = %ld\n", read_entry);
            if (buffer_entry != read_entry) {
                fprintf(stderr, "Fail to read input file\n");
            }
            
            for (size_t i = 0; i < read_entry; i++) {
                printf("tsc_timestamp: %20ld  ", buffer[i].tsc_timestamp);
                printf("tpoint_name: %-16s  ", buffer[i].tpoint_name);
                //printf("lcore: %d  ", buffer[i].lcore);
                //printf("tsc_rate: %ld  ", buffer[i].tsc_rate);
                //printf("cid: %3d  ", buffer[i].cid);
                //printf("obj_id: %ld  ", buffer[i].obj_id);
                printf("tsc_sc_time: %15ld  ", buffer[i].tsc_sc_time);
                printf("obj_start_time: %15ld  ", buffer[i].obj_start);
                //printf("nsid: %d  ", buffer[i].nsid);
                //printf("cpl: %d  ", buffer[i].cpl);
                printf("opc: 0x%2x  ", buffer[i].opc); 
                printf("cdw10: 0x%x  ", buffer[i].cdw10);
                printf("cdw11: 0x%x  ", buffer[i].cdw11);
                printf("cdw12: 0x%x  ", buffer[i].cdw12);
                printf("cdw13: 0x%x  ", buffer[i].cdw13);
                printf("\n");
            }
        }
        fclose(fptr);
    }

    spdk_trace_parser_cleanup(g_parser);

    return (0);
}

