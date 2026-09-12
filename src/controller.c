#include "controller.h"

#include <stddef.h>

#define SATURATION_ENTER_PERCENT 90U
#define SATURATION_EXIT_PERCENT 80U
#define SATURATION_CONFIRMATION_SAMPLES 3U
#define RECOVERY_CONFIRMATION_SAMPLES 3U

static uint64_t percentage_of(
    uint64_t value,
    unsigned int percentage
)
{
    uint64_t quotient = value / 100U;
    uint64_t remainder = value % 100U;

    return quotient * percentage +
        (remainder * percentage + 99U) / 100U;
}

static void reset_direction(struct controller_direction *direction)
{
    direction->state = CONTROLLER_LINE_UNKNOWN;
    direction->saturation_samples = 0U;
    direction->recovery_samples = 0U;
}

void controller_init(struct sqm_mon_controller *controller)
{
    reset_direction(&controller->download);
    reset_direction(&controller->upload);
}

static enum controller_line_state update_direction(
    struct controller_direction *direction,
    const struct controller_direction_input *input
)
{
    uint64_t saturation_threshold;
    uint64_t recovery_threshold;

    if (!input->valid || input->cake_rate_bits_per_second == 0U) {
        reset_direction(direction);
        return direction->state;
    }

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

void controller_update(
    struct sqm_mon_controller *controller,
    const struct controller_input *input,
    struct controller_output *output
)
{
    enum controller_line_state previous_download = controller->download.state;
    enum controller_line_state previous_upload = controller->upload.state;

    output->download_state = update_direction(
        &controller->download,
        &input->download
    );
    output->upload_state = update_direction(
        &controller->upload,
        &input->upload
    );
    output->download_state_changed =
        output->download_state != previous_download;
    output->upload_state_changed = output->upload_state != previous_upload;
}
