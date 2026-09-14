#include "controller.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SATURATION_ENTER_PERCENT 90U
#define SATURATION_EXIT_PERCENT 80U
#define SATURATION_CONFIRMATION_SAMPLES 3U
#define RECOVERY_CONFIRMATION_SAMPLES 3U

#define BITS_PER_KILOBIT 1000U
#define FACTOR_PER_THOUSAND 1000U
#define FACTOR_PER_MILLION 1000000U

static uint64_t percentage_of(
    uint64_t value,
    unsigned int percentage
)
{
    uint64_t quotient = value / 100U;
    uint64_t remainder = value % 100U;

    /* Split before multiplying to avoid overflowing the full-width value. */
    return quotient * percentage +
        (remainder * percentage + 99U) / 100U;
}

static uint64_t scale_rate(
    uint64_t rate_bits_per_second,
    uint64_t factor,
    uint64_t factor_scale
)
{
    uint64_t rate_kilobits_per_second =
        rate_bits_per_second / BITS_PER_KILOBIT;
    uint64_t quotient = rate_kilobits_per_second / factor_scale;
    uint64_t remainder = rate_kilobits_per_second % factor_scale;
    uint64_t scaled_kilobits_per_second;
    uint64_t fractional;

    if ((factor != 0U && quotient > UINT64_MAX / factor) ||
        (factor != 0U && remainder > UINT64_MAX / factor)) {
        return UINT64_MAX;
    }

    fractional = remainder * factor / factor_scale;
    scaled_kilobits_per_second = quotient * factor;
    if (scaled_kilobits_per_second > UINT64_MAX - fractional) {
        return UINT64_MAX;
    }
    scaled_kilobits_per_second += fractional;
    if (scaled_kilobits_per_second > UINT64_MAX / BITS_PER_KILOBIT) {
        return UINT64_MAX;
    }

    /* cake-autorate performs every shaper calculation in whole kbit/s. */
    return scaled_kilobits_per_second * BITS_PER_KILOBIT;
}

static uint64_t clamp_rate(
    uint64_t rate,
    const struct controller_direction_config *config
)
{
    if (rate < config->minimum_rate_bits_per_second) {
        return config->minimum_rate_bits_per_second;
    }
    if (rate > config->maximum_rate_bits_per_second) {
        return config->maximum_rate_bits_per_second;
    }
    return rate;
}

static uint64_t rate_toward_base(
    uint64_t rate,
    uint64_t base_rate,
    const struct controller_config *config
)
{
    uint64_t adjusted_rate;

    if (rate > base_rate) {
        adjusted_rate = scale_rate(
            rate,
            config->rate_adjust_down_low_load_per_thousand,
            FACTOR_PER_THOUSAND
        );
        return adjusted_rate < base_rate ? base_rate : adjusted_rate;
    }
    if (rate < base_rate) {
        adjusted_rate = scale_rate(
            rate,
            config->rate_adjust_up_low_load_per_thousand,
            FACTOR_PER_THOUSAND
        );
        return adjusted_rate > base_rate ? base_rate : adjusted_rate;
    }
    return rate;
}

static int initialize_direction(
    struct controller_direction *direction,
    const struct controller_direction_config *config,
    unsigned int delay_window
)
{
    memset(direction, 0, sizeof(*direction));
    direction->config = *config;
    direction->delay_samples = calloc(
        delay_window,
        sizeof(*direction->delay_samples)
    );
    if (direction->delay_samples == NULL) {
        return -1;
    }
    direction->state = CONTROLLER_LINE_UNKNOWN;
    direction->congestion = CONTROLLER_CONGESTION_UNKNOWN;
    direction->shaper_rate_bits_per_second =
        config->base_rate_bits_per_second;
    direction->initial_rate_pending = config->adjust;
    return 0;
}

int controller_init(
    struct sqm_mon_controller *controller,
    const struct controller_config *config
)
{
    memset(controller, 0, sizeof(*controller));
    if (config->bufferbloat_detection_window == 0U ||
        config->bufferbloat_detection_threshold >
            config->bufferbloat_detection_window ||
        config->rate_minimum_adjust_down_bufferbloat_per_thousand >
            UINT64_MAX / 1000U ||
        config->rate_maximum_adjust_down_bufferbloat_per_thousand >
            UINT64_MAX / 1000U ||
        config->rate_minimum_adjust_up_high_load_per_thousand >
            UINT64_MAX / 1000U ||
        config->rate_maximum_adjust_up_high_load_per_thousand >
            UINT64_MAX / 1000U ||
        config->rate_adjust_down_low_load_per_thousand >
            UINT64_MAX / 1000U ||
        config->rate_adjust_up_low_load_per_thousand >
            UINT64_MAX / 1000U) {
        errno = EINVAL;
        return -1;
    }

    controller->config = *config;
    if (initialize_direction(
            &controller->download,
            &config->download,
            config->bufferbloat_detection_window
        ) != 0) {
        return -1;
    }
    if (initialize_direction(
            &controller->upload,
            &config->upload,
            config->bufferbloat_detection_window
        ) != 0) {
        free(controller->download.delay_samples);
        controller->download.delay_samples = NULL;
        return -1;
    }
    return 0;
}

void controller_close(struct sqm_mon_controller *controller)
{
    free(controller->download.delay_samples);
    free(controller->upload.delay_samples);
    controller->download.delay_samples = NULL;
    controller->upload.delay_samples = NULL;
}

static void reset_line_state(struct controller_direction *direction)
{
    direction->state = CONTROLLER_LINE_UNKNOWN;
    direction->saturation_samples = 0U;
    direction->recovery_samples = 0U;
}

static enum controller_line_state update_line_state(
    struct controller_direction *direction,
    const struct controller_direction_input *input
)
{
    uint64_t saturation_threshold;
    uint64_t recovery_threshold;

    if (!input->valid || input->cake_rate_bits_per_second == 0U) {
        reset_line_state(direction);
        return direction->state;
    }

    /* Separate enter/exit thresholds provide hysteresis around line load. */
    saturation_threshold = percentage_of(
        input->cake_rate_bits_per_second,
        SATURATION_ENTER_PERCENT
    );
    recovery_threshold = percentage_of(
        input->cake_rate_bits_per_second,
        SATURATION_EXIT_PERCENT
    );

    if (direction->state == CONTROLLER_LINE_SATURATED) {
        direction->saturation_samples = 0U;
        if (input->traffic_rate_bits_per_second <= recovery_threshold) {
            direction->recovery_samples++;
            if (direction->recovery_samples >= RECOVERY_CONFIRMATION_SAMPLES) {
                direction->state = CONTROLLER_LINE_BELOW_CAPACITY;
                direction->recovery_samples = 0U;
            }
        } else {
            direction->recovery_samples = 0U;
        }

        return direction->state;
    }

    direction->recovery_samples = 0U;
    if (input->traffic_rate_bits_per_second >= saturation_threshold) {
        direction->saturation_samples++;
        if (direction->saturation_samples >=
            SATURATION_CONFIRMATION_SAMPLES) {
            direction->state = CONTROLLER_LINE_SATURATED;
            direction->saturation_samples = 0U;
        }
    } else {
        direction->state = CONTROLLER_LINE_BELOW_CAPACITY;
        direction->saturation_samples = 0U;
    }

    return direction->state;
}

static enum controller_congestion_state update_congestion(
    struct controller_direction *direction,
    const struct controller_config *config,
    const struct controller_latency_input *latency,
    int64_t *average_delay_microseconds
)
{
    struct controller_delay_sample *sample;
    int64_t rtt_delta;
    int64_t owd_delta;
    unsigned int index;

    if (!latency->valid) {
        direction->congestion = CONTROLLER_CONGESTION_UNKNOWN;
        *average_delay_microseconds = 0;
        return direction->congestion;
    }

    /* A round-trip delta approximates twice the one-way queueing delay. */
    rtt_delta = (int64_t)latency->current_rtt_microseconds -
        (int64_t)latency->baseline_rtt_microseconds;
    owd_delta = rtt_delta / 2;
    index = direction->delay_next_sample;
    sample = &direction->delay_samples[index];

    /* Maintain a fixed rolling window without rescanning every sample. */
    direction->delay_sum_microseconds -= sample->delay_microseconds;
    direction->delay_sum_microseconds += owd_delta;
    sample->delay_microseconds = owd_delta;

    if (sample->delayed) {
        direction->delayed_sample_count--;
    }
    sample->delayed = owd_delta > 0 &&
        (uint64_t)owd_delta > direction->config.delay_threshold_microseconds;
    if (sample->delayed) {
        direction->delayed_sample_count++;
    }

    direction->delay_next_sample =
        (index + 1U) % config->bufferbloat_detection_window;
    *average_delay_microseconds = direction->delay_sum_microseconds /
        (int64_t)config->bufferbloat_detection_window;
    direction->congestion =
        direction->delayed_sample_count >=
            config->bufferbloat_detection_threshold
            ? CONTROLLER_CONGESTION_DETECTED
            : CONTROLLER_CONGESTION_CLEAR;
    return direction->congestion;
}

static uint64_t interpolate_factor(
    uint64_t minimum_factor_per_thousand,
    uint64_t maximum_factor_per_thousand,
    uint64_t adjustment
)
{
    uint64_t difference;
    uint64_t base = minimum_factor_per_thousand * 1000U;

    /*
     * cake-autorate keeps the configured endpoints per-thousand, then keeps
     * three additional decimal places while interpolating between them.
     */
    if (minimum_factor_per_thousand >= maximum_factor_per_thousand) {
        difference = minimum_factor_per_thousand -
            maximum_factor_per_thousand;
        return base - adjustment * difference;
    }

    difference = maximum_factor_per_thousand -
        minimum_factor_per_thousand;
    return base + adjustment * difference;
}

static uint64_t downward_factor(
    const struct controller_direction_config *config,
    const struct controller_config *controller_config,
    int64_t average_delay_microseconds
)
{
    uint64_t adjustment;
    uint64_t delay_above_threshold;
    uint64_t adjustment_range;

    if (config->average_delay_maximum_adjust_down_microseconds <=
        config->delay_threshold_microseconds) {
        adjustment = 1000U;
    } else if (average_delay_microseconds > 0 &&
        (uint64_t)average_delay_microseconds >
            config->delay_threshold_microseconds) {
        adjustment_range =
            config->average_delay_maximum_adjust_down_microseconds -
            config->delay_threshold_microseconds;
        if ((uint64_t)average_delay_microseconds >=
            config->average_delay_maximum_adjust_down_microseconds) {
            adjustment = 1000U;
        } else {
            delay_above_threshold = (uint64_t)average_delay_microseconds -
                config->delay_threshold_microseconds;
            adjustment = 1000U * delay_above_threshold / adjustment_range;
        }
    } else {
        adjustment = 0U;
    }

    return interpolate_factor(
        controller_config->rate_minimum_adjust_down_bufferbloat_per_thousand,
        controller_config->rate_maximum_adjust_down_bufferbloat_per_thousand,
        adjustment
    );
}

static uint64_t upward_factor(
    const struct controller_direction_config *config,
    const struct controller_config *controller_config,
    int64_t average_delay_microseconds
)
{
    uint64_t adjustment;
    uint64_t delay_below_threshold;
    uint64_t adjustment_range;

    if (config->delay_threshold_microseconds <=
        config->average_delay_maximum_adjust_up_microseconds) {
        adjustment = 1000U;
    } else if (average_delay_microseconds <= 0 ||
        (uint64_t)average_delay_microseconds <=
            config->average_delay_maximum_adjust_up_microseconds) {
        adjustment = 1000U;
    } else if ((uint64_t)average_delay_microseconds <
        config->delay_threshold_microseconds) {
        delay_below_threshold = config->delay_threshold_microseconds -
            (uint64_t)average_delay_microseconds;
        adjustment_range = config->delay_threshold_microseconds -
            config->average_delay_maximum_adjust_up_microseconds;
        adjustment = 1000U * delay_below_threshold / adjustment_range;
    } else {
        adjustment = 0U;
    }

    return interpolate_factor(
        controller_config->rate_minimum_adjust_up_high_load_per_thousand,
        controller_config->rate_maximum_adjust_up_high_load_per_thousand,
        adjustment
    );
}

static bool refractory_period_elapsed(
    uint64_t timestamp_microseconds,
    uint64_t previous_timestamp_microseconds,
    uint64_t refractory_period_microseconds
)
{
    /* A backwards monotonic timestamp is treated as not yet elapsed. */
    return timestamp_microseconds > previous_timestamp_microseconds &&
        timestamp_microseconds - previous_timestamp_microseconds >
            refractory_period_microseconds;
}

unsigned int controller_load_percent(
    uint64_t traffic_rate_bits_per_second,
    uint64_t shaper_rate_bits_per_second
)
{
    uint64_t traffic_rate_kilobits_per_second =
        traffic_rate_bits_per_second / BITS_PER_KILOBIT;
    uint64_t shaper_rate_kilobits_per_second =
        shaper_rate_bits_per_second / BITS_PER_KILOBIT;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t percentage;

    if (shaper_rate_kilobits_per_second == 0U) {
        return 0U;
    }

    quotient = traffic_rate_kilobits_per_second /
        shaper_rate_kilobits_per_second;
    if (quotient > UINT_MAX / 100U) {
        return UINT_MAX;
    }
    remainder = traffic_rate_kilobits_per_second %
        shaper_rate_kilobits_per_second;
    percentage = quotient * 100U;
    if (remainder > UINT64_MAX / 100U) {
        percentage += (uint64_t)(
            (long double)remainder * 100.0L /
            (long double)shaper_rate_kilobits_per_second
        );
    } else {
        percentage += remainder * 100U /
            shaper_rate_kilobits_per_second;
    }
    return percentage > UINT_MAX ? UINT_MAX : (unsigned int)percentage;
}

static enum controller_rate_reason adjust_rate(
    struct controller_direction *direction,
    const struct controller_config *config,
    const struct controller_direction_input *input,
    bool latency_valid,
    int64_t average_delay_microseconds,
    uint64_t timestamp_microseconds
)
{
    uint64_t previous_rate = direction->shaper_rate_bits_per_second;
    bool high_load;

    if (!direction->config.adjust) {
        return CONTROLLER_RATE_UNCHANGED;
    }
    if (direction->initial_rate_pending) {
        if (!input->valid) {
            return CONTROLLER_RATE_UNCHANGED;
        }
        direction->initial_rate_pending = false;
        direction->last_congestion_adjustment_microseconds =
            timestamp_microseconds;
        direction->last_decay_adjustment_microseconds =
            timestamp_microseconds;
        return CONTROLLER_RATE_INITIAL;
    }
    if (!input->valid || !latency_valid) {
        return CONTROLLER_RATE_UNCHANGED;
    }

    if (direction->congestion == CONTROLLER_CONGESTION_DETECTED &&
        refractory_period_elapsed(
            timestamp_microseconds,
            direction->last_congestion_adjustment_microseconds,
            config->bufferbloat_refractory_period_microseconds
        )) {
        direction->shaper_rate_bits_per_second = scale_rate(
            previous_rate,
            downward_factor(
                &direction->config,
                config,
                average_delay_microseconds
            ),
            FACTOR_PER_MILLION
        );
        direction->last_congestion_adjustment_microseconds =
            timestamp_microseconds;
        /* Do not let low-load decay immediately undo a congestion cut. */
        direction->last_decay_adjustment_microseconds =
            timestamp_microseconds;
    } else {
        high_load = controller_load_percent(
            input->traffic_rate_bits_per_second,
            previous_rate
        ) > config->high_load_threshold_percent;
        if (direction->congestion != CONTROLLER_CONGESTION_DETECTED &&
            high_load &&
            refractory_period_elapsed(
                timestamp_microseconds,
                direction->last_congestion_adjustment_microseconds,
                config->bufferbloat_refractory_period_microseconds
            )) {
            direction->shaper_rate_bits_per_second = scale_rate(
                previous_rate,
                upward_factor(
                    &direction->config,
                    config,
                    average_delay_microseconds
                ),
                FACTOR_PER_MILLION
            );
            /* Give the increased rate a full decay interval to be observed. */
            direction->last_decay_adjustment_microseconds =
                timestamp_microseconds;
        } else if (direction->congestion != CONTROLLER_CONGESTION_DETECTED &&
            !high_load &&
            previous_rate != direction->config.base_rate_bits_per_second &&
            refractory_period_elapsed(
                timestamp_microseconds,
                direction->last_decay_adjustment_microseconds,
                config->decay_refractory_period_microseconds
            )) {
            /* With low load, converge by 1% steps instead of jumping to base. */
            direction->shaper_rate_bits_per_second = rate_toward_base(
                previous_rate,
                direction->config.base_rate_bits_per_second,
                config
            );
            direction->last_decay_adjustment_microseconds =
                timestamp_microseconds;
        }
    }

    direction->shaper_rate_bits_per_second = clamp_rate(
        direction->shaper_rate_bits_per_second,
        &direction->config
    );
    if (direction->shaper_rate_bits_per_second == previous_rate) {
        return CONTROLLER_RATE_UNCHANGED;
    }
    if (direction->congestion == CONTROLLER_CONGESTION_DETECTED) {
        return CONTROLLER_RATE_CONGESTION;
    }
    if (controller_load_percent(
            input->traffic_rate_bits_per_second,
            previous_rate
        ) > config->high_load_threshold_percent) {
        return CONTROLLER_RATE_HIGH_LOAD;
    }
    return CONTROLLER_RATE_RETURN_TO_BASE;
}

static void set_rate_output(
    const struct controller_direction *direction,
    const struct controller_direction_input *input,
    enum controller_rate_reason reason,
    struct controller_direction_output *output
)
{
    if (!direction->config.adjust) {
        output->rate_bits_per_second = input->cake_rate_bits_per_second;
        output->rate_changed = false;
        output->rate_reason = CONTROLLER_RATE_UNCHANGED;
        return;
    }

    output->rate_bits_per_second = direction->shaper_rate_bits_per_second;
    output->rate_changed = input->valid &&
        input->cake_rate_bits_per_second != output->rate_bits_per_second;
    output->rate_reason = output->rate_changed &&
        reason == CONTROLLER_RATE_UNCHANGED
        ? CONTROLLER_RATE_RECONCILE
        : reason;
}

static void update_direction(
    struct controller_direction *direction,
    const struct controller_config *config,
    const struct controller_direction_input *input,
    const struct controller_latency_input *latency,
    uint64_t timestamp_microseconds,
    struct controller_direction_output *output
)
{
    enum controller_line_state previous_state = direction->state;
    enum controller_congestion_state previous_congestion =
        direction->congestion;
    enum controller_rate_reason reason;

    output->state = update_line_state(direction, input);
    output->congestion = update_congestion(
        direction,
        config,
        latency,
        &output->average_delay_microseconds
    );
    reason = adjust_rate(
        direction,
        config,
        input,
        latency->valid,
        output->average_delay_microseconds,
        timestamp_microseconds
    );
    set_rate_output(
        direction,
        input,
        reason,
        output
    );

    output->delay_sum_microseconds = direction->delay_sum_microseconds;
    output->delayed_sample_count = direction->delayed_sample_count;
    output->state_changed = output->state != previous_state;
    output->congestion_changed =
        output->congestion != previous_congestion;
}

void controller_update(
    struct sqm_mon_controller *controller,
    const struct controller_input *input,
    struct controller_output *output
)
{
    update_direction(
        &controller->download,
        &controller->config,
        &input->download,
        &input->latency,
        input->timestamp_microseconds,
        &output->download
    );
    update_direction(
        &controller->upload,
        &controller->config,
        &input->upload,
        &input->latency,
        input->timestamp_microseconds,
        &output->upload
    );
}
