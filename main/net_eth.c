#include "net_eth.h"

#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Waveshare ESP32-P4-ETH-POE pin map. PHY is IP101GRI at MDIO addr 1.
// REF_CLK is 50 MHz output from the PHY into GPIO50 (CLK_EXT_IN).
// Verified against Waveshare wiki and ESPHome's working config.
#define PIN_PHY_MDC         31
#define PIN_PHY_MDIO        52
#define PIN_PHY_REF_CLK     50
#define PIN_PHY_TX_EN       49
#define PIN_PHY_TXD0        34
#define PIN_PHY_TXD1        35
#define PIN_PHY_CRS_DV      28
#define PIN_PHY_RXD0        30
#define PIN_PHY_RXD1        29
#define PIN_PHY_RESET       51
#define PHY_ADDR            1

static const char *TAG = "net_eth";

static EventGroupHandle_t s_event_group;
static const int BIT_GOT_IP = BIT0;

static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;
static char s_ip_str[16] = {0};

static void on_eth_event(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    uint8_t mac[6] = {0};
    esp_eth_handle_t handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "link up, mac %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        xEventGroupClearBits(s_event_group, BIT_GOT_IP);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "ethernet stopped");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip = &event->ip_info;

    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip->ip));
    ESP_LOGI(TAG, "got ip %s  gw " IPSTR "  netmask " IPSTR,
             s_ip_str, IP2STR(&ip->gw), IP2STR(&ip->netmask));

    xEventGroupSetBits(s_event_group, BIT_GOT_IP);
}

esp_err_t net_eth_init(void)
{
    s_event_group = xEventGroupCreate();
    if (!s_event_group) return ESP_ERR_NO_MEM;

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) return ESP_FAIL;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num  = PIN_PHY_MDC;
    emac_config.smi_gpio.mdio_num = PIN_PHY_MDIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = PIN_PHY_REF_CLK;
    // ESP32-P4 EMAC also needs the data pins routed explicitly.
    emac_config.emac_dataif_gpio.rmii.tx_en_num = PIN_PHY_TX_EN;
    emac_config.emac_dataif_gpio.rmii.txd0_num  = PIN_PHY_TXD0;
    emac_config.emac_dataif_gpio.rmii.txd1_num  = PIN_PHY_TXD1;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = PIN_PHY_CRS_DV;
    emac_config.emac_dataif_gpio.rmii.rxd0_num  = PIN_PHY_RXD0;
    emac_config.emac_dataif_gpio.rmii.rxd1_num  = PIN_PHY_RXD1;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 failed");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = PHY_ADDR;
    phy_config.reset_gpio_num = PIN_PHY_RESET;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_ip101 failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle),
                       TAG, "driver install failed");

    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif,
                                        esp_eth_new_netif_glue(s_eth_handle)),
                       TAG, "netif attach failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                  on_eth_event, NULL),
                       TAG, "eth event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                  on_ip_event, NULL),
                       TAG, "ip event register failed");

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start failed");
    ESP_LOGI(TAG, "ethernet driver started, waiting for link + DHCP");
    return ESP_OK;
}

esp_err_t net_eth_wait_for_ip(uint32_t timeout_ms)
{
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY
                                                : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_event_group, BIT_GOT_IP,
                                           pdFALSE, pdTRUE, ticks);
    return (bits & BIT_GOT_IP) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool net_eth_is_up(void)
{
    return (xEventGroupGetBits(s_event_group) & BIT_GOT_IP) != 0;
}

const char *net_eth_get_ip_str(void)
{
    return s_ip_str;
}
