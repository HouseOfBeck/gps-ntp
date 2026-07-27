#pragma once

#include "esp_err.h"

/*
 * Starts the non-critical display task. A successful return only means the
 * task was created; display detection and initialization happen in that task.
 */
esp_err_t oled_display_start(void);

