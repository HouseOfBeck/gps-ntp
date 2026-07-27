#include "esp_err.h"
#include "esp_log.h"

#include "ethernet_w5500.h"
#include "gps_receiver.h"
#include "ntp_server.h"
#include "oled_display.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting GPS-disciplined W5500 NTP server");
    ESP_ERROR_CHECK(gps_receiver_start());
    ESP_ERROR_CHECK(ethernet_w5500_start());
    ESP_ERROR_CHECK(ntp_server_start());

    esp_err_t display_err = oled_display_start();
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "OLED task not started: %s",
                 esp_err_to_name(display_err));
    }
}
