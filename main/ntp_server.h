#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t request_count;
    uint64_t response_count;
    bool last_receive_valid;
    bool last_transmit_valid;
    bool last_transmit_synchronized;
    bool last_receive_to_transmit_valid;
    int64_t last_receive_unix_us;
    int64_t last_transmit_unix_us;
    int64_t last_receive_to_transmit_us;
} ntp_server_status_t;

esp_err_t ntp_server_start(void);
ntp_server_status_t ntp_server_status(void);
