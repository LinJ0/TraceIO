#include "trace_io.h"

int
enable_spdk_trace(const char *app_name, const char *tpoint_group_name)
{
    bool error_found = false;

    /* generate spdk trace file in /dev/shm/ */
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, (int)getpid());
    if (spdk_trace_init(shm_name, SPDK_DEFAULT_NUM_TRACE_ENTRIES) != 0) {
        return -1;
    } 

    if (tpoint_group_name == NULL) {
        return 0;
    }

    char *tpoint_group_mask_str = NULL;
    tpoint_group_mask_str = strdup(tpoint_group_name);
    if (tpoint_group_mask_str == NULL) {
        fprintf(stderr, "Unable to get string of tpoint group mask from opts.\n");
        return -1;
    }

    char *tpoint_group_str = NULL;
    char *tpoint_group, *tpoints;
    uint64_t tpoint_group_mask;
    uint64_t tpoint_mask = -1ULL;

    /* Save a pointer to the original value of the tpoint group mask string
     * to free later, because spdk_strsepq() modifies given char*. */
    char *tp_g_str = tpoint_group_mask_str;
    char *end = NULL;

    while ((tpoint_group_str = spdk_strsepq(&tpoint_group_mask_str, ",")) != NULL) {
        if (strchr(tpoint_group_str, ':')) {
            /* Get the tpoint group mask */
            tpoint_group = spdk_strsepq(&tpoint_group_str, ":");
            /* Get the tpoint mask inside that group */
            tpoints = spdk_strsepq(&tpoint_group_str, ":");

            tpoint_group_mask = strtoull(tpoint_group, &end, 16);
            if (*end != '\0') {
                tpoint_group_mask = spdk_trace_create_tpoint_group_mask(tpoint_group);
                if (tpoint_group_mask == 0) {
                    error_found = true;
                    break;
                }
            }
            /* Check if tpoint group mask has only one bit set.
             * This is to avoid enabling individual tpoints in
             * more than one tracepoint group at once. */
            if (!spdk_u64_is_pow2(tpoint_group_mask)) {
                fprintf(stderr, "Tpoint group mask: %s contains multiple tpoint groups.\n", tpoint_group);
                fprintf(stderr, "This is not supported, to prevent from activating tpoints by mistake.\n");
                error_found = true;
                break;
            }

            tpoint_mask = strtoull(tpoints, &end, 16);
            if (*end != '\0') {
                error_found = true;
                break;
            }
        } else {
            tpoint_group_mask = strtoull(tpoint_group_str, &end, 16);
            if (*end != '\0') {
                tpoint_group_mask = spdk_trace_create_tpoint_group_mask(tpoint_group_str);
                if (tpoint_group_mask == 0) {
                    error_found = true;
                    break;
                }
            }
            tpoint_mask = -1ULL;
        }
        for (uint64_t group_id = 0; group_id < SPDK_TRACE_MAX_GROUP_ID; ++group_id) {
            if (tpoint_group_mask & (1 << group_id)) {
                spdk_trace_set_tpoints(group_id, tpoint_mask);
            }
        }
    }

    if (error_found) {
        fprintf(stderr, "invalid tpoint mask %s\n", tpoint_group_name);
        free(tp_g_str);
        return -1;
    } else {
        printf("Tracepoint Group Mask %s specified.\n", tpoint_group_name);
        printf("Use 'spdk_trace -s %s -p %d' to capture a snapshot of events at runtime.\n",
            app_name, getpid());
#if defined(__linux__)
        printf("Or copy /dev/shm%s for offline analysis/debug.\n", shm_name);
#endif
    }

    free(tp_g_str);
    return 0;
}

