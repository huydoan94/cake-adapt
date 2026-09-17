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
	cake.c \
	config.c \
	controller.c \
	cpu.c \
	error.c \
	helpers.c \
	latency.c \
	log.c \
	monitor.c \
	netlink.c \
	traffic.c
OBJECT_DIR ?= ../build
OBJECTS := $(SOURCES:%.c=$(OBJECT_DIR)/%.o)
DEPENDENCIES := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS) -luci -lubox -lz $(NETLINK_LIBS)

$(OBJECT_DIR):
	mkdir -p $@

$(OBJECT_DIR)/%.o: %.c | $(OBJECT_DIR)
	$(CC) $(CPPFLAGS) $(NETLINK_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJECTS) $(DEPENDENCIES)

-include $(DEPENDENCIES)
