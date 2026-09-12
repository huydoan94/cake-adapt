#define _XOPEN_SOURCE 700

#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void read_log(
    const char *path,
    char *contents,
    size_t contents_size
)
{
    FILE *file;
    size_t bytes_read;

    file = fopen(path, "r");
    assert(file != NULL);
    bytes_read = fread(contents, 1U, contents_size - 1U, file);
    assert(!ferror(file));
    contents[bytes_read] = '\0';
    assert(fclose(file) == 0);
}

static void test_debug_logging_to_file(void)
{
    char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
    char contents[256];
    int descriptor;

    descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);

    log_init("sqm-mon-test", false);
    assert(log_set_level("debug") == 0);
    assert(log_set_file(path) == 0);
    log_message(LOG_LEVEL_DEBUG, "sample value=%d", 42);
    log_close();

    read_log(path, contents, sizeof(contents));
    assert(strstr(contents, "debug: sample value=42\n") != NULL);
    assert(unlink(path) == 0);
}

static void test_file_logging_respects_level(void)
{
    char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
    char contents[256];
    int descriptor;

    descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);

    log_init("sqm-mon-test", false);
    assert(log_set_level("notice") == 0);
    assert(log_set_file(path) == 0);
    log_message(LOG_LEVEL_DEBUG, "hidden debug message");
    log_message(LOG_LEVEL_NOTICE, "visible notice");
    log_close();

    read_log(path, contents, sizeof(contents));
    assert(strstr(contents, "notice: visible notice\n") != NULL);
    assert(strstr(contents, "hidden debug message") == NULL);
    assert(unlink(path) == 0);
}

static void test_empty_file_path_is_rejected(void)
{
    assert(log_set_file("") != 0);
}

int main(void)
{
    test_debug_logging_to_file();
    test_file_logging_respects_level();
    test_empty_file_path_is_rejected();

    (void)puts("log tests passed");
    return 0;
}
