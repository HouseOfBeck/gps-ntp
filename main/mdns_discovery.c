#include "mdns_discovery.h"

#include <stdbool.h>

#include "esp_log.h"
#include "mdns.h"

#define MDNS_INSTANCE_NAME "GPS-Disciplined NTP Server"
#define HTTP_PORT 80
#define NTP_PORT 123

static const char *TAG = "mdns";

static esp_err_t initialization_failed(const char *step, esp_err_t err,
                                       bool initialized)
{
    ESP_LOGW(TAG, "Initialization failed at %s: %s",
             step, esp_err_to_name(err));
    if (initialized) {
        mdns_free();
    }
    return err;
}

esp_err_t mdns_discovery_start(const char *hostname)
{
    if (hostname == NULL || hostname[0] == '\0') {
        ESP_LOGW(TAG, "Initialization failed: hostname is empty");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return initialization_failed("mdns_init", err, false);
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        return initialization_failed("hostname", err, true);
    }

    err = mdns_instance_name_set(MDNS_INSTANCE_NAME);
    if (err != ESP_OK) {
        return initialization_failed("instance name", err, true);
    }

    err = mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
    if (err != ESP_OK) {
        return initialization_failed("_http._tcp service", err, true);
    }

    err = mdns_service_add(NULL, "_ntp", "_udp", NTP_PORT, NULL, 0);
    if (err != ESP_OK) {
        return initialization_failed("_ntp._udp service", err, true);
    }

    ESP_LOGI(TAG,
             "Initialized %s.local as \"%s\"; services: "
             "_http._tcp:%d, _ntp._udp:%d",
             hostname, MDNS_INSTANCE_NAME, HTTP_PORT, NTP_PORT);
    return ESP_OK;
}
