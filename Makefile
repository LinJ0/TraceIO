SHELL_HACK := $(shell mkdir -p bin obj)

# Compiler
CC := gcc
CXX := g++

# Directories
ROOT_DIR := $(abspath $(CURDIR))
OBJ_DIR := $(ROOT_DIR)/obj
BIN_DIR := $(ROOT_DIR)/obj
TEST_DIR := $(ROOT_DIR)/test

TARGETS := trace_io_analysis trace_io_record trace_io_replay trace_app
TEST_TARGETS := 

.PHONY: all $(TARGETS) test $(TEST_TARGETS) clean

all: $(TARGETS)

$(TARGETS):
	@ $(MAKE) -C $@
	@ mv $@/$@.o $(OBJ_DIR)
	@ mv $@/$@ $(BIN_DIR)

clean:
	@ rm -rf $(OBJ_DIR) $(BIN_DIR)
