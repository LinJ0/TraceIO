#include <spdk/trace.h>
#include <spdk/string.h>

#ifndef SPDK_TRACE_H
#define SPDK_TRACE_H

/**
 * For enable spdk trace tool.
 *
 * \param app_name that must equal to env_opts.name or app_opts.name.
 * \param tpoint_group_name to specific one of more tracepoints
 *        e.g. "nvme_pcie,bdev" to enable tracpoint nvme_pcie and bdev (string without any space)
 * \return 0 on success, else non-zero indicates a failure.
 */

int enable_spdk_trace(const char *app_name, const char *tpoint_group_name);

#endif
