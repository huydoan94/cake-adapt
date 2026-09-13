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

TARGETS := test_controller test_latency test_log test_traffic

.PHONY: all check clean

all: $(TARGETS)

check: $(TARGETS)
	./test_controller
	./test_latency
	./test_log
	./test_traffic

test_latency: test_latency.o latency.o error.o
	$(CC) $(LDFLAGS) -o $@ test_latency.o latency.o error.o $(LDLIBS)

test_traffic: test_traffic.o traffic.o
	$(CC) $(LDFLAGS) -o $@ test_traffic.o traffic.o $(LDLIBS)

test_controller: test_controller.o controller.o
	$(CC) $(LDFLAGS) -o $@ test_controller.o controller.o $(LDLIBS)

test_log: test_log.o log.o
	$(CC) $(LDFLAGS) -o $@ test_log.o log.o $(LDLIBS)

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

traffic.o: ../src/traffic.c ../src/traffic.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -MMD -MP -c -o $@ ../src/traffic.c

clean:
	$(RM) $(TARGETS) *.o *.d

-include *.d
