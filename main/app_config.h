#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_HOSTNAME_MAX_LENGTH 63
#define APP_CONFIG_HOSTNAME_BUFFER_SIZE (APP_CONFIG_HOSTNAME_MAX_LENGTH + 1)

/* Initializes NVS configuration. Accessors retain safe defaults on failure. */
esp_err_t app_config_init(void);

/* Hostname in use for this boot. It does not change after initialization. */
const char *app_config_active_hostname(void);

/* Hostname persisted for the next boot; it may differ from the active value. */
const char *app_config_saved_hostname(void);

/* Validates a DNS label and writes its lowercase form to normalized. */
esp_err_t app_config_normalize_hostname(
    const char *input,
    char normalized[APP_CONFIG_HOSTNAME_BUFFER_SIZE]);

/*
 * Persists a valid hostname. No NVS write occurs if it is already saved.
 * This does not change the active hostname.
 */
esp_err_t app_config_save_hostname(
    const char *input,
    char normalized[APP_CONFIG_HOSTNAME_BUFFER_SIZE],
    bool *changed);

#ifdef __cplusplus
}
#endif
