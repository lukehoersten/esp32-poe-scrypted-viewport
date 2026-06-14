#include "display.h"
#include "http_api.h"
#include "mdns_service.h"
#include "net_eth.h"
#include "viewport_state.h"

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

    viewport_state_init();
    ESP_LOGI(TAG, "Scrypted Viewport boot (v%s)", VIEWPORT_VERSION);

    ESP_ERROR_CHECK(net_eth_init());
    if (net_eth_wait_for_ip(30 * 1000) == ESP_OK) {
        ESP_LOGI(TAG, "online at %s", net_eth_get_ip_str());
    } else {
        ESP_LOGW(TAG, "no DHCP lease after 30s, will keep retrying in the background");
    }

    ESP_ERROR_CHECK(mdns_service_start());
    ESP_ERROR_CHECK(http_api_start());

    // Display is best-effort during bring-up: a missing/miswired panel must
    // not kill networking + /state. M3 acceptance: show a test pattern.
    if (display_init() == ESP_OK) {
        display_test_pattern();
        ESP_LOGI(TAG, "display up — test pattern on screen");
    } else {
        ESP_LOGW(TAG, "display init failed — continuing without panel");
    }

    // TODO M4: /config persistence (NVS) + GET /config + POST /config
    // TODO M5: /frame JPEG decode -> framebuffer
    // TODO M6: POST /state + idle timer
    // TODO M7: Capacitive touch -> outbound /state POST
    // TODO M8: Local screens (IP, loading) + BOOT button
}
