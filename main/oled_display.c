#include "oled_display.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock_discipline.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "ethernet_w5500.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_SDA_GPIO GPIO_NUM_17
#define OLED_SCL_GPIO GPIO_NUM_18
#define OLED_I2C_ADDRESS 0x3C
#define OLED_I2C_SPEED_HZ 400000
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_FRAME_BYTES (OLED_WIDTH * OLED_HEIGHT / 8)
#define OLED_TRANSFER_TIMEOUT_MS 100
#define OLED_UPDATE_PERIOD_MS 250

static const char *TAG = "oled";
static uint8_t s_frame[OLED_FRAME_BYTES];
static uint8_t s_previous_frame[OLED_FRAME_BYTES];
static uint8_t s_transfer[OLED_FRAME_BYTES + 1];
static bool s_previous_frame_valid;

static const uint8_t *glyph_for(char character)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t unknown[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const uint8_t digits[10][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e},
        {0x00, 0x42, 0x7f, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4b, 0x31},
        {0x18, 0x14, 0x12, 0x7f, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3c, 0x4a, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1e},
    };
    static const uint8_t letters[26][5] = {
        {0x7e, 0x11, 0x11, 0x11, 0x7e},
        {0x7f, 0x49, 0x49, 0x49, 0x36},
        {0x3e, 0x41, 0x41, 0x41, 0x22},
        {0x7f, 0x41, 0x41, 0x22, 0x1c},
        {0x7f, 0x49, 0x49, 0x49, 0x41},
        {0x7f, 0x09, 0x09, 0x09, 0x01},
        {0x3e, 0x41, 0x49, 0x49, 0x7a},
        {0x7f, 0x08, 0x08, 0x08, 0x7f},
        {0x00, 0x41, 0x7f, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3f, 0x01},
        {0x7f, 0x08, 0x14, 0x22, 0x41},
        {0x7f, 0x40, 0x40, 0x40, 0x40},
        {0x7f, 0x02, 0x0c, 0x02, 0x7f},
        {0x7f, 0x04, 0x08, 0x10, 0x7f},
        {0x3e, 0x41, 0x41, 0x41, 0x3e},
        {0x7f, 0x09, 0x09, 0x09, 0x06},
        {0x3e, 0x41, 0x51, 0x21, 0x5e},
        {0x7f, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7f, 0x01, 0x01},
        {0x3f, 0x40, 0x40, 0x40, 0x3f},
        {0x1f, 0x20, 0x40, 0x20, 0x1f},
        {0x3f, 0x40, 0x38, 0x40, 0x3f},
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07},
        {0x61, 0x51, 0x49, 0x45, 0x43},
    };
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t plus[5] = {0x08, 0x08, 0x3e, 0x08, 0x08};
    static const uint8_t minus[5] = {0x08, 0x08, 0x08, 0x08, 0x08};

    if (character >= 'a' && character <= 'z') {
        character -= 'a' - 'A';
    }
    if (character >= '0' && character <= '9') {
        return digits[character - '0'];
    }
    if (character >= 'A' && character <= 'Z') {
        return letters[character - 'A'];
    }
    switch (character) {
    case ' ':
        return blank;
    case ':':
        return colon;
    case '.':
        return dot;
    case '+':
        return plus;
    case '-':
        return minus;
    default:
        return unknown;
    }
}

static void set_pixel(int x, int y)
{
    if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < OLED_HEIGHT) {
        s_frame[(y / 8) * OLED_WIDTH + x] |= 1U << (y % 8);
    }
}

static void draw_text_centered(int y, const char *text)
{
    size_t length = strlen(text);
    int width = (int)(length * 6);
    int x = width < OLED_WIDTH ? (OLED_WIDTH - width) / 2 : 0;

    for (size_t character = 0;
         character < length && x + 5 <= OLED_WIDTH;
         character++, x += 6) {
        const uint8_t *glyph = glyph_for(text[character]);
        for (int column = 0; column < 5; column++) {
            for (int row = 0; row < 7; row++) {
                if (glyph[column] & (1U << row)) {
                    set_pixel(x + column, y + row);
                }
            }
        }
    }
}

static const char *display_state_name(clock_state_t state)
{
    switch (state) {
    case CLOCK_STATE_ACQUIRING:
        return "ACQUIRING";
    case CLOCK_STATE_SYNCED:
        return "SYNCED  STRATUM 1";
    case CLOCK_STATE_HOLDOVER:
        return "HOLDOVER";
    case CLOCK_STATE_PPS_LOST:
        return "PPS LOST";
    case CLOCK_STATE_UNSYNC:
    default:
        return "UNSYNC";
    }
}

static void render_frame(const clock_snapshot_t *clock,
                         const ethernet_status_t *ethernet)
{
    char line[24];
    memset(s_frame, 0, sizeof(s_frame));

    draw_text_centered(0, "GPS NTP SERVER");

    if (clock->time_valid) {
        time_t seconds = (time_t)(clock->unix_us / 1000000LL);
        struct tm utc;
        gmtime_r(&seconds, &utc);
        snprintf(line, sizeof(line), "%02d:%02d:%02d UTC",
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
        strlcpy(line, "--:--:-- UTC", sizeof(line));
    }
    draw_text_centered(13, line);

    draw_text_centered(26, display_state_name(clock->state));

    if (clock->pps_count >= 2) {
        int64_t milli_ppm =
            clock->esp_us_per_second_x1000 - 1000000000LL;
        int64_t centi_ppm =
            (milli_ppm + (milli_ppm >= 0 ? 5 : -5)) / 10;
        if (centi_ppm > 99999) {
            centi_ppm = 99999;
        } else if (centi_ppm < -99999) {
            centi_ppm = -99999;
        }
        char sign = centi_ppm >= 0 ? '+' : '-';
        int magnitude = (int)llabs(centi_ppm);
        snprintf(line, sizeof(line), "PPS %c%d.%02d PPM",
                 sign, magnitude / 100, magnitude % 100);
    } else {
        strlcpy(line, "PPS --.-- PPM", sizeof(line));
    }
    draw_text_centered(39, line);

    if (ethernet->has_ipv4) {
        strlcpy(line, ethernet->ipv4, sizeof(line));
    } else if (ethernet->link_up) {
        strlcpy(line, "DHCP...", sizeof(line));
    } else if (ethernet->started) {
        strlcpy(line, "ETH LINK DOWN", sizeof(line));
    } else {
        strlcpy(line, "ETH STARTING", sizeof(line));
    }
    draw_text_centered(52, line);
}

static esp_err_t send_commands(i2c_master_dev_handle_t device,
                               const uint8_t *commands, size_t count)
{
    uint8_t buffer[32];
    if (count + 1 > sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = 0x00;
    memcpy(buffer + 1, commands, count);
    return i2c_master_transmit(
        device, buffer, count + 1, OLED_TRANSFER_TIMEOUT_MS);
}

static esp_err_t initialize_controller(i2c_master_dev_handle_t device)
{
    /*
     * SSD1306 128x64, internal charge pump, horizontal addressing,
     * segment remap and descending COM scan for the common module wiring.
     */
    static const uint8_t commands[] = {
        0xae,       /* display off */
        0xd5, 0x80, /* clock divide */
        0xa8, 0x3f, /* multiplex: 64 rows */
        0xd3, 0x00, /* display offset */
        0x40,       /* display start line */
        0x8d, 0x14, /* charge pump on */
        0x20, 0x00, /* horizontal addressing */
        0xa1,       /* segment remap */
        0xc8,       /* COM scan direction */
        0xda, 0x12, /* COM pin configuration */
        0x81, 0x8f, /* contrast */
        0xd9, 0xf1, /* precharge */
        0xdb, 0x40, /* VCOM detect */
        0xa4,       /* resume RAM display */
        0xa6,       /* normal, not inverted */
        0x2e,       /* scroll off */
        0xaf,       /* display on */
    };
    return send_commands(device, commands, sizeof(commands));
}

static esp_err_t send_frame(i2c_master_dev_handle_t device)
{
    static const uint8_t address_commands[] = {
        0x21, 0x00, OLED_WIDTH - 1,
        0x22, 0x00, (OLED_HEIGHT / 8) - 1,
    };

    esp_err_t err =
        send_commands(device, address_commands, sizeof(address_commands));
    if (err != ESP_OK) {
        return err;
    }

    s_transfer[0] = 0x40;
    memcpy(s_transfer + 1, s_frame, sizeof(s_frame));
    return i2c_master_transmit(
        device, s_transfer, sizeof(s_transfer), OLED_TRANSFER_TIMEOUT_MS);
}

static esp_err_t create_display(i2c_master_bus_handle_t *bus,
                                i2c_master_dev_handle_t *device)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, bus);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_master_probe(*bus, OLED_I2C_ADDRESS,
                           OLED_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        i2c_del_master_bus(*bus);
        *bus = NULL;
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(*bus, &device_config, device);
    if (err != ESP_OK) {
        i2c_del_master_bus(*bus);
        *bus = NULL;
        return err;
    }

    err = initialize_controller(*device);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(*device);
        i2c_del_master_bus(*bus);
        *device = NULL;
        *bus = NULL;
    }
    return err;
}

static void display_task(void *arg)
{
    (void)arg;
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t device = NULL;
    esp_err_t err = create_display(&bus, &device);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Display unavailable at 0x%02x: %s",
                 OLED_I2C_ADDRESS, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SSD1306-compatible 128x64 display at 0x%02x",
             OLED_I2C_ADDRESS);
    TickType_t next_wake = xTaskGetTickCount();
    int consecutive_failures = 0;

    while (true) {
        clock_snapshot_t clock = clock_discipline_snapshot();
        ethernet_status_t ethernet = ethernet_w5500_status();
        render_frame(&clock, &ethernet);

        if (!s_previous_frame_valid ||
            memcmp(s_frame, s_previous_frame, sizeof(s_frame)) != 0) {
            err = send_frame(device);
            if (err == ESP_OK) {
                memcpy(s_previous_frame, s_frame, sizeof(s_frame));
                s_previous_frame_valid = true;
                consecutive_failures = 0;
            } else {
                consecutive_failures++;
                if (consecutive_failures == 1 ||
                    consecutive_failures % 20 == 0) {
                    ESP_LOGW(TAG, "Display update failed (%d): %s",
                             consecutive_failures, esp_err_to_name(err));
                }
            }
        }

        xTaskDelayUntil(&next_wake, pdMS_TO_TICKS(OLED_UPDATE_PERIOD_MS));
    }
}

esp_err_t oled_display_start(void)
{
    BaseType_t created =
        xTaskCreate(display_task, "oled", 4096, NULL, 3, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
