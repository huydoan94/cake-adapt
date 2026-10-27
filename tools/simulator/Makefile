# Builds the closed-loop simulator against the real controller and tracker.
CC ?= cc
CFLAGS ?= -O2
OBJECT_DIR ?= ../../build/tools/simulator
SOURCE_DIR := ../../src

WARNINGS := -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Werror
SOURCES := simulate.c $(SOURCE_DIR)/controller/controller.c $(SOURCE_DIR)/controller/activity.c $(SOURCE_DIR)/controller/congestion.c $(SOURCE_DIR)/controller/coupling.c $(SOURCE_DIR)/controller/rate.c $(SOURCE_DIR)/latency/tracker.c $(SOURCE_DIR)/common/clock.c $(SOURCE_DIR)/common/rate.c

.PHONY: all run clean
all: $(OBJECT_DIR)/simulate

$(OBJECT_DIR)/simulate: $(SOURCES) $(wildcard $(SOURCE_DIR)/*/*.h)
	@mkdir -p $(@D)
	$(CC) -I$(SOURCE_DIR) $(CFLAGS) $(WARNINGS) -std=c11 -o $@ $(SOURCES) -lm

run: $(OBJECT_DIR)/simulate
	$(OBJECT_DIR)/simulate
	$(OBJECT_DIR)/simulate --owd

clean:
	$(RM) $(OBJECT_DIR)/simulate
