CC ?= cc

CPPFLAGS ?=
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

TARGET := sqm-mon
OBJECTS := main.o cake.o config.o controller.o error.o latency.o log.o monitor.o netlink.o traffic.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS) -luci -lubox

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJECTS) $(OBJECTS:.o=.d)

-include $(OBJECTS:.o=.d)
