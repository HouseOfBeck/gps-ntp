#include "ntp_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "clock_discipline.h"
#include "clock_math.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_UNIX_EPOCH_OFFSET 2208988800ULL

#define NTP_LI_NO_WARNING 0
#define NTP_LI_UNSYNC 3
#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4
#define NTP_STRATUM_PRIMARY 1
#define NTP_STRATUM_UNSYNC 16

static const char *TAG = "ntp";

static void put_u32(uint8_t *destination, uint32_t value)
{
    uint32_t network_value = htonl(value);
    memcpy(destination, &network_value, sizeof(network_value));
}

static void put_timestamp(uint8_t *destination, int64_t unix_us)
{
    int64_t unix_sec = unix_us / 1000000LL;
    int64_t usec = unix_us % 1000000LL;
    if (usec < 0) {
        unix_sec--;
        usec += 1000000LL;
    }

    uint64_t ntp_sec = (uint64_t)(unix_sec + NTP_UNIX_EPOCH_OFFSET);
    uint32_t fraction =
        clock_math_ntp_fraction_from_us((uint32_t)usec);
    put_u32(destination, (uint32_t)ntp_sec);
    put_u32(destination + 4, fraction);
}

static void build_response(uint8_t response[NTP_PACKET_SIZE],
                           const uint8_t request[NTP_PACKET_SIZE],
                           const clock_snapshot_t *receive_clock)
{
    memset(response, 0, NTP_PACKET_SIZE);

    uint8_t version = (request[0] >> 3) & 0x07;
    response[2] = request[2];
    response[3] = (uint8_t)-20; /* Approximately one microsecond. */
    memcpy(response + 24, request + 40, 8);

    if (receive_clock->time_valid) {
        put_timestamp(response + 32, receive_clock->unix_us);
    }

    clock_snapshot_t transmit_clock = clock_discipline_snapshot();
    bool synced = transmit_clock.synchronized;
    uint8_t leap = synced ? NTP_LI_NO_WARNING : NTP_LI_UNSYNC;

    response[0] = (leap << 6) | (version << 3) | NTP_MODE_SERVER;
    response[1] = synced ? NTP_STRATUM_PRIMARY : NTP_STRATUM_UNSYNC;

    if (synced) {
        /* Root dispersion of about 1 ms in NTP 16.16 format. */
        put_u32(response + 8, 66);
        memcpy(response + 12, "GPS\0", 4);
        put_timestamp(response + 16,
                      transmit_clock.reference_unix_sec * 1000000LL);
    } else {
        put_u32(response + 8, UINT32_MAX);
        memcpy(response + 12, "INIT", 4);
        if (transmit_clock.time_valid) {
            put_timestamp(response + 16,
                          transmit_clock.reference_unix_sec * 1000000LL);
        }
    }

    if (transmit_clock.time_valid) {
        put_timestamp(response + 40, transmit_clock.unix_us);
    }
}

static void ntp_task(void *arg)
{
    (void)arg;
    uint8_t request[128];
    uint8_t response[NTP_PACKET_SIZE];

    while (true) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(NTP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
            ESP_LOGE(TAG, "bind UDP/%d failed: errno %d", NTP_PORT, errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Listening on UDP/%d", NTP_PORT);

        while (true) {
            struct sockaddr_storage source;
            socklen_t source_length = sizeof(source);
            int received = recvfrom(sock, request, sizeof(request), 0,
                                    (struct sockaddr *)&source,
                                    &source_length);
            clock_snapshot_t receive_clock = clock_discipline_snapshot();

            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
            if (received < NTP_PACKET_SIZE) {
                ESP_LOGW(TAG, "Ignoring short NTP packet (%d bytes)",
                         received);
                continue;
            }

            uint8_t mode = request[0] & 0x07;
            uint8_t version = (request[0] >> 3) & 0x07;
            if (mode != NTP_MODE_CLIENT || version < 3 || version > 4) {
                ESP_LOGW(TAG, "Ignoring NTP mode=%u version=%u",
                         mode, version);
                continue;
            }

            build_response(response, request, &receive_clock);
            int sent = sendto(sock, response, sizeof(response), 0,
                              (struct sockaddr *)&source, source_length);
            if (sent < 0) {
                ESP_LOGE(TAG, "sendto failed: errno %d", errno);
            } else if (sent != sizeof(response)) {
                ESP_LOGE(TAG, "sendto sent only %d of %d bytes",
                         sent, NTP_PACKET_SIZE);
            }
        }

        if (close(sock) < 0) {
            ESP_LOGE(TAG, "close failed: errno %d", errno);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t ntp_server_start(void)
{
    BaseType_t created =
        xTaskCreate(ntp_task, "ntp_server", 4096, NULL, 8, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
