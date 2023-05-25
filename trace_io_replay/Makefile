#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(CURDIR)/../../spdk
TRACE_IO_ROOT_DIR := $(abspath $(CURDIR)/..)

SPDK_LIB_LIST =  $(ALL_MODULES_LIST) event event_bdev

TRACE_IO_LIB_DIR := $(TRACE_IO_ROOT_DIR)/lib/
TRACE_IO_LIBS := $(wildcard $(TRACE_IO_LIB_DIR)*.c)
LIBS += $(TRACE_IO_LIBS)

APP = trace_io_replay

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk
