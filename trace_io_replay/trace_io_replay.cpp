#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include <map>

extern "C" {
#include "spdk/trace_parser.h"
#include "spdk/util.h"
}

static struct spdk_trace_parser *g_parser;
static const struct spdk_trace_flags *g_flags;

struct ctrlr_entry {
	struct spdk_nvme_ctrlr      *ctrlr;
	TAILQ_ENTRY(ctrlr_entry)    link;
	char				        name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr      *ctrlr;
	struct spdk_nvme_ns         *ns;
	TAILQ_ENTRY(ns_entry)       link;
	struct spdk_nvme_qpair      *qpair;
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};

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


// haven't modify yet
static void
process_event(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_offset)
{
    struct spdk_trace_entry         *e = entry->entry;
	const struct spdk_trace_tpoint  *d;
	float                           us;
	size_t                          i;

	d = &g_flags->tpoint[e->tpoint_id];
	us = get_us_from_tsc(e->tsc - tsc_offset, tsc_rate);

	printf("%2d: %10.3f ", entry->lcore, us);
	if (g_print_tsc) {
		printf("(%9ju) ", e->tsc - tsc_offset);
	}
	if (g_flags->owner[d->owner_type].id_prefix) {
		printf("%c%02d ", g_flags->owner[d->owner_type].id_prefix, e->poller_id);
	} else {
		printf("%4s", " ");
	}

	printf("%-*s ", (int)sizeof(d->name), d->name);
	print_size(e->size);

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

	for (i = 0; i < d->num_args; ++i) {
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
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
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

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link); /* Find NVMe Controller */

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

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
nvme_cleanup(void)
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


static void
usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "   %s <option> <lcore#>\n", g_exe_name);
	fprintf(stderr, "         '-c' to display single lcore history\n");
	fprintf(stderr, "         '-s' to specify spdk_trace shm name for a\n");
	fprintf(stderr, "              currently running process\n");
	fprintf(stderr, "         '-i' to specify the shared memory ID\n");
	fprintf(stderr, "         '-p' to specify the trace PID\n");
	fprintf(stderr, "              (If -s is specified, then one of\n");
	fprintf(stderr, "               -i or -p must be specified)\n");
	fprintf(stderr, "         '-f' to specify a tracepoint file name\n");
	fprintf(stderr, "              (-s and -f are mutually exclusive)\n");
}

int
main(int argc, char **argv)
{
	struct spdk_trace_parser_opts   trace_opts;
	struct spdk_trace_parser_entry  entry;
	int                             lcore = SPDK_TRACE_MAX_LCORE;
	uint64_t                        tsc_offset;
	const char                      *app_name = NULL;
	const char                      *file_name = NULL;
	int                             op, i, rc;
	char                            shm_name[64];
	int                             shm_id = -1, shm_pid = -1;
	struct spdk_env_opts            env_opts;

    /* 
     * Initial Trace Parser
     */
	g_exe_name = argv[0];
	while ((op = getopt(argc, argv, "c:f:i:p:s:")) != -1) {
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

	trace_opts.filename = file_name;
	trace_opts.lcore = lcore;
	trace_opts.mode = app_name == NULL ? SPDK_TRACE_PARSER_MODE_FILE : SPDK_TRACE_PARSER_MODE_SHM;
	g_parser = spdk_trace_parser_init(&trace_opts);
	if (g_parser == NULL) {
		fprintf(stderr, "Failed to initialize trace parser\n");
		exit(1);
	}

	g_flags = spdk_trace_parser_get_flags(g_parser);
	tsc_offset = spdk_trace_parser_get_tsc_offset(g_parser);
    printf("Initialization Trace Complete\n");
	
    /* 
     * Initial env for NVMe
     */
    spdk_env_opts_init(&env_opts);
    env_opts.name = "trace_io_replay";
    if (spdk_env_init(&env_opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

    rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);  
    if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}
    
    if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}
    
    printf("Initialization complete.\n");

    /*  
     * Process event
     */
    while (spdk_trace_parser_next_entry(g_parser, &entry)) {
		if (entry.entry->tsc < tsc_offset) {
			continue;
		}
		process_event(&entry, g_flags->tsc_rate, tsc_offset);
	}
    
exit:
	nvme_cleanup();
	spdk_env_fini();
    spdk_trace_parser_cleanup(g_parser);
	return rc;
}
