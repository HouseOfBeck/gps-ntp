#include "http_status_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "clock_discipline.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_model.h"

#define HTTP_STATUS_PORT 80
#define FORM_BODY_MAX_LENGTH 255
#define RESTART_DELAY_MS 750

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

static esp_err_t prepare_html_response(httpd_req_t *request)
{
    esp_err_t err =
        httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_set_hdr(request, "Cache-Control", "no-store");
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t url_decode(const char *encoded, char *decoded,
                            size_t decoded_size)
{
    if (encoded == NULL || decoded == NULL || decoded_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t output = 0;
    for (size_t input = 0; encoded[input] != '\0'; ++input) {
        char value = encoded[input];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (encoded[input + 1] == '\0' ||
                encoded[input + 2] == '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            int high = hex_value(encoded[input + 1]);
            int low = high >= 0 ? hex_value(encoded[input + 2]) : -1;
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            value = (char)((high << 4) | low);
            input += 2;
        }

        if (output + 1 >= decoded_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        decoded[output++] = value;
    }
    decoded[output] = '\0';
    return ESP_OK;
}

static esp_err_t receive_form_body(httpd_req_t *request, char *body,
                                   size_t body_size)
{
    if (request->content_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->content_len >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < request->content_len) {
        int result = httpd_req_recv(request, body + received,
                                    request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t advanced_page(httpd_req_t *request, const char *notice,
                               bool notice_is_error)
{
    const char *active = app_config_active_hostname();
    const char *saved = app_config_saved_hostname();
    bool restart_required = strcmp(active, saved) != 0;
    char content[1024];
    esp_err_t err = prepare_html_response(request);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(
        request,
        "<!doctype html><html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Advanced Settings - GPS NTP Server</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:52rem;margin:2rem auto;"
        "padding:0 1rem;background:#111827;color:#e5e7eb}"
        "h1{font-size:1.6rem}section{background:#1f2937;padding:1rem;"
        "border-radius:.35rem}label{display:block;color:#93c5fd;"
        "font-weight:600;margin-bottom:.45rem}input{box-sizing:border-box;"
        "width:100%;max-width:28rem;font:inherit;color:#e5e7eb;"
        "background:#111827;border:1px solid #4b5563;border-radius:.3rem;"
        "padding:.55rem}.url{color:#9ca3af;overflow-wrap:anywhere}"
        "button{font:inherit;color:#111827;background:#93c5fd;border:0;"
        "border-radius:.3rem;padding:.55rem .85rem;cursor:pointer}"
        "button.restart{background:#fbbf24}.notice{padding:.7rem .8rem;"
        "border-radius:.3rem;background:#064e3b;color:#a7f3d0}"
        ".error{background:#7f1d1d;color:#fecaca}"
        "footer{margin-top:1rem;color:#9ca3af;font-size:.9rem}"
        "a{color:#93c5fd}form+form{margin-top:1rem}"
        "</style></head><body><h1>Advanced Settings</h1>");
    if (err != ESP_OK) {
        return err;
    }

    if (notice != NULL) {
        int length = snprintf(
            content, sizeof(content), "<p class=\"notice%s\">%s</p>",
            notice_is_error ? " error" : "", notice);
        if (length < 0 || length >= (int)sizeof(content)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((err = send_chunk(request, content)) != ESP_OK) {
            return err;
        }
    } else if (restart_required) {
        int length = snprintf(
            content, sizeof(content),
            "<p class=\"notice\">Hostname saved. Restart required. "
            "After restart, use: http://%s.local/</p>",
            saved);
        if (length < 0 || length >= (int)sizeof(content)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((err = send_chunk(request, content)) != ESP_OK) {
            return err;
        }
    }

    int length = snprintf(
        content, sizeof(content),
        "<section><h2>Device hostname</h2>"
        "<form method=\"post\" action=\"/advanced/hostname\">"
        "<label for=\"hostname\">Device hostname</label>"
        "<input id=\"hostname\" name=\"hostname\" value=\"%s\" maxlength=\"%d\""
        " pattern=\"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?\" required"
        " autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\">"
        "<p class=\"url\">Resulting mDNS URL:<br>"
        "<span id=\"result-url\">http://%s.local/</span></p>"
        "<button type=\"submit\">Save hostname</button></form>"
        "<form method=\"post\" action=\"/advanced/restart\">"
        "<button class=\"restart\" type=\"submit\">Restart now</button></form>"
        "</section><footer><a href=\"/\">Back to status page</a></footer>"
        "<script>(()=>{const i=document.getElementById('hostname');"
        "const u=document.getElementById('result-url');"
        "i.addEventListener('input',()=>{u.textContent='http://'+"
        "i.value.toLowerCase()+'.local/'})})();</script>"
        "</body></html>",
        saved, APP_CONFIG_HOSTNAME_MAX_LENGTH, saved);
    if (length < 0 || length >= (int)sizeof(content)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((err = send_chunk(request, content)) != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t advanced_get_handler(httpd_req_t *request)
{
    return advanced_page(request, NULL, false);
}

static esp_err_t hostname_post_handler(httpd_req_t *request)
{
    char body[FORM_BODY_MAX_LENGTH + 1];
    esp_err_t err = receive_form_body(request, body, sizeof(body));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE,
                                   "Form submission is too large");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid form submission");
    }

    char encoded[FORM_BODY_MAX_LENGTH + 1];
    err = httpd_query_key_value(body, "hostname", encoded, sizeof(encoded));
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Hostname is required");
    }

    char hostname[APP_CONFIG_HOSTNAME_BUFFER_SIZE];
    err = url_decode(encoded, hostname, sizeof(hostname));
    if (err != ESP_OK) {
        httpd_resp_set_status(request, HTTPD_400);
        return advanced_page(
            request,
            "Invalid hostname. Use 1-63 letters, digits, or hyphens; "
            "do not begin or end with a hyphen.",
            true);
    }

    char normalized[APP_CONFIG_HOSTNAME_BUFFER_SIZE];
    bool changed;
    err = app_config_save_hostname(hostname, normalized, &changed);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(request, HTTPD_400);
        return advanced_page(
            request,
            "Invalid hostname. Use 1-63 letters, digits, or hyphens; "
            "do not begin or end with a hyphen.",
            true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save hostname: %s", esp_err_to_name(err));
        httpd_resp_set_status(request, HTTPD_500);
        return advanced_page(
            request,
            "The hostname could not be saved. The appliance is still "
            "operating with its current hostname.",
            true);
    }

    const bool restart_required =
        strcmp(normalized, app_config_active_hostname()) != 0;
    char notice[256];
    if (restart_required) {
        snprintf(notice, sizeof(notice),
                 "Hostname saved. Restart required. After restart, use: "
                 "http://%s.local/",
                 normalized);
    } else if (changed) {
        snprintf(notice, sizeof(notice),
                 "Hostname saved. It already matches the active hostname.");
    } else {
        snprintf(notice, sizeof(notice),
                 "Hostname is unchanged. No NVS write was needed.");
    }
    return advanced_page(request, notice, false);
}

static esp_err_t restart_post_handler(httpd_req_t *request)
{
    esp_err_t err = prepare_html_response(request);
    if (err == ESP_OK) {
        err = httpd_resp_send(
            request,
            "<!doctype html><html lang=\"en\"><head>"
            "<meta charset=\"utf-8\"><meta name=\"viewport\" "
            "content=\"width=device-width,initial-scale=1\">"
            "<title>Restarting GPS NTP Server</title>"
            "<style>body{font-family:system-ui,sans-serif;max-width:40rem;"
            "margin:2rem auto;padding:0 1rem;background:#111827;"
            "color:#e5e7eb}a{color:#93c5fd}</style></head>"
            "<body><h1>Restarting</h1><p>The GPS NTP server is restarting. "
            "Reconnect using its saved hostname after a few seconds.</p>"
            "<p><a href=\"/\">Return to status page</a></p></body></html>",
            HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Restart confirmation response failed: %s",
                 esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

static esp_err_t root_handler(httpd_req_t *request)
{
    status_model_snapshot_t status = status_model_snapshot();
    char value[128];
    esp_err_t err;

    err = prepare_html_response(request);
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
        "a{color:#93c5fd}"
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

    snprintf(value, sizeof(value), "%s.local", status.hostname);
    if ((err = send_row(request, "Hostname", value)) != ESP_OK) {
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
        "<span aria-hidden=\"true\">&middot;</span>"
        "<a href=\"/advanced\">Advanced settings</a>"
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
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    const httpd_uri_t routes[] = {
        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/advanced",
            .method = HTTP_GET,
            .handler = advanced_get_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/advanced/hostname",
            .method = HTTP_POST,
            .handler = hostname_post_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/advanced/restart",
            .method = HTTP_POST,
            .handler = restart_post_handler,
            .user_ctx = NULL,
        },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            esp_err_t stop_err = httpd_stop(s_server);
            s_server = NULL;
            return stop_err == ESP_OK ? err : stop_err;
        }
    }

    ESP_LOGI(TAG, "Status and advanced pages listening on TCP/%d",
             HTTP_STATUS_PORT);
    return ESP_OK;
}
