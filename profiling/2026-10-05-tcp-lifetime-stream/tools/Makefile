CC ?= cc
CLANG_FORMAT ?= clang-format
CPPFLAGS ?=
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=
LIBBPF_CFLAGS ?= $(patsubst -I%,-isystem %,$(shell pkg-config --cflags-only-I libbpf 2>/dev/null))
LIBBPF_LIBS ?= $(shell pkg-config --libs libbpf 2>/dev/null)
STAGING_DIR ?=
comma := ,
STAGING_CFLAGS := $(if $(STAGING_DIR),-isystem $(STAGING_DIR)/usr/include)
STAGING_LDFLAGS := $(if $(STAGING_DIR),-L$(STAGING_DIR)/usr/lib -Wl$(comma)-rpath-link$(comma)$(STAGING_DIR)/usr/lib)

WARNINGS := -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror
ROOT := ../..
BUILD_DIR ?= $(ROOT)/build/tcp-lifetime
TARGET := $(BUILD_DIR)/runner
SOURCES := runner.c $(ROOT)/src/tcpdelay/capture.c \
	$(ROOT)/src/tcpdelay/estimator.c $(ROOT)/src/tcpdelay/lifetime.c \
	$(ROOT)/src/common/error.c $(ROOT)/src/common/helpers.c log-shim.c

.PHONY: all clean format
all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(@D)
	$(CC) -I$(ROOT)/src $(CPPFLAGS) $(STAGING_CFLAGS) $(LIBBPF_CFLAGS) \
		$(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) $(STAGING_LDFLAGS) \
		-o $@ $(SOURCES) $(LDLIBS) $(LIBBPF_LIBS) -lbpf -lelf -lz

format:
	$(CLANG_FORMAT) -i runner.c log-shim.c

clean:
	rm -rf $(BUILD_DIR)
