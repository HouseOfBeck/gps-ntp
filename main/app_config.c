#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_CONFIG_NAMESPACE "gps_ntp"
#define APP_CONFIG_HOSTNAME_KEY "hostname"
#define APP_CONFIG_DEFAULT_HOSTNAME "clock"

static const char *TAG = "app_config";

static nvs_handle_t s_nvs_handle;
static bool s_nvs_ready;
static char s_active_hostname[APP_CONFIG_HOSTNAME_BUFFER_SIZE] =
    APP_CONFIG_DEFAULT_HOSTNAME;
static char s_saved_hostname[APP_CONFIG_HOSTNAME_BUFFER_SIZE] =
    APP_CONFIG_DEFAULT_HOSTNAME;

esp_err_t app_config_normalize_hostname(
    const char *input,
    char normalized[APP_CONFIG_HOSTNAME_BUFFER_SIZE])
{
    if (input == NULL || normalized == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strnlen(input, APP_CONFIG_HOSTNAME_BUFFER_SIZE);
    if (length == 0 || length > APP_CONFIG_HOSTNAME_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (input[0] == '-' || input[length - 1] == '-') {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < length; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        } else if (!((c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '-')) {
            return ESP_ERR_INVALID_ARG;
        }
        normalized[i] = c;
    }
    normalized[length] = '\0';
    return ESP_OK;
}

esp_err_t app_config_init(void)
{
    strlcpy(s_active_hostname, APP_CONFIG_DEFAULT_HOSTNAME,
            sizeof(s_active_hostname));
    strlcpy(s_saved_hostname, APP_CONFIG_DEFAULT_HOSTNAME,
            sizeof(s_saved_hostname));
    s_nvs_ready = false;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS initialization failed; using default hostname: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS namespace; using default hostname: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_nvs_ready = true;

    char stored[APP_CONFIG_HOSTNAME_BUFFER_SIZE];
    size_t stored_size = sizeof(stored);
    err = nvs_get_str(s_nvs_handle, APP_CONFIG_HOSTNAME_KEY, stored,
                      &stored_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved hostname; using default '%s'",
                 APP_CONFIG_DEFAULT_HOSTNAME);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read hostname; using default: %s",
                 esp_err_to_name(err));
        return err;
    }

    char normalized[APP_CONFIG_HOSTNAME_BUFFER_SIZE];
    err = app_config_normalize_hostname(stored, normalized);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved hostname is invalid; using default '%s'",
                 APP_CONFIG_DEFAULT_HOSTNAME);
        return ESP_OK;
    }

    strlcpy(s_active_hostname, normalized, sizeof(s_active_hostname));
    strlcpy(s_saved_hostname, normalized, sizeof(s_saved_hostname));
    ESP_LOGI(TAG, "Loaded hostname '%s'", s_active_hostname);
    return ESP_OK;
}

const char *app_config_active_hostname(void)
{
    return s_active_hostname;
}

const char *app_config_saved_hostname(void)
{
    return s_saved_hostname;
}

esp_err_t app_config_save_hostname(
    const char *input,
    char normalized[APP_CONFIG_HOSTNAME_BUFFER_SIZE],
    bool *changed)
{
    if (changed != NULL) {
        *changed = false;
    }

    esp_err_t err = app_config_normalize_hostname(input, normalized);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(normalized, s_saved_hostname) == 0) {
        return ESP_OK;
    }

    err = nvs_set_str(s_nvs_handle, APP_CONFIG_HOSTNAME_KEY, normalized);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_saved_hostname, normalized, sizeof(s_saved_hostname));
    if (changed != NULL) {
        *changed = true;
    }
    ESP_LOGI(TAG, "Saved hostname '%s' for next boot", normalized);
    return ESP_OK;
}
