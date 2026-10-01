CC ?= cc

CPPFLAGS ?=
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=
NETLINK_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libnl-3.0 2>/dev/null))
NETLINK_LIBS ?= $(shell pkg-config --libs libnl-3.0 2>/dev/null)
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
	monitor/observe.c \
	monitor/pingers.c \
	monitor/reflectors.c \
	platform/cpu.c \
	platform/netlink.c \
	platform/traffic.c
OBJECT_DIR ?= ../build
OBJECTS := $(SOURCES:%.c=$(OBJECT_DIR)/%.o)
DEPENDENCIES := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS) -luci -lubox -lz $(NETLINK_LIBS)

# Subsystem headers are included relative to this source root.
$(OBJECT_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -I$(CURDIR) -DPROGRAM_VERSION='"$(VERSION)"' $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJECTS) $(DEPENDENCIES)

-include $(DEPENDENCIES)
