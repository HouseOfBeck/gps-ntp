#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool started;
    bool link_up;
    bool has_ipv4;
    char ipv4[16];
} ethernet_status_t;

esp_err_t ethernet_w5500_start(void);
ethernet_status_t ethernet_w5500_status(void);
