#include "http_status_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "clock_discipline.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "status_model.h"

#define HTTP_STATUS_PORT 80

static const char *TAG = "http_status";
static httpd_handle_t s_server;

static esp_err_t send_chunk(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_row(httpd_req_t *request, const char *label,
                          const char *value)
{
    char row[320];
    int length = snprintf(row, sizeof(row),
                          "<tr><th>%s</th><td>%s</td></tr>",
                          label, value);
    if (length < 0 || length >= (int)sizeof(row)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_chunk(request, row);
}

static void format_utc(char *destination, size_t size, int64_t unix_us)
{
    time_t seconds = (time_t)(unix_us / 1000000LL);
    int64_t microseconds = unix_us % 1000000LL;
    if (microseconds < 0) {
        seconds--;
        microseconds += 1000000LL;
    }

    struct tm utc;
    gmtime_r(&seconds, &utc);
    snprintf(destination, size,
             "%04d-%02d-%02d %02d:%02d:%02d.%06" PRId64 " UTC",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec, microseconds);
}

static void format_uptime(char *destination, size_t size, int64_t uptime_us)
{
    uint64_t total_seconds =
        uptime_us > 0 ? (uint64_t)uptime_us / 1000000ULL : 0;
    uint64_t days = total_seconds / 86400ULL;
    uint64_t hours = (total_seconds % 86400ULL) / 3600ULL;
    uint64_t minutes = (total_seconds % 3600ULL) / 60ULL;
    uint64_t seconds = total_seconds % 60ULL;

    snprintf(destination, size,
             "%" PRIu64 "d %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
             days, hours, minutes, seconds);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    status_model_snapshot_t status = status_model_snapshot();
    char value[128];
    esp_err_t err;

    err = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(
        request,
        "<!doctype html><html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>GPS NTP Server</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:52rem;margin:2rem auto;"
        "padding:0 1rem;background:#111827;color:#e5e7eb}"
        "h1{font-size:1.6rem}table{width:100%;border-collapse:collapse;"
        "background:#1f2937}th,td{padding:.65rem .8rem;border-bottom:1px solid "
        "#374151;text-align:left}th{width:46%;color:#93c5fd}"
        ".ok{color:#86efac}.bad{color:#fca5a5}"
        "footer{display:flex;flex-wrap:wrap;align-items:center;gap:.4rem;"
        "margin-top:1rem;color:#9ca3af;font-size:.85rem}"
        "select{font:inherit;color:#e5e7eb;background:#1f2937;"
        "border:1px solid #4b5563;border-radius:.3rem;padding:.25rem .4rem}"
        "</style></head><body><h1>GPS-Disciplined NTP Server</h1><table>");
    if (err != ESP_OK) {
        return err;
    }

    snprintf(value, sizeof(value), "<span class=\"%s\">%s</span>",
             status.clock_synchronized ? "ok" : "bad",
             clock_state_name(status.clock_state));
    if ((err = send_row(request, "GPS sync state", value)) != ESP_OK) {
        return err;
    }

    snprintf(value, sizeof(value), "%" PRIu32, status.pps_count);
    if ((err = send_row(request, "PPS count", value)) != ESP_OK) {
        return err;
    }

    if (status.last_pps_interval_us > 0) {
        snprintf(value, sizeof(value), "%" PRId64 " us",
                 status.last_pps_interval_us);
    } else {
        snprintf(value, sizeof(value), "not available");
    }
    if ((err = send_row(request, "Last PPS interval", value)) != ESP_OK) {
        return err;
    }

    if (status.frequency_correction_available) {
        int64_t milli_ppm = status.frequency_correction_milli_ppm;
        uint64_t magnitude =
            (uint64_t)(milli_ppm < 0 ? -milli_ppm : milli_ppm);
        snprintf(value, sizeof(value), "%c%" PRIu64 ".%03" PRIu64 " ppm",
                 milli_ppm >= 0 ? '+' : '-',
                 magnitude / 1000ULL, magnitude % 1000ULL);
    } else {
        snprintf(value, sizeof(value), "not available");
    }
    if ((err = send_row(request, "Clock frequency correction", value)) !=
        ESP_OK) {
        return err;
    }

    if (status.phase_error_available) {
        snprintf(value, sizeof(value), "%" PRId64 " us",
                 status.phase_error_us);
    } else {
        snprintf(value, sizeof(value), "not available");
    }
    if ((err = send_row(request, "Clock phase error", value)) != ESP_OK) {
        return err;
    }

    snprintf(value, sizeof(value), "%" PRIu64, status.ntp_request_count);
    if ((err = send_row(request, "NTP requests", value)) != ESP_OK) {
        return err;
    }

    snprintf(value, sizeof(value), "%" PRIu64, status.ntp_response_count);
    if ((err = send_row(request, "NTP responses", value)) != ESP_OK) {
        return err;
    }

    if (status.ntp_last_receive_valid) {
        format_utc(value, sizeof(value),
                   status.ntp_last_receive_unix_us);
    } else {
        snprintf(value, sizeof(value), "not available");
    }
    if ((err = send_row(request, "Last NTP receive timestamp", value)) !=
        ESP_OK) {
        return err;
    }

    if (status.ntp_last_transmit_valid) {
        format_utc(value, sizeof(value),
                   status.ntp_last_transmit_unix_us);
    } else {
        snprintf(value, sizeof(value), "not available");
    }
    if ((err = send_row(request, "Last NTP transmit timestamp", value)) !=
        ESP_OK) {
        return err;
    }

    if (status.ntp_last_receive_to_transmit_valid) {
        snprintf(value, sizeof(value), "%" PRId64 " us (%s)",
                 status.ntp_last_receive_to_transmit_us,
                 status.ntp_last_transmit_synchronized
                     ? "synchronized"
                     : "unsynchronized");
    } else {
        snprintf(value, sizeof(value), "not available");
    }
    if ((err = send_row(request, "Last NTP RX-to-TX timestamp delta",
                        value)) != ESP_OK) {
        return err;
    }

    if (status.has_ipv4) {
        snprintf(value, sizeof(value), "%s", status.ipv4);
    } else if (status.ethernet_link_up) {
        snprintf(value, sizeof(value), "DHCP pending");
    } else if (status.ethernet_started) {
        snprintf(value, sizeof(value), "link down");
    } else {
        snprintf(value, sizeof(value), "Ethernet starting");
    }
    if ((err = send_row(request, "IP address", value)) != ESP_OK) {
        return err;
    }

    format_uptime(value, sizeof(value), status.uptime_us);
    if ((err = send_row(request, "Uptime", value)) != ESP_OK) {
        return err;
    }

    err = send_chunk(
        request,
        "</table><footer><label for=\"refresh-interval\">Auto-refresh:</label>"
        "<select id=\"refresh-interval\">"
        "<option value=\"0\">Off</option>"
        "<option value=\"2\">2 seconds</option>"
        "<option value=\"5\">5 seconds</option>"
        "<option value=\"15\" selected>15 seconds</option>"
        "<option value=\"30\">30 seconds</option>"
        "<option value=\"60\">60 seconds</option>"
        "</select><span aria-hidden=\"true\">&middot;</span>"
        "<span>Status reporting is independent of the NTP timing path.</span>"
        "</footer><script>"
        "(()=>{"
        "const key='gps-ntp.refresh-seconds';"
        "const allowed=['0','2','5','15','30','60'];"
        "const select=document.getElementById('refresh-interval');"
        "let timer=null;"
        "let saved=null;"
        "try{saved=localStorage.getItem(key)}catch(e){}"
        "if(!allowed.includes(saved)){saved='15'}"
        "select.value=saved;"
        "const schedule=()=>{"
        "if(timer!==null){clearTimeout(timer);timer=null}"
        "const seconds=Number(select.value);"
        "if(seconds>0){timer=setTimeout(()=>location.reload(),seconds*1000)}"
        "};"
        "select.addEventListener('change',()=>{"
        "let value=select.value;"
        "if(!allowed.includes(value)){value='15';select.value=value}"
        "try{localStorage.setItem(key,value)}catch(e){}"
        "schedule();"
        "});"
        "schedule();"
        "})();"
        "</script>"
        "</body></html>");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(request, NULL, 0);
}

esp_err_t http_status_server_start(void)
{
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_STATUS_PORT;
    config.max_uri_handlers = 1;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &root);
    if (err != ESP_OK) {
        esp_err_t stop_err = httpd_stop(s_server);
        s_server = NULL;
        return stop_err == ESP_OK ? err : stop_err;
    }

    ESP_LOGI(TAG, "Status page listening on TCP/%d", HTTP_STATUS_PORT);
    return ESP_OK;
}
