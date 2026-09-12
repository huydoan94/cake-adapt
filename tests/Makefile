CC ?= cc

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

TARGETS := test_traffic

.PHONY: all check clean

all: $(TARGETS)

check: $(TARGETS)
	./test_traffic

test_traffic: test_traffic.o traffic.o
	$(CC) $(LDFLAGS) -o $@ test_traffic.o traffic.o $(LDLIBS)

test_traffic.o: test_traffic.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ $<

traffic.o: ../src/traffic.c ../src/traffic.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/traffic.c

clean:
	$(RM) $(TARGETS) *.o *.d

-include *.d
