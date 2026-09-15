#include "../src/config.c"

#include <assert.h>

static const char *remote_contents;
static int remote_spawn_error;
static bool remote_command_failed;

int __real_posix_spawn(
    pid_t *process,
    const char *path,
    const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attributes,
    char *const arguments[],
    char *const environment[]
);

/* Exercise the real pipe, CSV reader and child reaping without network access. */
int __wrap_posix_spawn(
    pid_t *process,
    const char *path,
    const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attributes,
    char *const arguments[],
    char *const environment[]
)
{
    char *fixture_arguments[] = {
        (char *)"/usr/bin/printf",
        (char *)"%s",
        (char *)remote_contents,
        NULL
    };
    char *failed_arguments[] = { (char *)"/bin/false", NULL };

    assert(strcmp(path, UCLIENT_FETCH_PATH) == 0);
    assert(strcmp(arguments[1], "-q") == 0);
    assert(strcmp(arguments[2], "-O") == 0);
    assert(strcmp(arguments[3], "-") == 0);
    assert(strcmp(arguments[4], "-T") == 0);
    assert(strcmp(arguments[5], "10") == 0);
    if (remote_spawn_error != 0) {
        return remote_spawn_error;
    }
    return __real_posix_spawn(
        process,
        remote_command_failed ? "/bin/false" : "/usr/bin/printf",
        actions,
        attributes,
        remote_command_failed ? failed_arguments : fixture_arguments,
        environment
    );
}

static struct sqm_mon_config valid_config(void)
{
    return (struct sqm_mon_config) {
        .enabled = true,
        .pinger_method = "fping",
        .no_pingers = 1U,
        .reflector_count = 1U,
        .reflectors = { "::1" },
        .reflector_ping_interval_microseconds = 300000U,
        .monitor_achieved_rates_interval_microseconds = 200000U,
        .monitor_cpu_usage_interval_microseconds = 2000000U,
        .bufferbloat_detection_window = 6U,
        .bufferbloat_detection_threshold = 3U,
        .reflector_health_check_interval_microseconds = 1000000U,
        .reflector_response_deadline_microseconds = 1000000U,
        .reflector_misbehaving_detection_window = 60U,
        .reflector_misbehaving_detection_threshold = 3U,
        .stall_detection_threshold = 5U,
        .global_ping_response_timeout_microseconds = 10000000U,
        .interface_up_check_interval_microseconds = 10000000U,
        .minimum_download_rate_bits_per_second = 5000000U,
        .minimum_upload_rate_bits_per_second = 5000000U,
        .connection_active_threshold_bits_per_second = 2000000U
    };
}

static void test_fping_only_and_intentional_exclusions(void)
{
    struct sqm_mon_config config = valid_config();
    char error[256] = "";

    config.startup_wait_microseconds = UINT64_MAX;
    config.irtt_session_duration_minutes = UINT64_MAX;
    assert(validate_latency_config(&config, error, sizeof(error)) == 0);
    (void)snprintf(config.pinger_method, sizeof(config.pinger_method), "irtt");
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "no other pinger") != NULL);
}

static void test_reflector_list_is_validated_after_url_fetch(void)
{
    struct sqm_mon_config config = valid_config();
    char error[256] = "";

    config.no_pingers = 2U;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    (void)snprintf(config.reflectors_url, sizeof(config.reflectors_url), "https://example.invalid/reflectors.csv");
    assert(validate_latency_config(&config, error, sizeof(error)) == 0);
    assert(validate_reflectors(&config, error, sizeof(error)) != 0);
    config.reflector_count = 2U;
    (void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "1.1.1.1");
    assert(validate_reflectors(&config, error, sizeof(error)) == 0);
    (void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "::1");
    assert(validate_reflectors(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "duplicate") != NULL);
    (void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "not an endpoint");
    assert(validate_reflectors(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "invalid reflector") != NULL);
}

static void test_new_timer_and_limit_validation(void)
{
    struct sqm_mon_config config = valid_config();
    char error[256] = "";

    config.stall_detection_threshold = UINT64_MAX;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config = valid_config();
    config.global_ping_response_timeout_microseconds = 0U;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config = valid_config();
    config.interface_up_check_interval_microseconds = 0U;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config = valid_config();
    config.output_cpu_stats = true;
    config.monitor_cpu_usage_interval_microseconds = 500U;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config = valid_config();
    config.log_file_max_time_minutes = UINT64_MAX;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config = valid_config();
    config.log_file_max_size_kilobytes = UINT64_MAX;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config = valid_config();
    config.enable_sleep_function = true;
    config.connection_active_threshold_bits_per_second = 5000001U;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
}

static void test_remote_csv_and_local_fallback(void)
{
    struct sqm_mon_config config = valid_config();
    char error[256] = "";

    (void)snprintf(config.reflectors_url, sizeof(config.reflectors_url), "https://example.invalid/reflectors.csv");
    config.reflectors_url_skip_lines = 2U;
    config.no_pingers = 3U;
    remote_contents = "comment\naddress,description\n1.1.1.1,one\r\n\n2606:4700:4700::1111,two\n";
    assert(config_load_remote_reflectors(&config, error, sizeof(error)) == 0);
    assert(config.reflector_count == 3U);
    assert(strcmp(config.reflectors[0], "::1") == 0);
    assert(strcmp(config.reflectors[1], "1.1.1.1") == 0);
    assert(strcmp(config.reflectors[2], "2606:4700:4700::1111") == 0);

    config.reflector_count = 1U;
    config.no_pingers = 1U;
    remote_spawn_error = ENOENT;
    assert(config_load_remote_reflectors(&config, error, sizeof(error)) == 0);
    assert(config.reflector_count == 1U);
    assert(error[0] == '\0');
    config.no_pingers = 2U;
    assert(config_load_remote_reflectors(&config, error, sizeof(error)) != 0);
    remote_spawn_error = 0;

    config.no_pingers = 1U;
    remote_command_failed = true;
    assert(config_load_remote_reflectors(&config, error, sizeof(error)) == 0);
    assert(config.reflector_count == 1U);
    remote_command_failed = false;

    config.reflectors_url_skip_lines = 0U;
    remote_contents = "::1,duplicate\n";
    assert(config_load_remote_reflectors(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "duplicate") != NULL);
    config.reflector_count = 1U;
    remote_contents = "invalid endpoint,description\n";
    assert(config_load_remote_reflectors(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "invalid reflector") != NULL);
}

int main(void)
{
    test_fping_only_and_intentional_exclusions();
    test_reflector_list_is_validated_after_url_fetch();
    test_new_timer_and_limit_validation();
    test_remote_csv_and_local_fallback();
    (void)puts("configuration validation tests passed");
    return 0;
}
