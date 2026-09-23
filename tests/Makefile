CC ?= cc
.DEFAULT_GOAL := all

CPPFLAGS ?= -I../src
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

TARGETS := test_controller test_latency test_log test_traffic test_cpu test_helpers
OBJECT_DIR ?= ../build/tests
OBJECT_NAMES := $(TARGETS) controller latency log traffic cpu error helpers helpers_log
OBJECTS := $(addprefix $(OBJECT_DIR)/,$(addsuffix .o,$(OBJECT_NAMES)))

# Optional host integration check using the same libnl API as libnl-tiny.
NETLINK_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libnl-3.0))
NETLINK_LIBS ?= $(shell pkg-config --libs libnl-3.0)
UCI_CFLAGS ?= $(shell pkg-config --cflags uci 2>/dev/null)

# Pure configuration validation needs UCI headers, but no native libuci build.
.PHONY: check-config
check-config: test_config
	./test_config

test_config: test_config.c ../src/config.c ../src/config.h ../src/latency.c ../src/log.c ../src/error.c ../src/helpers.c ../src/helpers.h
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 $(LDFLAGS) -Wl,--gc-sections -o $@ test_config.c ../src/latency.c ../src/log.c ../src/error.c ../src/helpers.c $(LDLIBS) -lz

.PHONY: check-netlink
check-netlink: test_cake test_netlink
	./test_cake
	./test_netlink

test_cake: test_cake.c ../src/cake.c ../src/cake.h ../src/netlink.c ../src/netlink.h ../src/error.c ../src/helpers.c
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ test_cake.c ../src/netlink.c ../src/error.c ../src/helpers.c $(LDLIBS) $(NETLINK_LIBS)

test_netlink: test_netlink.c ../src/netlink.c ../src/netlink.h ../src/helpers.h ../src/error.c
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ test_netlink.c ../src/error.c $(LDLIBS) $(NETLINK_LIBS)

.PHONY: all check clean

all: $(TARGETS)

check: $(TARGETS)
	./test_controller
	./test_latency
	./test_log
	./test_traffic
	./test_cpu
	./test_helpers
	sh ./test_init.sh
	sh ./test_import.sh

test_latency: $(addprefix $(OBJECT_DIR)/,test_latency.o latency.o error.o helpers.o)

test_traffic: $(addprefix $(OBJECT_DIR)/,test_traffic.o traffic.o helpers.o)

test_controller: $(addprefix $(OBJECT_DIR)/,test_controller.o controller.o helpers.o)

test_log: $(addprefix $(OBJECT_DIR)/,test_log.o log.o helpers_log.o)
	$(CC) $(LDFLAGS) -Wl,--wrap=syslog,--wrap=__syslog_chk,--wrap=open_memstream,--wrap=fclose -o $@ $^ $(LDLIBS) -lz

test_cpu: $(addprefix $(OBJECT_DIR)/,test_cpu.o cpu.o error.o helpers.o)

test_helpers: $(addprefix $(OBJECT_DIR)/,test_helpers.o helpers.o)

$(filter-out test_log,$(TARGETS)):
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJECT_DIR):
	mkdir -p $@

$(OBJECT_DIR)/%.o: %.c | $(OBJECT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

$(OBJECT_DIR)/%.o: ../src/%.c | $(OBJECT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

$(OBJECT_DIR)/helpers_log.o: ../src/helpers.c | $(OBJECT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Dclock_gettime=test_log_clock_gettime -std=c11 -MMD -MP -c -o $@ $<

clean:
	$(RM) $(TARGETS) test_cake test_netlink test_config $(OBJECTS) $(OBJECTS:.o=.d)

-include $(OBJECTS:.o=.d)
