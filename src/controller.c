#include "controller.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define SATURATION_ENTER_PERCENT 90U
#define SATURATION_EXIT_PERCENT 80U
#define SATURATION_CONFIRMATION_SAMPLES 3U
#define RECOVERY_CONFIRMATION_SAMPLES 3U

/* cake-autorate 3.3 defaults, represented as integer per-mille factors. */
#define RATE_MINIMUM_DOWN_PER_MILLE 990U
#define RATE_MAXIMUM_DOWN_PER_MILLE 750U
#define RATE_MINIMUM_UP_PER_MILLE 1000U
#define RATE_MAXIMUM_UP_PER_MILLE 1040U
#define RATE_LOW_LOAD_DOWN_PER_MILLE 990U
#define RATE_LOW_LOAD_UP_PER_MILLE 1010U
#define BUFFERBLOAT_DETECTION_SAMPLES 3U
#define BUFFERBLOAT_REFRACTORY_MICROSECONDS 300000U
#define DECAY_REFRACTORY_MICROSECONDS 1000000U

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
    uint64_t rate,
    unsigned int factor_per_mille
)
{
    uint64_t quotient = rate / 1000U;
    uint64_t remainder = rate % 1000U;
    uint64_t scaled;

    if (quotient > UINT64_MAX / factor_per_mille) {
        return UINT64_MAX;
    }

    scaled = quotient * factor_per_mille +
        (remainder * factor_per_mille) / 1000U;
    /* CAKE accepts whole bytes/s, so keep the bit rate divisible by eight. */
    return scaled - scaled % 8U;
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
    uint64_t base_rate
)
{
    uint64_t adjusted_rate;

    if (rate > base_rate) {
        adjusted_rate = scale_rate(rate, RATE_LOW_LOAD_DOWN_PER_MILLE);
        return adjusted_rate < base_rate ? base_rate : adjusted_rate;
    }
    if (rate < base_rate) {
        adjusted_rate = scale_rate(rate, RATE_LOW_LOAD_UP_PER_MILLE);
        return adjusted_rate > base_rate ? base_rate : adjusted_rate;
    }
    return rate;
}

static void initialize_direction(
    struct controller_direction *direction,
    const struct controller_direction_config *config
)
{
    memset(direction, 0, sizeof(*direction));
    direction->config = *config;
    direction->state = CONTROLLER_LINE_UNKNOWN;
    direction->congestion = CONTROLLER_CONGESTION_UNKNOWN;
    direction->shaper_rate_bits_per_second =
        config->base_rate_bits_per_second;
    direction->initial_rate_pending = config->adjust;
}

void controller_init(
    struct sqm_mon_controller *controller,
    const struct controller_config *config
)
{
    initialize_direction(&controller->download, &config->download);
    initialize_direction(&controller->upload, &config->upload);
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
    const struct controller_latency_input *latency,
    int64_t *average_delay_microseconds
)
{
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

    /* Maintain a fixed rolling window without rescanning every sample. */
    direction->delay_sum_microseconds -= direction->delay_samples[index];
    direction->delay_sum_microseconds += owd_delta;
    direction->delay_samples[index] = owd_delta;

    if (direction->delayed_samples[index]) {
        direction->delayed_sample_count--;
    }
    direction->delayed_samples[index] =
        owd_delta > CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS;
    if (direction->delayed_samples[index]) {
        direction->delayed_sample_count++;
    }

    direction->delay_next_sample =
        (index + 1U) % CONTROLLER_DELAY_WINDOW_SAMPLES;
    *average_delay_microseconds = direction->delay_sum_microseconds /
        CONTROLLER_DELAY_WINDOW_SAMPLES;
    direction->congestion =
        direction->delayed_sample_count >= BUFFERBLOAT_DETECTION_SAMPLES
            ? CONTROLLER_CONGESTION_DETECTED
            : CONTROLLER_CONGESTION_CLEAR;
    return direction->congestion;
}

static unsigned int downward_factor(int64_t average_delay_microseconds)
{
    uint64_t scaled;

    if (average_delay_microseconds <=
        CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS) {
        return RATE_MINIMUM_DOWN_PER_MILLE;
    }
    if (average_delay_microseconds >=
        CONTROLLER_OWD_MAXIMUM_ADJUST_DOWN_MICROSECONDS) {
        return RATE_MAXIMUM_DOWN_PER_MILLE;
    }

    /* Interpolate from a gentle 0.99 cut to cake-autorate's 0.75 maximum. */
    scaled = 1000U *
        (uint64_t)(average_delay_microseconds -
            CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS) /
        (CONTROLLER_OWD_MAXIMUM_ADJUST_DOWN_MICROSECONDS -
            CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS);
    return RATE_MINIMUM_DOWN_PER_MILLE -
        (unsigned int)(scaled *
            (RATE_MINIMUM_DOWN_PER_MILLE -
                RATE_MAXIMUM_DOWN_PER_MILLE) /
            1000U);
}

static unsigned int upward_factor(int64_t average_delay_microseconds)
{
    uint64_t scaled;

    if (average_delay_microseconds <=
        CONTROLLER_OWD_MAXIMUM_ADJUST_UP_MICROSECONDS) {
        return RATE_MAXIMUM_UP_PER_MILLE;
    }
    if (average_delay_microseconds >=
        CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS) {
        return RATE_MINIMUM_UP_PER_MILLE;
    }

    /* Interpolate from no increase at the delay threshold to 1.04. */
    scaled = 1000U *
        (uint64_t)(CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS -
            average_delay_microseconds) /
        (CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS -
            CONTROLLER_OWD_MAXIMUM_ADJUST_UP_MICROSECONDS);
    return RATE_MINIMUM_UP_PER_MILLE +
        (unsigned int)(scaled *
            (RATE_MAXIMUM_UP_PER_MILLE -
                RATE_MINIMUM_UP_PER_MILLE) /
            1000U);
}

static bool refractory_period_elapsed(
    uint64_t timestamp_microseconds,
    uint64_t previous_timestamp_microseconds,
    uint64_t refractory_period_microseconds
)
{
    /* A backwards monotonic timestamp is treated as not yet elapsed. */
    return timestamp_microseconds >= previous_timestamp_microseconds &&
        timestamp_microseconds - previous_timestamp_microseconds >=
            refractory_period_microseconds;
}

static enum controller_rate_reason adjust_rate(
    struct controller_direction *direction,
    const struct controller_direction_input *input,
    bool latency_valid,
    int64_t average_delay_microseconds,
    uint64_t timestamp_microseconds
)
{
    uint64_t previous_rate = direction->shaper_rate_bits_per_second;
    uint64_t high_load_threshold;

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
            BUFFERBLOAT_REFRACTORY_MICROSECONDS
        )) {
        direction->shaper_rate_bits_per_second = scale_rate(
            previous_rate,
            downward_factor(average_delay_microseconds)
        );
        direction->last_congestion_adjustment_microseconds =
            timestamp_microseconds;
        /* Do not let low-load decay immediately undo a congestion cut. */
        direction->last_decay_adjustment_microseconds =
            timestamp_microseconds;
    } else {
        high_load_threshold = percentage_of(
            previous_rate,
            CONTROLLER_HIGH_LOAD_PERCENT
        );
        if (direction->congestion != CONTROLLER_CONGESTION_DETECTED &&
            input->traffic_rate_bits_per_second > high_load_threshold &&
            refractory_period_elapsed(
                timestamp_microseconds,
                direction->last_congestion_adjustment_microseconds,
                BUFFERBLOAT_REFRACTORY_MICROSECONDS
            )) {
            direction->shaper_rate_bits_per_second = scale_rate(
                previous_rate,
                upward_factor(average_delay_microseconds)
            );
            /* Give the increased rate a full decay interval to be observed. */
            direction->last_decay_adjustment_microseconds =
                timestamp_microseconds;
        } else if (direction->congestion != CONTROLLER_CONGESTION_DETECTED &&
            input->traffic_rate_bits_per_second <= high_load_threshold &&
            previous_rate != direction->config.base_rate_bits_per_second &&
            refractory_period_elapsed(
                timestamp_microseconds,
                direction->last_decay_adjustment_microseconds,
                DECAY_REFRACTORY_MICROSECONDS
            )) {
            /* With low load, converge by 1% steps instead of jumping to base. */
            direction->shaper_rate_bits_per_second = rate_toward_base(
                previous_rate,
                direction->config.base_rate_bits_per_second
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
    if (input->traffic_rate_bits_per_second >
        percentage_of(previous_rate, CONTROLLER_HIGH_LOAD_PERCENT)) {
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
        latency,
        &output->average_delay_microseconds
    );
    reason = adjust_rate(
        direction,
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
        &input->download,
        &input->latency,
        input->timestamp_microseconds,
        &output->download
    );
    update_direction(
        &controller->upload,
        &input->upload,
        &input->latency,
        input->timestamp_microseconds,
        &output->upload
    );
}
