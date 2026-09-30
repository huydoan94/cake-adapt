CC ?= cc

CPPFLAGS ?=
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=
NETLINK_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libnl-3.0 2>/dev/null))
NETLINK_LIBS ?= $(shell pkg-config --libs libnl-3.0 2>/dev/null)

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
	monitor.c \
	cake.c \
	config/config.c \
	controller.c \
	cpu.c \
	config/defaults.c \
	latency.c \
	logging/log.c \
	netlink.c \
	traffic.c \
	common/error.c \
	common/helpers.c
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
	$(CC) -I$(CURDIR) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJECTS) $(DEPENDENCIES)

-include $(DEPENDENCIES)
