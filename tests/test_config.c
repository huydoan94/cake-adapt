#include "../src/config.c"

#include <assert.h>

/* Use real libuci inline accessors with a controlled single-option lookup. */
static struct uci_option *lookup_option;

int uci_lookup_next(
    struct uci_context *context,
    struct uci_element **element,
    struct uci_list *list,
    const char *name
)
{
    (void)context;
    (void)list;
    if (
        lookup_option == NULL ||
        strcmp(lookup_option->e.name, name) != 0
    ) {
        return UCI_ERR_NOTFOUND;
    }
    *element = &lookup_option->e;
    return UCI_OK;
}

static void test_scalar_option_types(void)
{
    struct uci_section section = { 0 };
    struct uci_option option = {
        .e = { .type = UCI_TYPE_OPTION, .name = "enabled" },
        .type = UCI_TYPE_LIST
    };
    struct config config = {
        .no_pingers = 7U,
        .interface = "default"
    };
    char error[128];

    lookup_option = &option;
    assert(load_boolean_options(NULL, &section, &config, error, sizeof(error)) == -1);
    assert(strstr(error, "not a list") != NULL);
    assert(!config.enabled);
    option.e.name = "no_pingers";
    assert(load_scaled_options(NULL, &section, &config, error, sizeof(error)) == -1);
    assert(config.no_pingers == 7U);
    option.e.name = "interface";
    assert(load_string_options(NULL, &section, &config, error, sizeof(error)) == -1);
    assert(strcmp(config.interface, "default") == 0);

    lookup_option = NULL;
    assert(load_boolean_options(NULL, &section, &config, error, sizeof(error)) == 0);
    assert(load_scaled_options(NULL, &section, &config, error, sizeof(error)) == 0);
    assert(load_string_options(NULL, &section, &config, error, sizeof(error)) == 0);
    assert(!config.enabled);
    assert(config.no_pingers == 7U);
    assert(strcmp(config.interface, "default") == 0);

    lookup_option = &option;
    option.type = UCI_TYPE_STRING;
    option.v.string = "1";
    option.e.name = "enabled";
    assert(load_boolean_options(NULL, &section, &config, error, sizeof(error)) == 0);
    assert(config.enabled);
    option.e.name = "no_pingers";
    assert(load_scaled_options(NULL, &section, &config, error, sizeof(error)) == 0);
    assert(config.no_pingers == 1U);
    option.e.name = "interface";
    assert(load_string_options(NULL, &section, &config, error, sizeof(error)) == 0);
    assert(strcmp(config.interface, "1") == 0);
    lookup_option = NULL;
}

static void test_supported_option_names(void)
{
    bool found_adjust_download = false;
    bool found_download_interface = false;
    bool found_ping_arguments = false;
    bool found_upload_interface = false;
    size_t index;

    for (index = 0U; index < config_option_count(); index++) {
        const char *name = config_option_name(index);

        assert(name != NULL);
        found_adjust_download = found_adjust_download ||
            strcmp(name, "adjust_dl_shaper_rate") == 0;
        found_download_interface = found_download_interface ||
            strcmp(name, "dl_if") == 0;
        found_ping_arguments = found_ping_arguments ||
            strcmp(name, "ping_extra_args") == 0;
        found_upload_interface = found_upload_interface ||
            strcmp(name, "ul_if") == 0;
    }
    assert(found_adjust_download);
    assert(found_download_interface);
    assert(found_ping_arguments);
    assert(found_upload_interface);
    assert(config_option_name(config_option_count()) == NULL);
}

static void test_interface_resolution(void)
{
    struct config config = { .interface = "eth0" };
    char error[128] = "";

    assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.interface, "eth0") == 0);
    assert(strcmp(config.ingress_interface, "ifb4eth0") == 0);
    assert(!config.interface_overridden);

    config = (struct config) {
        .interface = "eth0",
        .ul_if = "wan",
        .dl_if = "download"
    };
    assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.interface, "wan") == 0);
    assert(strcmp(config.ingress_interface, "download") == 0);
    assert(config.interface_overridden);

    config = (struct config) { .ul_if = "wan", .dl_if = "download" };
    assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.interface, "wan") == 0);
    assert(strcmp(config.ingress_interface, "download") == 0);
    assert(!config.interface_overridden);

    config = (struct config) { .interface = "eth0", .ul_if = "wan" };
    assert(resolve_interfaces(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "configured together") != NULL);

    config = (struct config) { .enabled = true };
    assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
    assert(validate_config(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "either option 'interface' or both options") != NULL);
}

static struct config valid_config(void)
{
    return (struct config) {
        .enabled = true,
        .interface = "eth0",
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
        .base_download_rate_bits_per_second = 20000000U,
        .maximum_download_rate_bits_per_second = 80000000U,
        .minimum_upload_rate_bits_per_second = 5000000U,
        .base_upload_rate_bits_per_second = 20000000U,
        .maximum_upload_rate_bits_per_second = 35000000U,
        .connection_active_threshold_bits_per_second = 2000000U
    };
}

static void test_fping_only_and_intentional_exclusions(void)
{
    struct config config = valid_config();
    char error[256] = "";

    config.irtt_session_duration_minutes = UINT64_MAX;
    assert(validate_latency_config(&config, error, sizeof(error)) == 0);
    (void)snprintf(config.pinger_method, sizeof(config.pinger_method), "irtt");
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    assert(strstr(error, "no other pinger") != NULL);
}

static void test_reflector_list_validation(void)
{
    struct config config = valid_config();
    char error[256] = "";

    config.no_pingers = 2U;
    assert(validate_latency_config(&config, error, sizeof(error)) != 0);
    config.reflector_count = 2U;
    (void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "1.1.1.1");
    assert(validate_latency_config(&config, error, sizeof(error)) == 0);
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
    struct config config = valid_config();
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
int main(void)
{
    uint64_t value = 0U;
    char error[256] = "";

    assert(parse_scaled_decimal("18446744073709551615", 1U, &value, "test", error, sizeof(error)) == 0);
    assert(value == UINT64_MAX);
    assert(parse_scaled_decimal("18446744073709551616", 1U, &value, "test", error, sizeof(error)) != 0);
    assert(strstr(error, "too large") != NULL);
    assert(parse_scaled_decimal("1.025", 1000U, &value, "test", error, sizeof(error)) == 0);
    assert(value == 1025U);
    assert(parse_scaled_decimal("1.0251", 1000U, &value, "test", error, sizeof(error)) != 0);
    assert(strstr(error, "more precision") != NULL);
    assert(parse_scaled_decimal("+1", 1U, &value, "test", error, sizeof(error)) != 0);
    assert(strstr(error, "non-negative decimal") != NULL);

    assert(validate_rate_range(true, 5000000U, 20000000U, 80000000U,
        "download", error, sizeof(error)) == 0);
    assert(validate_rate_range(true, 5000001U, 20000000U, 80000000U,
        "download", error, sizeof(error)) != 0);
    assert(strstr(error, "whole kbit/s") != NULL);

    test_fping_only_and_intentional_exclusions();
    test_scalar_option_types();
    test_supported_option_names();
    test_interface_resolution();
    test_reflector_list_validation();
    test_new_timer_and_limit_validation();
    (void)puts("configuration validation tests passed");
    return 0;
}
