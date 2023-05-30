#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/bdev_zone.h"

struct request_context_t {
    char *bdev_name;
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *bdev_desc;
    struct spdk_io_channel *bdev_io_channel;
    char *buff;
    uint32_t buff_size;
    struct spdk_bdev_io_wait_entry bdev_io_wait;
};
uint64_t g_tick;
/* info about bdev device */
uint64_t g_num_blk = 0;
uint32_t g_block_size = 0;
/* info about zone */
struct spdk_bdev_zone_info zone_info = {0};
uint64_t g_num_zone = 0;
uint64_t g_zone_capacity = 0;
uint64_t g_zone_sz_blk = 0;
uint32_t g_max_open_zone = 0;
uint32_t g_max_active_zone = 0;
uint32_t g_max_append_blk = 0;
/* io request */
uint64_t g_num_io = 0;
uint64_t g_num_io_zone = 0;

static void
usage(void)
{
    printf(" -c <.json> JSON file of the bdev device\n");
    printf(" -b <bdev> name of the bdev to use\n");
    printf(" -z <number> name of zones to send io request\n");
}

static char *g_bdev_name = "Malloc0"; /* Default bdev name if without -b */
static int
parse_arg(int ch, char *optarg)
{
    switch (ch) {
    case 'b':
        g_bdev_name = optarg;
        break;
    case 'z':
        g_num_io_zone = atol(optarg);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static void
queue_io_wait_with_cb(struct request_context_t *req_context, spdk_bdev_io_wait_cb cb_fn)
{
    req_context->bdev_io_wait.bdev = req_context->bdev;
    req_context->bdev_io_wait.cb_fn = cb_fn;
    req_context->bdev_io_wait.cb_arg = req_context;
    spdk_bdev_queue_io_wait(req_context->bdev, req_context->bdev_io_channel,
                    &req_context->bdev_io_wait);
}

static void
appstop_error(struct request_context_t *req_context)
{
	spdk_put_io_channel(req_context->bdev_io_channel);
    spdk_bdev_close(req_context->bdev_desc);
    spdk_app_stop(-1);
}

static void
appstop_success(struct request_context_t *req_context)
{
	spdk_put_io_channel(req_context->bdev_io_channel);
    spdk_bdev_close(req_context->bdev_desc);
    spdk_app_stop(0);
}

/* read start 
uint64_t r_complete = 0;

static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct request_context_t *req_context = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (success) {
        r_complete++;
        //SPDK_NOTICELOG("Read string from bdev : %s\n", req_context->buff);
    } else {
        SPDK_ERRLOG("bdev io read error\n");
        appstop_error(req_context);
    }

    if (r_complete == g_num_io) {
        printf("Read bdev complete\n");
        appstop_success(req_context);
    }

}

static void
read_bdev(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;

    printf("Reading the bdev...\n");
    
    uint64_t num_blocks = 1;
    for (uint64_t i = 0, offset_blocks = 0; i < g_num_io; i++, offset_blocks++) {
        // Zero the buffer so that we can use it for reading 
        memset(req_context->buff, 0, req_context->buff_size);
        rc = spdk_bdev_read_blocks(req_context->bdev_desc, req_context->bdev_io_channel,
                    req_context->buff, offset_blocks, num_blocks, 
                    read_complete, req_context);

        if (rc == -ENOMEM) {
            SPDK_NOTICELOG("Queueing io\n");
            queue_io_wait_with_cb(req_context, read_bdev);
        } else if (rc) {
            SPDK_ERRLOG("%s error while reading from bdev: %d\n", spdk_strerror(-rc), rc);
            appstop_error(req_context);
        }
    }
   
}
 read end */

/* write start 
uint64_t w_complete = 0;

static void
write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct request_context_t *req_context = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (success) {
        w_complete++;
    } else {
        SPDK_ERRLOG("bdev io write error: %d\n", EIO);
        appstop_error(req_context);
        return;
    }

    if (w_complete == g_num_io) {
        printf("Write bdev complete\n");
        read_bdev(req_context);
    }
}

static void
write_bdev(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;

    printf("Writing to the bdev...\n");

    g_num_io = g_num_blk;
    uint64_t num_blocks = 1;
    for (uint64_t i = 0, offset_blocks = 0; i < g_num_io; i++, offset_blocks++) {
        rc = spdk_bdev_write_blocks(req_context->bdev_desc, req_context->bdev_io_channel,
                                req_context->buff, offset_blocks, num_blocks, 
                                write_complete, req_context);

        if (rc == -ENOMEM) {
            SPDK_NOTICELOG("Queueing io\n");
            queue_io_wait_with_cb(req_context, write_bdev);
        } else if (rc) {
            SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
            appstop_error(req_context);
        }
    }
}
 write end */

/* read zone start */
uint64_t rz_complete = 0;

static void
read_zone_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct request_context_t *req_context = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (success) {
        rz_complete++;
        //printf("%s", req_context->buff);
    } else {
        SPDK_ERRLOG("bdev io read error\n");
        appstop_error(req_context);
    }

    if (rz_complete == g_num_io) {
        printf("Read bdev complete\n");
        appstop_success(req_context);
    }

}

static void
read_zone(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;

    printf("Reading the bdev...\n");
   
    uint64_t num_blocks = 1;
/*    uint64_t offset_blocks = 0;

    for (uint32_t zone = 0; zone < g_num_io_zone; zone++) {
        offset_blocks = zone * g_zone_sz_blk;
        for (; offset_blocks < zone * g_zone_sz_blk + g_zone_capacity; offset_blocks++) {        
            // Zero the buffer so that we can use it for reading 
            memset(req_context->buff, 0, req_context->buff_size);
            rc = spdk_bdev_read_blocks(req_context->bdev_desc, req_context->bdev_io_channel,
                    req_context->buff, offset_blocks, num_blocks, 
                    read_zone_complete, req_context);
            if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                queue_io_wait_with_cb(req_context, read_zone);
            } else if (rc) {
                SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
                appstop_error(req_context);
            }   
        }
    }  
*/

    for (uint64_t i = 0, offset_blocks = 0; i < g_num_io; i++, offset_blocks++) {
        // Zero the buffer so that we can use it for reading 
        memset(req_context->buff, 0, req_context->buff_size);
        rc = spdk_bdev_read_blocks(req_context->bdev_desc, req_context->bdev_io_channel,
                    req_context->buff, offset_blocks, num_blocks, 
                    read_zone_complete, req_context);

        if (rc == -ENOMEM) {
            SPDK_NOTICELOG("Queueing io\n");
            queue_io_wait_with_cb(req_context, read_zone);
        } else if (rc) {
            SPDK_ERRLOG("%s error while reading from bdev: %d\n", spdk_strerror(-rc), rc);
            appstop_error(req_context);
        }
    }
   
}
/* read zone end */

/* append zone start */
uint64_t az_complete = 0;

static void
append_zone_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct request_context_t *req_context = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (success) {
        az_complete++;
    } else {
        SPDK_ERRLOG("bdev io append error: %d\n", EIO);
        appstop_error(req_context);
        return;
    }

    if (az_complete == g_num_io) {
        printf("Append bdev complete...\n");
        read_zone(req_context);
        //appstop_success(req_context);
    }
}

static void
append_zone(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;

    printf("Append to the bdev...\n");
   
    if (g_max_active_zone == 0) {
        g_num_io = g_num_zone * g_zone_capacity;
    } else {
        g_num_io = g_num_io_zone * g_zone_capacity;
    }

    uint64_t zone_id = 0;
    uint64_t num_blocks = 1;
/*    uint64_t offset_blocks = 0;

    for (uint32_t zone = 0; zone < g_num_io_zone; zone++) {
        offset_blocks = zone * g_zone_sz_blk;
        for (; offset_blocks < zone * g_zone_sz_blk + g_zone_capacity; offset_blocks++) {
            zone_id =spdk_bdev_get_zone_id(req_context->bdev, offset_blocks);
            rc = spdk_bdev_zone_append(req_context->bdev_desc, req_context->bdev_io_channel,
                                req_context->buff, zone_id, num_blocks, 
                                append_zone_complete, req_context);
            if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                queue_io_wait_with_cb(req_context, append_zone);
            } else if (rc) {
                SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
                appstop_error(req_context);
            }   
        }
    }  
*/

    for (uint64_t i = 0, offset_blocks = 0; i < g_num_io; i++, offset_blocks++) {
        zone_id =spdk_bdev_get_zone_id(req_context->bdev, offset_blocks);
        //printf("offset_blocks = 0x%lx, zone_id=0x%lx\n", offset_blocks, zone_id);
        rc = spdk_bdev_zone_append(req_context->bdev_desc, req_context->bdev_io_channel,
                                req_context->buff, zone_id, num_blocks, 
                                append_zone_complete, req_context);
        if (rc == -ENOMEM) {
            SPDK_NOTICELOG("Queueing io\n");
            queue_io_wait_with_cb(req_context, append_zone);
        } else if (rc) {
            SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
            appstop_error(req_context);
        }   
    }
   
}
/* append zone end */

/* reset zone start */
uint64_t reset_complete = 0;

static void
reset_zone_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct request_context_t *req_context = cb_arg;

    /* Complete the I/O */
    spdk_bdev_free_io(bdev_io);
    
    if (success) {
		reset_complete++;
	} else {
        SPDK_ERRLOG("bdev io reset zone error: %d\n", EIO);
        appstop_error(req_context);
        return;
	}

    if (reset_complete == g_num_zone) {
        printf("Reset all zone complete\n");
        append_zone(req_context);
    }    
}

static void
reset_zone(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;

    printf("Reset all zone...\n");

    for (uint64_t i = 0; i < g_num_zone; i++) {
        rc = spdk_bdev_zone_management(req_context->bdev_desc, req_context->bdev_io_channel,
                       i * g_zone_sz_blk, SPDK_BDEV_ZONE_RESET, 
                       reset_zone_complete, req_context);

        if (rc == -ENOMEM) {
            SPDK_NOTICELOG("Queueing io\n");
            queue_io_wait_with_cb(req_context, reset_zone);
        } else if (rc) {
            SPDK_ERRLOG("%s error while resetting zone: %d\n", spdk_strerror(-rc), rc);
            appstop_error(req_context);
        }
    }
}
/* reset zone end */

/* get zone info start */
static void
get_zone_info_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct request_context_t *req_context = cb_arg;

    /* Complete the I/O */
    spdk_bdev_free_io(bdev_io);
    
    if (success) {
        printf("Get zone info complete\n");

        g_zone_capacity = zone_info.capacity;
        g_num_zone = spdk_bdev_get_num_zones(req_context->bdev);
        g_zone_sz_blk = spdk_bdev_get_zone_size(req_context->bdev);
        g_max_open_zone = spdk_bdev_get_max_open_zones(req_context->bdev);
        g_max_active_zone = spdk_bdev_get_max_active_zones(req_context->bdev);
        g_max_append_blk = spdk_bdev_get_max_zone_append_size(req_context->bdev);
        printf("[zone info]\n");
        printf("num zone: %lu zones\n", g_num_zone);
        printf("zone size: %lu blocks\n", g_zone_sz_blk);
        printf("zone capacity: %lu blocks\n", g_zone_capacity);
        printf("max open zone: %u zones\n", g_max_open_zone);
        printf("max active zone: %u zones\n", g_max_active_zone);
        printf("max append size: %u blocks\n", g_max_append_blk);

    } else {
        SPDK_ERRLOG("bdev io reset zone error: %d\n", EIO);
        appstop_error(req_context);
        return;
    } 
    reset_zone(req_context); 
}

static void
get_zone_info(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;

    printf("Get zone info...\n");

    /* get first zone to know zone capacity */
    uint64_t zone_id = 0x4000;
    size_t num_zones = 1;
    rc = spdk_bdev_get_zone_info(req_context->bdev_desc, req_context->bdev_io_channel,
                                zone_id, num_zones, &zone_info,
                                get_zone_info_complete, req_context);
    
    if (rc == -ENOMEM) {
        SPDK_NOTICELOG("Queueing io\n");
        queue_io_wait_with_cb(req_context, get_zone_info);
    } else if (rc) {
        SPDK_ERRLOG("%s error while get zone_info: %d\n", spdk_strerror(-rc), rc);
        appstop_error(req_context);
    }
}
/* get zone info end */


static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		    void *event_ctx)
{
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static void
appstart(void *arg)
{
    struct request_context_t *req_context = arg;
    int rc = 0;
    req_context->bdev = NULL;
    req_context->bdev_desc = NULL;

    SPDK_NOTICELOG("Successfully started the application\n");

    /* sleep 20sec to open spdk_trace_record.
     * spdk_trace_record is a spdk build-in trace tool 
     * that can collect more trace than spdk_trace
     */
    sleep(20);

    /*  Get bdev descriptor to open the bdev by calling spdk_bdev_open_ext() with its name */
    SPDK_NOTICELOG("Opening the bdev %s\n", req_context->bdev_name);
    rc = spdk_bdev_open_ext(req_context->bdev_name, true, bdev_event_cb, NULL,
                            &req_context->bdev_desc);
    if (rc) {
        SPDK_ERRLOG("Could not open bdev: %s\n", req_context->bdev_name);
        spdk_app_stop(-1);
        return;
    }

    /* A bdev pointer is valid while the bdev is opened */
    req_context->bdev = spdk_bdev_desc_get_bdev(req_context->bdev_desc);

    /* Open I/O channel */
    SPDK_NOTICELOG("Opening io channel\n");
    req_context->bdev_io_channel = spdk_bdev_get_io_channel(req_context->bdev_desc);
    if (req_context->bdev_io_channel == NULL) {
        SPDK_ERRLOG("Could not create bdev I/O channel!!\n");
        spdk_bdev_close(req_context->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    /* Get bdev device info */
    g_num_blk = spdk_bdev_get_num_blocks(req_context->bdev);
    g_block_size = spdk_bdev_get_block_size(req_context->bdev);

    /* Allocate memory for the write buffer.
     * Initialize the write buffer with the string "Hello World!"
     */
    uint32_t buf_align = spdk_bdev_get_buf_align(req_context->bdev);
    req_context->buff_size = g_block_size * spdk_bdev_get_write_unit_size(req_context->bdev);
    req_context->buff = spdk_zmalloc(req_context->buff_size, buf_align, NULL,
                    SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

    if (!req_context->buff) {
        SPDK_ERRLOG("Failed to allocate buffer\n");
        appstop_error(req_context);
        return;
    }
    snprintf(req_context->buff, req_context->buff_size, "%s", "Hello World!\n");

    if (spdk_bdev_is_zoned(req_context->bdev)) {
        get_zone_info(req_context);
        return;
	}

	//write_bdev(req_context);

}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc = 0;
    struct request_context_t req_context = {};

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "seqwrite";

    /* Parse built-in SPDK command line parameters to enable spdk trace*/
    if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:z:", NULL, parse_arg,
                      usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }
    req_context.bdev_name = g_bdev_name;

    rc = spdk_app_start(&opts, appstart, &req_context);
    if (rc) {
        SPDK_ERRLOG("ERROR starting application\n");
    }

    spdk_free(req_context.buff);
    spdk_app_fini();
    return rc;
}
