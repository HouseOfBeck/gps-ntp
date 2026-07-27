#include "gps_receiver.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock_discipline.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GPS_UART UART_NUM_1
#define GPS_TX_GPIO GPIO_NUM_43
#define GPS_RX_GPIO GPIO_NUM_44
#define GPS_PPS_GPIO GPIO_NUM_3
#define GPS_BAUD 9600
#define UART_BUF_SIZE 1024
#define NMEA_LINE_SIZE 128

static const char *TAG = "gps";

static bool digits(const char *value, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!isdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool nmea_checksum_valid(const char *line)
{
    if (line[0] != '$') {
        return false;
    }

    const char *star = strchr(line, '*');
    if (star == NULL || strlen(star) != 3 ||
        !isxdigit((unsigned char)star[1]) ||
        !isxdigit((unsigned char)star[2])) {
        return false;
    }

    unsigned checksum = 0;
    for (const char *p = line + 1; p < star; p++) {
        checksum ^= (unsigned char)*p;
    }

    char expected[3] = {star[1], star[2], '\0'};
    return checksum == strtoul(expected, NULL, 16);
}

static int64_t utc_to_unix(int year, int month, int day,
                           int hour, int minute, int second)
{
    struct tm value = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = 0,
    };
    return (int64_t)timegm(&value);
}

static void process_rmc(const char *line)
{
    if (strncmp(line, "$GPRMC,", 7) != 0 &&
        strncmp(line, "$GNRMC,", 7) != 0) {
        return;
    }
    if (!nmea_checksum_valid(line)) {
        ESP_LOGW(TAG, "Ignoring RMC with bad checksum");
        return;
    }

    char copy[NMEA_LINE_SIZE];
    char *fields[16];
    int field_count = 0;

    strlcpy(copy, line, sizeof(copy));
    char *star = strchr(copy, '*');
    if (star != NULL) {
        *star = '\0';
    }

    fields[field_count++] = copy;
    for (char *p = copy; *p != '\0' && field_count < 16; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[field_count++] = p + 1;
        }
    }
    if (field_count <= 9) {
        return;
    }

    const char *utc_time = fields[1];
    const char *status = fields[2];
    const char *date = fields[9];

    if (status[0] != 'A') {
        clock_discipline_report_invalid_rmc();
        return;
    }
    if (strlen(utc_time) < 6 || strlen(date) < 6 ||
        !digits(utc_time, 6) || !digits(date, 6)) {
        return;
    }

    int hour = (utc_time[0] - '0') * 10 + utc_time[1] - '0';
    int minute = (utc_time[2] - '0') * 10 + utc_time[3] - '0';
    int second = (utc_time[4] - '0') * 10 + utc_time[5] - '0';
    int day = (date[0] - '0') * 10 + date[1] - '0';
    int month = (date[2] - '0') * 10 + date[3] - '0';
    int two_digit_year = (date[4] - '0') * 10 + date[5] - '0';
    int year = two_digit_year >= 80 ? 1900 + two_digit_year
                                    : 2000 + two_digit_year;

    if (hour > 23 || minute > 59 || second > 60 ||
        day < 1 || day > 31 || month < 1 || month > 12) {
        return;
    }

    int64_t unix_sec =
        utc_to_unix(year, month, day, hour, minute, second);
    clock_anchor_result_t result =
        clock_discipline_accept_utc(unix_sec);

    if (result == CLOCK_ANCHOR_ACCEPTED) {
        clock_snapshot_t clock = clock_discipline_snapshot();
        int64_t error_x1000 =
            clock.esp_us_per_second_x1000 - 1000000000LL;
        ESP_LOGI(TAG,
                 "PPS #%" PRIu32 " UTC=%04d-%02d-%02d "
                 "%02d:%02d:%02d interval=%" PRId64
                 " rate=%" PRId64 ".%03" PRId64
                 " us/s error=%+" PRId64 ".%03" PRId64 " ppm",
                 clock.pps_count, year, month, day, hour, minute, second,
                 clock.last_pps_interval_us,
                 clock.esp_us_per_second_x1000 / 1000,
                 llabs(clock.esp_us_per_second_x1000 % 1000),
                 error_x1000 / 1000,
                 llabs(error_x1000 % 1000));
    } else if (result != CLOCK_ANCHOR_DUPLICATE_PPS) {
        ESP_LOGW(TAG, "RMC could not be paired with a fresh PPS (%d)",
                 (int)result);
    }
}

static void gps_task(void *arg)
{
    (void)arg;
    uint8_t uart_data[128];
    char line[NMEA_LINE_SIZE];
    size_t line_pos = 0;
    clock_state_t previous_state = (clock_state_t)-1;

    while (true) {
        int len = uart_read_bytes(GPS_UART, uart_data, sizeof(uart_data),
                                  pdMS_TO_TICKS(100));
        if (len < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed: %d", len);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (int i = 0; i < len; i++) {
            char c = (char)uart_data[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                if (line_pos > 0) {
                    line[line_pos] = '\0';
                    process_rmc(line);
                    line_pos = 0;
                }
            } else if (line_pos < sizeof(line) - 1) {
                line[line_pos++] = c;
            } else {
                line_pos = 0;
            }
        }

        clock_state_t state = clock_discipline_state();
        if (state != previous_state) {
            ESP_LOGI(TAG, "Clock state: %s", clock_state_name(state));
            previous_state = state;
        }
    }
}

esp_err_t gps_receiver_start(void)
{
    const uart_config_t uart_config = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(
        GPS_UART, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(GPS_UART, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(GPS_UART, GPS_TX_GPIO, GPS_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = clock_discipline_init(GPS_PPS_GPIO);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreate(
        gps_task, "gps", 4096, NULL, 10, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GPS RX=%d TX=%d PPS=%d at %d baud",
             GPS_RX_GPIO, GPS_TX_GPIO, GPS_PPS_GPIO, GPS_BAUD);
    return ESP_OK;
}

