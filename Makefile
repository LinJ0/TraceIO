SHELL_HACK := $(shell mkdir -p bin obj)
OBJ_DIR := $(abspath $(CURDIR)/obj)
BIN_DIR := $(abspath $(CURDIR)/bin)

TARGETS := trace_io_analysis trace_io_record trace_io_replay

.PHONY: all $(TARGETS) clean

all: $(TARGETS)

$(TARGETS):
	@ $(MAKE) -C $@
	@ mv $@/$@.o $(OBJ_DIR)
	@ mv $@/$@ $(BIN_DIR)

clean:
	@ rm -rf $(OBJ_DIR) $(BIN_DIR)
