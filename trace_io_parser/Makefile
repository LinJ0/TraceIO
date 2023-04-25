#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(CURDIR)/../../spdk
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = trace_io_parser
C_SRCS := trace_io_parser.c

SPDK_LIB_LIST = event

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
