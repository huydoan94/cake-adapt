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
	latency/latency.c \
	latency/parser.c \
	latency/tracker.c \
	logging/log.c \
	monitor/control.c \
	monitor/monitor.c \
	monitor/links.c \
	monitor/pingers.c \
	monitor/reflectors.c \
	monitor/tcpdelay.c \
	platform/cpu.c \
	platform/netlink.c \
	platform/traffic.c \
	tcpdelay/capture.c \
	tcpdelay/estimator.c \
	tcpdelay/lifetime.c
OBJECT_DIR ?= ../build
OBJECTS := $(SOURCES:%.c=$(OBJECT_DIR)/%.o)
BPF_OBJECT := $(OBJECT_DIR)/tcpdelay.bpf.o
DEPENDENCIES := $(OBJECTS:.o=.d) $(BPF_OBJECT:.o=.d)

.PHONY: all clean

all: $(TARGET) $(if $(BPF_CLANG),$(BPF_OBJECT))

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS) -luci -lubox -lz -lbpf $(NETLINK_LIBS)

# Subsystem headers are included relative to this source root.
$(OBJECT_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -I$(CURDIR) -DPROGRAM_VERSION='"$(VERSION)"' $(CPPFLAGS) $(NETLINK_CFLAGS) $(OBJECT_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

$(OBJECT_DIR)/tcpdelay/capture.o: OBJECT_CFLAGS := $(LIBBPF_CFLAGS)

# Kernel BPF objects carry BTF for libbpf, but not the DWARF debug sections.
$(BPF_OBJECT): tcpdelay/tcpdelay.bpf.c
	@mkdir -p $(@D)
	$(BPF_CLANG) -O2 -g -target $(BPF_TARGET) -I$(CURDIR) $(BPF_INCLUDES) -Wall -Wextra -Werror -MMD -MP -c -o $@ $<
	$(if $(BPF_STRIP),$(BPF_STRIP) --strip-debug $@)

clean:
	$(RM) $(TARGET) $(OBJECTS) $(BPF_OBJECT) $(DEPENDENCIES)

-include $(DEPENDENCIES)
