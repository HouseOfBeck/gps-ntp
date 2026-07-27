#include "ethernet_w5500.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#define ETH_MOSI_GPIO GPIO_NUM_11
#define ETH_MISO_GPIO GPIO_NUM_12
#define ETH_SCLK_GPIO GPIO_NUM_13
#define ETH_CS_GPIO GPIO_NUM_14
#define ETH_INT_GPIO GPIO_NUM_10
#define ETH_RST_GPIO GPIO_NUM_9
#define ETH_SPI_HOST SPI2_HOST

static const char *TAG = "ethernet";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static ethernet_status_t s_status;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        portENTER_CRITICAL(&s_status_lock);
        s_status.link_up = true;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "Link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&s_status_lock);
        s_status.link_up = false;
        s_status.has_ipv4 = false;
        s_status.ipv4[0] = '\0';
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGW(TAG, "Link down");
        break;
    case ETHERNET_EVENT_START:
        portENTER_CRITICAL(&s_status_lock);
        s_status.started = true;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "Started");
        break;
    case ETHERNET_EVENT_STOP:
        portENTER_CRITICAL(&s_status_lock);
        memset(&s_status, 0, sizeof(s_status));
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "Stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = event_data;
    const esp_netif_ip_info_t *ip = &event->ip_info;
    char ipv4[sizeof(s_status.ipv4)];

    snprintf(ipv4, sizeof(ipv4), IPSTR, IP2STR(&ip->ip));
    portENTER_CRITICAL(&s_status_lock);
    memcpy(s_status.ipv4, ipv4, sizeof(s_status.ipv4));
    s_status.has_ipv4 = true;
    portEXIT_CRITICAL(&s_status_lock);

    ESP_LOGI(TAG, "DHCP address: " IPSTR, IP2STR(&ip->ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip->gw));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip->netmask));
}

static void lost_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    portENTER_CRITICAL(&s_status_lock);
    s_status.has_ipv4 = false;
    s_status.ipv4[0] = '\0';
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGW(TAG, "IPv4 address lost");
}

esp_err_t ethernet_w5500_start(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    if (netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = ETH_MOSI_GPIO,
        .miso_io_num = ETH_MISO_GPIO,
        .sclk_io_num = ETH_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    err = spi_bus_initialize(ETH_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = ETH_CS_GPIO,
        .queue_size = 20,
    };
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_RST_GPIO;
    phy_config.phy_addr = 1;

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &dev_cfg);
    w5500_config.base.int_gpio_num = ETH_INT_GPIO;

    esp_eth_mac_t *mac =
        esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (mac == NULL || phy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t eth_mac[6];
    err = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             eth_mac[0], eth_mac[1], eth_mac[2],
             eth_mac[3], eth_mac[4], eth_mac[5]);

    esp_eth_netif_glue_handle_t glue =
        esp_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_netif_attach(netif, glue);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_LOST_IP, lost_ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return esp_eth_start(eth_handle);
}

ethernet_status_t ethernet_w5500_status(void)
{
    ethernet_status_t status;

    portENTER_CRITICAL(&s_status_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return status;
}
