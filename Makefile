SHELL_HACK := $(shell mkdir -p bin obj)
export SPDK_ROOT_DIR=/home/znsvm/spdk

# Compiler
CC := gcc
CXX := g++

# Directories
ROOT_DIR := $(abspath $(CURDIR))
OBJ_DIR := $(ROOT_DIR)/obj
BIN_DIR := $(ROOT_DIR)/bin
APP_DIR := $(ROOT_DIR)/app

TARGETS := trace_io_analysis trace_io_record trace_io_replay #trace_app

.PHONY: all $(TARGETS) app clean

all: $(TARGETS)

$(TARGETS):
	@ $(MAKE) -C $@
	@ mv $@/$@.o $(OBJ_DIR)
	@ mv $@/$@ $(BIN_DIR)

app:
	$(MAKE) -C $(APP_DIR)

clean:
	@ rm -rf $(OBJ_DIR) $(BIN_DIR)
