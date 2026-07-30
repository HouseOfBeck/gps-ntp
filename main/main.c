#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "ethernet_w5500.h"
#include "gps_receiver.h"
#include "http_status_server.h"
#include "mdns_discovery.h"
#include "ntp_server.h"
#include "oled_display.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting GPS-disciplined W5500 NTP server");

    esp_err_t config_err = app_config_init();
    if (config_err != ESP_OK) {
        ESP_LOGW(TAG, "Persistent configuration unavailable; using defaults: %s",
                 esp_err_to_name(config_err));
    }

    ESP_ERROR_CHECK(gps_receiver_start());
    ESP_ERROR_CHECK(ethernet_w5500_start());
    ESP_ERROR_CHECK(ntp_server_start());

    esp_err_t http_err = http_status_server_start();
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP status server not started: %s",
                 esp_err_to_name(http_err));
    }

    /*
     * Discovery is optional. mdns_discovery_start() logs the failing
     * initialization step and cleans up any partial mDNS state.
     */
    (void)mdns_discovery_start(app_config_active_hostname());

    esp_err_t display_err = oled_display_start();
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "OLED task not started: %s",
                 esp_err_to_name(display_err));
    }
}
