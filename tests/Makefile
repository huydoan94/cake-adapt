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
TARGETS := test_controller test_replay test_reflector test_latency test_parser test_tracker test_log test_traffic test_cpu test_memory test_helpers test_defaults test_estimator test_accounting test_inject test_mqtt_none
BINARIES := $(addprefix $(BINARY_DIR)/,$(TARGETS))

test_controller_OBJECTS := controller/test_controller.o src/controller/controller.o src/common/helpers.o
test_replay_OBJECTS := controller/test_replay.o src/controller/controller.o src/common/helpers.o
test_reflector_OBJECTS := controller/test_reflector.o src/controller/reflector.o src/latency/tracker.o src/common/helpers.o
LATENCY_OBJECTS := src/latency/latency.o src/latency/fping.o src/latency/irtt.o src/latency/parser.o
test_latency_OBJECTS := latency/test_latency.o $(LATENCY_OBJECTS) src/common/error.o src/common/helpers.o
test_parser_OBJECTS := latency/test_parser.o src/latency/parser.o src/common/helpers.o
test_tracker_OBJECTS := latency/test_tracker.o src/latency/tracker.o src/common/helpers.o
test_log_OBJECTS := logging/test_log.o logging/mqtt_stub.o src/logging/log.o src/common/helpers_log.o
test_mqtt_none_OBJECTS := logging/test_mqtt_none.o src/logging/mqtt_none.o src/common/error.o
test_traffic_OBJECTS := platform/test_traffic.o src/platform/traffic.o src/common/helpers.o
test_cpu_OBJECTS := platform/test_cpu.o src/platform/cpu.o src/common/error.o src/common/helpers.o
test_memory_OBJECTS := platform/test_memory.o src/platform/memory.o src/common/error.o
test_helpers_OBJECTS := common/test_helpers.o src/common/helpers.o
test_defaults_OBJECTS := config/test_defaults.o src/config/defaults.o
test_estimator_OBJECTS := tcpdelay/test_estimator.o src/tcpdelay/estimator.o
test_accounting_OBJECTS := cake/test_accounting.o
test_inject_OBJECTS := tcpdelay/test_inject.o

OBJECTS := $(sort $(addprefix $(OBJECT_DIR)/,$(foreach target,$(TARGETS),$($(target)_OBJECTS))))

# Optional host integration check using the same libnl API as libnl-tiny.
NETLINK_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libnl-3.0))
NETLINK_LIBS ?= $(shell pkg-config --libs libnl-3.0)
UCI_CFLAGS ?= $(shell pkg-config --cflags uci 2>/dev/null)
MQTT_CFLAGS ?= $(shell pkg-config --cflags libmosquitto 2>/dev/null)
MQTT_LIBS ?= $(shell pkg-config --libs libmosquitto 2>/dev/null)
# The sender and driver are built as the MQTT variants build them; MQTT_TLS_SUPPORTED=1 is
# cake-adapt-mqtt-ssl's. check-mqtt runs the queue tests in both.
MQTT_TLS_SUPPORTED ?= 0
ifneq ($(filter 0 1,$(MQTT_TLS_SUPPORTED)),$(MQTT_TLS_SUPPORTED))
$(error MQTT_TLS_SUPPORTED must be 0 or 1)
endif
MQTT_BUILD_CONFIG := $(OBJECT_DIR)/mqtt-tls.config

.PHONY: FORCE_MQTT_CONFIG
$(MQTT_BUILD_CONFIG): FORCE_MQTT_CONFIG
	@mkdir -p $(@D)
	@printf '%s\n' '$(MQTT_TLS_SUPPORTED)' | cmp -s - $@ || printf '%s\n' '$(MQTT_TLS_SUPPORTED)' > $@

$(BINARY_DIR)/mqtt_driver: $(MQTT_BUILD_CONFIG)

.PHONY: check-mqtt
check-mqtt: $(BINARY_DIR)/test_mqtt $(BINARY_DIR)/test_mqtt_tls
	$(BINARY_DIR)/test_mqtt
	$(BINARY_DIR)/test_mqtt_tls

MQTT_TEST_SOURCES := logging/test_mqtt.c $(SOURCE_DIR)/common/helpers.c $(SOURCE_DIR)/common/error.c
MQTT_TEST_DEPENDENCIES := $(MQTT_TEST_SOURCES) $(SOURCE_DIR)/logging/mqtt.c $(SOURCE_DIR)/logging/mqtt.h $(SOURCE_DIR)/config/config.h

$(BINARY_DIR)/test_mqtt: MQTT_TEST_FLAGS := -DMQTT_SUPPORTED=1 -DMQTT_TLS_SUPPORTED=0
$(BINARY_DIR)/test_mqtt_tls: MQTT_TEST_FLAGS := -DMQTT_SUPPORTED=1 -DMQTT_TLS_SUPPORTED=1
$(BINARY_DIR)/test_mqtt $(BINARY_DIR)/test_mqtt_tls: $(MQTT_TEST_DEPENDENCIES)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(MQTT_TEST_FLAGS) $(MQTT_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -pthread $(LDFLAGS) -o $@ $(MQTT_TEST_SOURCES) $(LDLIBS) $(MQTT_LIBS)

$(BINARY_DIR)/mqtt_driver: logging/mqtt_driver.c $(SOURCE_DIR)/logging/mqtt.c $(SOURCE_DIR)/logging/log.c $(SOURCE_DIR)/common/helpers.c $(SOURCE_DIR)/common/error.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DMQTT_SUPPORTED=1 -DMQTT_TLS_SUPPORTED=$(MQTT_TLS_SUPPORTED) $(MQTT_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -pthread $(LDFLAGS) -o $@ $< $(SOURCE_DIR)/logging/log.c $(SOURCE_DIR)/common/helpers.c $(SOURCE_DIR)/common/error.c $(LDLIBS) $(MQTT_LIBS) -lz

# The monitor needs UCI/libubox and the filter seam needs libbpf headers.
# UCI_CFLAGS may expose all three from the SDK staged include directory.
.PHONY: check-tcpdelay
check-tcpdelay: $(BINARY_DIR)/test_monitor_tcpdelay $(BINARY_DIR)/test_monitor_reflectors $(BINARY_DIR)/test_filter_accounting $(BINARY_DIR)/test_monitor_status $(BINARY_DIR)/test_monitor_publish
	$(BINARY_DIR)/test_monitor_tcpdelay
	$(BINARY_DIR)/test_monitor_reflectors
	$(BINARY_DIR)/test_filter_accounting
	$(BINARY_DIR)/test_monitor_status
	$(BINARY_DIR)/test_monitor_publish

# libbpf map-definition macros require GNU typeof, as in the BPF build.
$(BINARY_DIR)/test_filter_accounting: tcpdelay/test_filter_accounting.c $(SOURCE_DIR)/tcpdelay/tcpdelay.bpf.c $(SOURCE_DIR)/tcpdelay/record.h $(SOURCE_DIR)/cake/accounting.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=gnu11 $(LDFLAGS) -Wl,--gc-sections -o $@ $< $(LDLIBS)

$(BINARY_DIR)/test_monitor_reflectors: monitor/test_reflectors.c $(SOURCE_DIR)/monitor/reflectors.c $(SOURCE_DIR)/monitor/loop.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 $(LDFLAGS) -Wl,--gc-sections -o $@ $< $(LDLIBS)

$(BINARY_DIR)/test_monitor_tcpdelay: monitor/test_tcpdelay.c $(SOURCE_DIR)/monitor/tcpdelay.c $(SOURCE_DIR)/monitor/links.c $(SOURCE_DIR)/monitor/loop.h $(SOURCE_DIR)/tcpdelay/record.h $(SOURCE_DIR)/tcpdelay/capture.h $(SOURCE_DIR)/tcpdelay/injector.h $(SOURCE_DIR)/tcpdelay/inject.h $(SOURCE_DIR)/cake/accounting.h $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 $(LDFLAGS) -Wl,--gc-sections -o $@ $< $(SOURCE_DIR)/common/helpers.c $(LDLIBS)

$(BINARY_DIR)/test_monitor_status: monitor/test_status.c $(SOURCE_DIR)/monitor/status.c $(SOURCE_DIR)/monitor/loop.h $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 $(LDFLAGS) -Wl,--gc-sections -o $@ $< $(SOURCE_DIR)/common/helpers.c $(LDLIBS)

# libubus and blobmsg are replaced by the test's recorders.
$(BINARY_DIR)/test_monitor_publish: monitor/test_publish.c $(SOURCE_DIR)/monitor/publish.c $(SOURCE_DIR)/monitor/loop.h $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 -pthread $(LDFLAGS) -Wl,--gc-sections -o $@ $< $(SOURCE_DIR)/common/helpers.c $(LDLIBS)

# Pure configuration validation needs UCI headers, but no native libuci build.
# It is built once per package variant: without MQTT, with MQTT, and with MQTT and TLS.
CONFIG_SOURCES := $(addprefix $(SOURCE_DIR)/,config/defaults.c latency/latency.c latency/fping.c latency/irtt.c latency/parser.c logging/log.c common/error.c common/helpers.c) logging/mqtt_stub.c
CONFIG_TESTS := $(BINARY_DIR)/test_config $(BINARY_DIR)/test_config_mqtt $(BINARY_DIR)/test_config_mqtt_ssl

.PHONY: check-config
check-config: $(CONFIG_TESTS)
	set -e; for binary in $(CONFIG_TESTS); do $$binary; done

$(BINARY_DIR)/test_config: CONFIG_FLAGS := -DMQTT_SUPPORTED=0 -DMQTT_TLS_SUPPORTED=0
$(BINARY_DIR)/test_config_mqtt: CONFIG_FLAGS := -DMQTT_SUPPORTED=1 -DMQTT_TLS_SUPPORTED=0
$(BINARY_DIR)/test_config_mqtt_ssl: CONFIG_FLAGS := -DMQTT_SUPPORTED=1 -DMQTT_TLS_SUPPORTED=1
$(CONFIG_TESTS): config/test_config.c $(SOURCE_DIR)/config/config.c $(SOURCE_DIR)/config/config.h $(SOURCE_DIR)/config/validate.c $(CONFIG_SOURCES)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CONFIG_FLAGS) $(UCI_CFLAGS) $(CFLAGS) $(WARNINGS) -ffunction-sections -fdata-sections -std=c11 $(LDFLAGS) -Wl,--gc-sections -o $@ config/test_config.c $(CONFIG_SOURCES) $(LDLIBS) -lz

.PHONY: check-netlink
check-netlink: $(BINARY_DIR)/test_cake $(BINARY_DIR)/test_netlink
	$(BINARY_DIR)/test_cake
	$(BINARY_DIR)/test_netlink

$(BINARY_DIR)/test_cake: cake/test_cake.c $(SOURCE_DIR)/cake/cake.c $(SOURCE_DIR)/cake/cake.h $(SOURCE_DIR)/platform/netlink.c $(SOURCE_DIR)/platform/netlink.h $(SOURCE_DIR)/common/error.c $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ cake/test_cake.c $(SOURCE_DIR)/platform/netlink.c $(SOURCE_DIR)/common/error.c $(SOURCE_DIR)/common/helpers.c $(LDLIBS) $(NETLINK_LIBS)

$(BINARY_DIR)/test_netlink: platform/test_netlink.c $(SOURCE_DIR)/platform/netlink.c $(SOURCE_DIR)/platform/netlink.h $(SOURCE_DIR)/common/helpers.h $(SOURCE_DIR)/common/error.c $(SOURCE_DIR)/common/helpers.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ platform/test_netlink.c $(SOURCE_DIR)/common/error.c $(SOURCE_DIR)/common/helpers.c $(LDLIBS) $(NETLINK_LIBS)

.PHONY: all check clean

all: $(BINARIES)

check: $(BINARIES)
	set -e; for binary in $(BINARIES); do $$binary; done
	sh ./test_init.sh
	sh ./test_import.sh
	sh ./test_luci.sh
	sh ./test_docs.sh
	sh ./test_style.sh
	sh ./test_wait.sh
	python3 tools/test_sdk_build.py

.PHONY: check-sdk-build
check-sdk-build:
	python3 tools/test_sdk_build.py

.SECONDEXPANSION:
$(BINARIES): $(BINARY_DIR)/%: $$(addprefix $(OBJECT_DIR)/,$$($$*_OBJECTS))
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) $(TEST_LDFLAGS) -o $@ $^ $(LDLIBS) $(TEST_LDLIBS)

# Kept apart from LDFLAGS so command-line overrides cannot drop the wrappers.
$(BINARY_DIR)/test_controller: TEST_LDFLAGS := -Wl,--wrap=calloc
$(BINARY_DIR)/test_latency: TEST_LDFLAGS := -Wl,--wrap=access
$(BINARY_DIR)/test_log: TEST_LDFLAGS := -Wl,--wrap=syslog,--wrap=__syslog_chk,--wrap=open_memstream,--wrap=fclose,--wrap=localtime_r
$(BINARY_DIR)/test_log: TEST_LDLIBS := -lz

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
