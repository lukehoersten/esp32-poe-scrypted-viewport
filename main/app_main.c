#include "display.h"
#include "http_api.h"
#include "jpeg_decoder.h"
#include "mdns_service.h"
#include "net_eth.h"
#include "nvs_config.h"
#include "state_machine.h"
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
    nvs_config_load();  // apply persisted config over defaults (best-effort)
    ESP_LOGI(TAG, "Scrypted Viewport boot (v%s)", VIEWPORT_VERSION);

    ESP_ERROR_CHECK(net_eth_init());
    if (net_eth_wait_for_ip(30 * 1000) == ESP_OK) {
        ESP_LOGI(TAG, "online at %s", net_eth_get_ip_str());
    } else {
        ESP_LOGW(TAG, "no DHCP lease after 30s, will keep retrying in the background");
    }

    ESP_ERROR_CHECK(state_machine_init());
    ESP_ERROR_CHECK(mdns_service_start());
    ESP_ERROR_CHECK(http_api_start());

    // Display is best-effort — a missing/miswired panel must not kill
    // networking + /state.
    if (display_init() == ESP_OK) {
        // Reconcile panel with current run-state:
        //   UNCONFIGURED -> placeholder test pattern (M8 replaces with IP screen)
        //   ASLEEP       -> backlight off (configured device booted asleep)
        //   AWAKE        -> leave on (shouldn't happen on a fresh boot)
        viewport_state_lock();
        viewport_run_state_t s = viewport_state_get()->state;
        viewport_state_unlock();

        if (s == VIEWPORT_STATE_ASLEEP) {
            display_sleep();
            ESP_LOGI(TAG, "display up — configured, backlight off (asleep)");
        } else {
            display_test_pattern();
            ESP_LOGI(TAG, "display up — test pattern (unconfigured)");
        }
    } else {
        ESP_LOGW(TAG, "display init failed — continuing without panel");
    }

    // JPEG decoder is the M5 dependency for POST /frame. Best-effort.
    if (jpeg_decoder_init() != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decoder init failed — /frame will be unavailable");
    }

    // TODO M7: Capacitive touch -> outbound /state POST to <scrypted>/state
    // TODO M8: Local screens (IP, loading) + BOOT button
}
