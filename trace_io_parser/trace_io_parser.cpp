#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/nvme_spec.h"

#include <map>

extern "C" {
#include "spdk/trace_parser.h"
#include "spdk/util.h"
}

static struct spdk_trace_parser *g_parser;
static const struct spdk_trace_flags *g_flags;
static bool g_print_tsc = false;

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
    NVME_ZNS_OPC_ZONE_APPEND = 0x79,
    NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND = 0x7A,
    NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV = 0x7D,
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
	 * Print arg as signed, since -1 is a common value especially
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
print_nvme_io_op(uint64_t arg)
{
    switch (arg) {
    case NVME_OPC_FLUSH:
        printf("%-15.15s ", "flush");
        break;
    case NVME_OPC_WRITE:
        printf("%-15.15s ", "write");
        break;
    case NVME_OPC_READ:
        printf("%-15.15s ", "read");
        break;
    case NVME_OPC_WRITE_UNCORRECTABLE:
        printf("%-15.15s ", "write_uncorrectable");
        break;
    case NVME_OPC_COMPARE:
        printf("%-15.15s ", "compare");
        break;
    case NVME_OPC_WRITE_ZEROES:
        printf("%-15.15s ", "write_zeroes");
        break;
    case NVME_OPC_DATASET_MANAGEMENT:
        printf("%-15.15s ", "dataset_management");
        break;
    case NVME_OPC_VERIFY:
        printf("%-15.15s ", "verify");
        break;
    case NVME_OPC_RESERVATION_REGISTER: 
        printf("%-15.15s ", "reservation_register");
        break; 
    case NVME_OPC_RESERVATION_REPORT: 
        printf("%-15.15s ", "reservation_report");
        break;
    case NVME_OPC_RESERVATION_ACQUIRE: 
        printf("%-15.15s ", "reservation_acquire");
        break;
    case NVME_OPC_RESERVATION_RELEASE:
        printf("%-15.15s ", "reservation_release");
        break;
    case NVME_ZNS_OPC_ZONE_APPEND:
        printf("%-15.15s ", "zone_append");
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_SEND:
        printf("%-15.15s ", "zone_mgmt_send");
        break;
    case NVME_ZNS_OPC_ZONE_MANAGEMENT_RECV:
        printf("%-15.15s ", "zone_mgmt_recv");
        break;
    }
}

static void
process_event(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_offset)
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

	for (i = 1; i < d->num_args; ++i) {
        if (i == 1) {
            print_nvme_io_op(entry->args[i].integer);
            continue;
        }

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
	int				lcore = SPDK_TRACE_MAX_LCORE;
	uint64_t			tsc_offset, entry_count;
	const char			*app_name = NULL;
	const char			*file_name = NULL;
	int				op, i;
	char				shm_name[64];
	int				shm_id = -1, shm_pid = -1;
	const struct spdk_trace_tpoint	*d;
    struct spdk_trace_entry     *e;
    const char tp_name[] = "NVME_IO_SUBMIT";


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
    
        if (strcmp(d->name, tp_name) != 0) {
            continue;
        } else if (strcmp(d->name, tp_name) == 0 && entry.args[0].integer) { //admin == true
            continue;   
        }
        process_event(&entry, g_flags->tsc_rate, tsc_offset);
    }   

	spdk_trace_parser_cleanup(g_parser);

	return (0);
}
