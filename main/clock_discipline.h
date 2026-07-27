#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    CLOCK_STATE_UNSYNC = 0,
    CLOCK_STATE_ACQUIRING,
    CLOCK_STATE_SYNCED,
    CLOCK_STATE_HOLDOVER,
    CLOCK_STATE_PPS_LOST,
} clock_state_t;

typedef enum {
    CLOCK_ANCHOR_ACCEPTED = 0,
    CLOCK_ANCHOR_NO_PPS,
    CLOCK_ANCHOR_DUPLICATE_PPS,
    CLOCK_ANCHOR_STALE_PPS,
} clock_anchor_result_t;

typedef struct {
    clock_state_t state;
    bool time_valid;
    bool synchronized;
    int64_t unix_us;
    int64_t reference_unix_sec;
    int64_t reference_esp_us;
    int64_t last_pps_age_us;
    int64_t last_pps_interval_us;
    int64_t esp_us_per_second_x1000;
    uint32_t pps_count;
} clock_snapshot_t;

esp_err_t clock_discipline_init(gpio_num_t pps_gpio);
clock_anchor_result_t clock_discipline_accept_utc(int64_t unix_sec);
void clock_discipline_report_invalid_rmc(void);
clock_snapshot_t clock_discipline_snapshot(void);
clock_state_t clock_discipline_state(void);
const char *clock_state_name(clock_state_t state);

