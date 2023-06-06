SHELL_HACK := $(shell mkdir -p bin obj)
export SPDK_ROOT_DIR=/home/znsvm/spdk
# Directories
APP_DIR := $(abspath $(CURDIR))
APP_OBJ_DIR := $(APP_DIR)/obj
APP_BIN_DIR := $(APP_DIR)/bin

APP_TARGETS := seqio_nvme

.PHONY: all $(APP_TARGETS) clean

all: $(APP_TARGETS)

$(APP_TARGETS):
	@ $(MAKE) -C $@
	@ mv $@/$@.o $(APP_OBJ_DIR)
	@ mv $@/$@ $(APP_BIN_DIR)

clean:
	@ rm -rf $(APP_OBJ_DIR) $(APP_BIN_DIR)
