#include "net_eth.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "viewport";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Scrypted Viewport boot");

    ESP_ERROR_CHECK(net_eth_init());
    if (net_eth_wait_for_ip(30 * 1000) == ESP_OK) {
        ESP_LOGI(TAG, "online at %s", net_eth_get_ip_str());
    } else {
        ESP_LOGW(TAG, "no DHCP lease after 30s, will keep retrying in the background");
    }

    // TODO M2: mDNS _scrypted-viewport._tcp.local
    // TODO M2: HTTP server: GET /state
    // TODO M3: MIPI-DSI panel init (800x480 IPS, default portrait 480x800)
    // TODO M4: /config persistence (NVS)
    // TODO M5: /frame JPEG decode -> framebuffer
    // TODO M6: /state POST + idle timer
    // TODO M7: Capacitive touch -> outbound /state POST
    // TODO M8: Local screens (IP, loading) + BOOT button
}
