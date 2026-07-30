#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "clock_discipline.h"

typedef struct {
    clock_state_t clock_state;
    bool clock_synchronized;
    uint32_t pps_count;
    int64_t last_pps_interval_us;
    bool frequency_correction_available;
    int64_t frequency_correction_milli_ppm;
    bool phase_error_available;
    int64_t phase_error_us;

    uint64_t ntp_request_count;
    uint64_t ntp_response_count;
    bool ntp_last_receive_valid;
    bool ntp_last_transmit_valid;
    bool ntp_last_transmit_synchronized;
    bool ntp_last_receive_to_transmit_valid;
    int64_t ntp_last_receive_unix_us;
    int64_t ntp_last_transmit_unix_us;
    int64_t ntp_last_receive_to_transmit_us;

    bool ethernet_started;
    bool ethernet_link_up;
    bool has_ipv4;
    char ipv4[16];

    int64_t uptime_us;
} status_model_snapshot_t;

status_model_snapshot_t status_model_snapshot(void);
