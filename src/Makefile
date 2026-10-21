CC ?= cc

CPPFLAGS ?=
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=
NETLINK_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libnl-3.0 2>/dev/null))
NETLINK_LIBS ?= $(shell pkg-config --libs libnl-3.0 2>/dev/null)
# libbpf headers are not -Wpedantic clean, so they are included as system headers.
LIBBPF_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libbpf 2>/dev/null))
# The socket filter is built only when a BPF compiler is given; the OpenWrt
# package passes bpf.mk's clang, target and strip tool, and the uapi headers.
BPF_CLANG ?=
BPF_TARGET ?= bpfel
BPF_STRIP ?=
BPF_INCLUDES ?=
# The OpenWrt package passes PKG_VERSION-rPKG_RELEASE.
VERSION ?= unknown
# The package variant: cake-adapt 0/0, cake-adapt-mqtt 1/0, cake-adapt-mqtt-ssl 1/1.
# Without MQTT the no-op sender replaces mqtt.c and libmosquitto is not linked.
# The default is cake-adapt's, as in common/constants.h.
MQTT_SUPPORTED ?= 0
MQTT_TLS_SUPPORTED ?= 0
MQTT_SELECTION := $(MQTT_SUPPORTED)$(MQTT_TLS_SUPPORTED)
ifneq ($(filter 00 10 11,$(MQTT_SELECTION)),$(MQTT_SELECTION))
$(error MQTT_SUPPORTED/MQTT_TLS_SUPPORTED must be 0/0, 1/0 or 1/1)
endif
ifeq ($(MQTT_SUPPORTED),1)
MQTT_SOURCE := logging/mqtt.c
MQTT_LIBS := -lmosquitto
else
MQTT_SOURCE := logging/mqtt_none.c
MQTT_LIBS :=
endif

WARNINGS := \
	-Wall \
	-Wextra \
	-Wpedantic \
	-Wformat=2 \
	-Wshadow \
	-Wconversion \
	-Werror

TARGET := cake-adapt
SOURCES := \
	main.c \
	cake/cake.c \
	common/error.c \
	common/helpers.c \
	config/config.c \
	config/defaults.c \
	config/validate.c \
	controller/controller.c \
	controller/reflector.c \
	latency/fping.c \
	latency/irtt.c \
	latency/icmp.c \
	latency/native.c \
	latency/latency.c \
	latency/parser.c \
	latency/tracker.c \
	logging/log.c \
	$(MQTT_SOURCE) \
	monitor/control.c \
	monitor/monitor.c \
	monitor/links.c \
	monitor/pingers.c \
	monitor/reflectors.c \
	monitor/tcpdelay.c \
	monitor/status.c \
	monitor/publish.c \
	platform/cpu.c \
	platform/memory.c \
	platform/netlink.c \
	platform/traffic.c \
	tcpdelay/capture.c \
	tcpdelay/estimator.c \
	tcpdelay/injector.c
OBJECT_DIR ?= ../build
OBJECTS := $(SOURCES:%.c=$(OBJECT_DIR)/%.o)
BPF_OBJECT := $(OBJECT_DIR)/tcpdelay.bpf.o
DEPENDENCIES := $(OBJECTS:.o=.d) $(BPF_OBJECT:.o=.d)
MQTT_BUILD_CONFIG := $(OBJECT_DIR)/mqtt.config
MQTT_FLAGS := -DMQTT_SUPPORTED=$(MQTT_SUPPORTED) -DMQTT_TLS_SUPPORTED=$(MQTT_TLS_SUPPORTED)

.PHONY: all clean

all: $(TARGET) $(if $(BPF_CLANG),$(BPF_OBJECT))

# A rerun without cleaning after a change of the MQTT flags rebuilds every object.
.PHONY: FORCE_MQTT_CONFIG
$(MQTT_BUILD_CONFIG): FORCE_MQTT_CONFIG
	@mkdir -p $(@D)
	@printf '%s\n' '$(MQTT_FLAGS)' | cmp -s - $@ || printf '%s\n' '$(MQTT_FLAGS)' > $@

$(OBJECTS): $(MQTT_BUILD_CONFIG)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -pthread -o $@ $(OBJECTS) $(LDLIBS) -luci -lubus -lubox -lz -lbpf $(MQTT_LIBS) $(NETLINK_LIBS)

# Subsystem headers are included relative to this source root.
$(OBJECT_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -I$(CURDIR) -DPROGRAM_VERSION='"$(VERSION)"' $(MQTT_FLAGS) $(CPPFLAGS) $(NETLINK_CFLAGS) $(OBJECT_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

$(OBJECT_DIR)/tcpdelay/capture.o $(OBJECT_DIR)/tcpdelay/injector.o: OBJECT_CFLAGS := $(LIBBPF_CFLAGS)

# Kernel BPF objects carry BTF for libbpf, but not the DWARF debug sections.
$(BPF_OBJECT): tcpdelay/tcpdelay.bpf.c
	@mkdir -p $(@D)
	$(BPF_CLANG) -O2 -g -target $(BPF_TARGET) -I$(CURDIR) -I$(CURDIR)/tcpdelay/include $(BPF_INCLUDES) -Wall -Wextra -Werror -MMD -MP -c -o $@ $<
	$(if $(BPF_STRIP),$(BPF_STRIP) --strip-debug $@)
	@# LLVM 20+ compiles code it proves unreachable to a call of the __bpf_trap
	@# kfunc; loading it needs kernel BTF, which OpenWrt kernels do not ship.
	@if grep -q __bpf_trap $@; then echo "$@ references __bpf_trap" >&2; rm -f $@; exit 1; fi

clean:
	$(RM) $(TARGET) $(OBJECTS) $(BPF_OBJECT) $(DEPENDENCIES) $(MQTT_BUILD_CONFIG)

-include $(DEPENDENCIES)
