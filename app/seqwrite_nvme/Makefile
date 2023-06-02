#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../spdk)
ROOT_DIR := $(abspath $(CURDIR)/../..)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

CFLAGS += -O3 -I$(ROOT_DIR)/include
C_SRCS := $(wildcard $(ROOT_DIR)/lib/*.c) $(wildcard ./*.c)
LIB += -L$(ROOT_DIR)/lib

SPDK_LIB_LIST = $(ALL_MODULES_LIST)

APP := seqwrite_nvme

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
