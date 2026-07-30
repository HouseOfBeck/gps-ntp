#include "clock_discipline.h"

#include <limits.h>

#include "clock_math.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define RATE_WINDOW 32
#define PPS_PAIR_MAX_AGE_US 900000
#define PPS_STALE_US 2500000
#define RMC_STALE_US 2500000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static gpio_num_t s_pps_gpio;
static int64_t s_pps_timestamp_us;
static int64_t s_pps_delta_us;
static uint32_t s_pps_count;
static uint32_t s_last_used_pps_count;

static bool s_saw_valid_rmc;
static bool s_rmc_valid;
static bool s_has_anchor;
static int64_t s_last_accepted_rmc_us;
static int64_t s_anchor_esp_us;
static int64_t s_anchor_utc_sec;

/* Local esp_timer microseconds measured over s_rate_intervals GPS seconds. */
static int64_t s_rate_elapsed_us = 1000000;
static int s_rate_intervals = 1;
static int64_t s_rate_samples[RATE_WINDOW + 1];
static int s_rate_sample_count;
static int s_rate_sample_pos;

static void IRAM_ATTR pps_isr_handler(void *arg)
{
    (void)arg;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&s_lock);
    if (s_pps_timestamp_us != 0) {
        s_pps_delta_us = now_us - s_pps_timestamp_us;
    }
    s_pps_timestamp_us = now_us;
    s_pps_count++;
    portEXIT_CRITICAL_ISR(&s_lock);
}

static void update_rate_locked(int64_t timestamp_us)
{
    s_rate_samples[s_rate_sample_pos] = timestamp_us;
    s_rate_sample_pos = (s_rate_sample_pos + 1) % (RATE_WINDOW + 1);

    if (s_rate_sample_count < RATE_WINDOW + 1) {
        s_rate_sample_count++;
    }
    if (s_rate_sample_count < 2) {
        return;
    }

    int newest = (s_rate_sample_pos - 1 + RATE_WINDOW + 1) %
                 (RATE_WINDOW + 1);
    int intervals = s_rate_sample_count - 1;
    int oldest = (newest - intervals + RATE_WINDOW + 1) %
                 (RATE_WINDOW + 1);
    int64_t elapsed_us = s_rate_samples[newest] - s_rate_samples[oldest];

    if (elapsed_us > 0) {
        s_rate_elapsed_us = elapsed_us;
        s_rate_intervals = intervals;
    }
}

static clock_state_t state_locked(int64_t now_us)
{
    if (!s_has_anchor) {
        return (s_pps_count != 0 || s_saw_valid_rmc)
                   ? CLOCK_STATE_ACQUIRING
                   : CLOCK_STATE_UNSYNC;
    }
    if (!s_rmc_valid) {
        return CLOCK_STATE_UNSYNC;
    }
    if (s_pps_timestamp_us == 0 ||
        now_us - s_pps_timestamp_us > PPS_STALE_US) {
        return CLOCK_STATE_PPS_LOST;
    }
    if (now_us - s_last_accepted_rmc_us > RMC_STALE_US) {
        return CLOCK_STATE_HOLDOVER;
    }
    return CLOCK_STATE_SYNCED;
}

esp_err_t clock_discipline_init(gpio_num_t pps_gpio)
{
    s_pps_gpio = pps_gpio;

    const gpio_config_t pps_config = {
        .pin_bit_mask = 1ULL << pps_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };

    esp_err_t err = gpio_config(&pps_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(s_pps_gpio, pps_isr_handler, NULL);
}

clock_anchor_result_t clock_discipline_accept_utc(int64_t unix_sec)
{
    int64_t now_us = esp_timer_get_time();
    clock_anchor_result_t result;

    portENTER_CRITICAL(&s_lock);
    s_saw_valid_rmc = true;

    if (s_pps_count == 0 || s_pps_timestamp_us <= 0) {
        result = CLOCK_ANCHOR_NO_PPS;
    } else if (s_pps_count == s_last_used_pps_count) {
        result = CLOCK_ANCHOR_DUPLICATE_PPS;
    } else if (now_us - s_pps_timestamp_us < 0 ||
               now_us - s_pps_timestamp_us > PPS_PAIR_MAX_AGE_US) {
        result = CLOCK_ANCHOR_STALE_PPS;
    } else {
        s_last_used_pps_count = s_pps_count;
        update_rate_locked(s_pps_timestamp_us);
        s_anchor_esp_us = s_pps_timestamp_us;
        s_anchor_utc_sec = unix_sec;
        s_last_accepted_rmc_us = now_us;
        s_rmc_valid = true;
        s_has_anchor = true;
        result = CLOCK_ANCHOR_ACCEPTED;
    }

    portEXIT_CRITICAL(&s_lock);
    return result;
}

void clock_discipline_report_invalid_rmc(void)
{
    portENTER_CRITICAL(&s_lock);
    s_rmc_valid = false;
    portEXIT_CRITICAL(&s_lock);
}

clock_snapshot_t clock_discipline_snapshot(void)
{
    int64_t now_us = esp_timer_get_time();
    clock_snapshot_t snapshot = {0};
    int64_t anchor_esp_us;
    int64_t anchor_utc_sec;
    int64_t rate_elapsed_us;
    int rate_intervals;

    portENTER_CRITICAL(&s_lock);
    snapshot.state = state_locked(now_us);
    snapshot.time_valid = s_has_anchor;
    snapshot.synchronized = snapshot.state == CLOCK_STATE_SYNCED;
    snapshot.reference_unix_sec = s_anchor_utc_sec;
    snapshot.reference_esp_us = s_anchor_esp_us;
    snapshot.last_pps_interval_us = s_pps_delta_us;
    snapshot.rate_valid = s_rate_sample_count >= 2;
    snapshot.pps_count = s_pps_count;
    snapshot.last_pps_age_us = s_pps_timestamp_us == 0
                                   ? INT64_MAX
                                   : now_us - s_pps_timestamp_us;
    anchor_esp_us = s_anchor_esp_us;
    anchor_utc_sec = s_anchor_utc_sec;
    rate_elapsed_us = s_rate_elapsed_us;
    rate_intervals = s_rate_intervals;
    portEXIT_CRITICAL(&s_lock);

    if (snapshot.time_valid && rate_elapsed_us > 0) {
        int64_t elapsed_esp_us = now_us - anchor_esp_us;
        int64_t elapsed_utc_us =
            clock_math_correct_elapsed_us(elapsed_esp_us,
                                          rate_elapsed_us,
                                          (uint32_t)rate_intervals);
        snapshot.unix_us = anchor_utc_sec * 1000000LL + elapsed_utc_us;
    }

    snapshot.esp_us_per_second_x1000 =
        (rate_elapsed_us * 1000LL) / rate_intervals;
    return snapshot;
}

clock_state_t clock_discipline_state(void)
{
    return clock_discipline_snapshot().state;
}

const char *clock_state_name(clock_state_t state)
{
    switch (state) {
    case CLOCK_STATE_UNSYNC:
        return "UNSYNC";
    case CLOCK_STATE_ACQUIRING:
        return "ACQUIRING";
    case CLOCK_STATE_SYNCED:
        return "SYNCED";
    case CLOCK_STATE_HOLDOVER:
        return "HOLDOVER";
    case CLOCK_STATE_PPS_LOST:
        return "PPS_LOST";
    default:
        return "UNKNOWN";
    }
}
