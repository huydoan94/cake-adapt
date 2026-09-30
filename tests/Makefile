CC ?= cc
.DEFAULT_GOAL := all

SOURCE_DIR := ../src
CPPFLAGS ?= -I$(SOURCE_DIR)
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=

WARNINGS := \
	-Wall \
	-Wextra \
	-Wpedantic \
	-Wformat=2 \
	-Wshadow \
	-Wconversion \
	-Werror

# Test sources mirror src/; objects and binaries stay below build/.
OBJECT_DIR ?= ../build/tests
BINARY_DIR := $(OBJECT_DIR)/bin
TARGETS := test_controller test_reflector test_latency test_log test_traffic test_cpu test_helpers test_defaults
BINARIES := $(addprefix $(BINARY_DIR)/,$(TARGETS))

test_controller_OBJECTS := controller/test_controller.o src/controller/controller.o src/common/helpers.o
test_reflector_OBJECTS := controller/test_reflector.o src/controller/reflector.o src/latency/latency.o src/common/error.o src/common/helpers.o
test_latency_OBJECTS := latency/test_latency.o src/latency/latency.o src/common/error.o src/common/helpers.o
test_log_OBJECTS := logging/test_log.o src/logging/log.o src/common/helpers_log.o
test_traffic_OBJECTS := platform/test_traffic.o src/platform/traffic.o src/common/helpers.o
test_cpu_OBJECTS := platform/test_cpu.o src/platform/cpu.o src/common/error.o src/common/helpers.o
test_helpers_OBJECTS := common/test_helpers.o src/common/helpers.o
test_defaults_OBJECTS := config/test_defaults.o src/config/defaults.o

OBJECTS := $(sort $(addprefix $(OBJECT_DIR)/,$(foreach target,$(TARGETS),$($(target)_OBJECTS))))

# Optional host integration check using the same libnl API as libnl-tiny.
NETLINK_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libnl-3.0))
NETLINK_LIBS ?= $(shell pkg-config --libs libnl-3.0)
UCI_CFLAGS ?= $(shell pkg-config --cflags uci 2>/dev/null)

# Pure configuration validation needs UCI headers, but no native libuci build.
CONFIG_SOURCES := $(addprefix $(SOURCE_DIR)/,config/defaults.c latency/latency.c logging/log.c common/error.c common/helpers.c)

.PHONY: check-config
check-config: $(BINARY_DIR)/test_config
	$(BINARY_DIR)/test_config

$(BINARY_DIR)/test_config: config/test_config.c $(SOURCE_DIR)/config/config.c $(SOURCE_DIR)/config/config.h $(CONFIG_SOURCES)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 $(LDFLAGS) -Wl,--gc-sections -o $@ config/test_config.c $(CONFIG_SOURCES) $(LDLIBS) -lz

.PHONY: check-netlink
check-netlink: $(BINARY_DIR)/test_cake $(BINARY_DIR)/test_netlink
	$(BINARY_DIR)/test_cake
	$(BINARY_DIR)/test_netlink

$(BINARY_DIR)/test_cake: cake/test_cake.c $(SOURCE_DIR)/cake/cake.c $(SOURCE_DIR)/cake/cake.h $(SOURCE_DIR)/platform/netlink.c $(SOURCE_DIR)/platform/netlink.h $(SOURCE_DIR)/common/error.c $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ cake/test_cake.c $(SOURCE_DIR)/platform/netlink.c $(SOURCE_DIR)/common/error.c $(SOURCE_DIR)/common/helpers.c $(LDLIBS) $(NETLINK_LIBS)

$(BINARY_DIR)/test_netlink: platform/test_netlink.c $(SOURCE_DIR)/platform/netlink.c $(SOURCE_DIR)/platform/netlink.h $(SOURCE_DIR)/common/helpers.h $(SOURCE_DIR)/common/error.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ platform/test_netlink.c $(SOURCE_DIR)/common/error.c $(LDLIBS) $(NETLINK_LIBS)

.PHONY: all check clean

all: $(BINARIES)

check: $(BINARIES)
	set -e; for binary in $(BINARIES); do $$binary; done
	sh ./test_init.sh
	sh ./test_import.sh

.SECONDEXPANSION:
$(BINARIES): $(BINARY_DIR)/%: $$(addprefix $(OBJECT_DIR)/,$$($$*_OBJECTS))
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BINARY_DIR)/test_log: LDFLAGS += -Wl,--wrap=syslog,--wrap=__syslog_chk,--wrap=open_memstream,--wrap=fclose
$(BINARY_DIR)/test_log: LDLIBS += -lz

$(OBJECT_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

$(OBJECT_DIR)/src/%.o: $(SOURCE_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

# test_log controls time by replacing the helper module's clock source.
$(OBJECT_DIR)/src/common/helpers_log.o: $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Dclock_gettime=test_log_clock_gettime -std=c11 -MMD -MP -c -o $@ $<

clean:
	$(RM) -r $(OBJECT_DIR)

-include $(OBJECTS:.o=.d)
