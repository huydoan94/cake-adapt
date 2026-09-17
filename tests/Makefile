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

test_latency: test_latency.o latency.o error.o helpers.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_traffic: test_traffic.o traffic.o helpers.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_controller: test_controller.o controller.o helpers.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_log: test_log.o log.o helpers_log.o
	$(CC) $(LDFLAGS) -Wl,--wrap=syslog,--wrap=__syslog_chk -o $@ $^ $(LDLIBS) -lz

test_cpu: test_cpu.c ../src/cpu.c ../src/cpu.h ../src/error.c ../src/helpers.c ../src/helpers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ test_cpu.c ../src/cpu.c ../src/error.c ../src/helpers.c $(LDLIBS)

test_helpers: test_helpers.c helpers.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_latency.o: test_latency.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

test_traffic.o: test_traffic.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

test_controller.o: test_controller.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

test_log.o: test_log.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

controller.o: ../src/controller.c ../src/controller.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/controller.c

latency.o: ../src/latency.c ../src/latency.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/latency.c

error.o: ../src/error.c ../src/error.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/error.c

log.o: ../src/log.c ../src/log.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/log.c

helpers.o: ../src/helpers.c ../src/helpers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/helpers.c

helpers_log.o: ../src/helpers.c ../src/helpers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Dclock_gettime=test_log_clock_gettime -std=c11 -MMD -MP -c -o $@ ../src/helpers.c

traffic.o: ../src/traffic.c ../src/traffic.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/traffic.c

clean:
	$(RM) $(TARGETS) test_cake test_netlink test_config *.o *.d

-include *.d
