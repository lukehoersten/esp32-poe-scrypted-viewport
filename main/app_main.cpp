#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>

static const char *TAG = "viewport";

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Scrypted Viewport boot");

    // TODO: Ethernet (ESP32-P4 EMAC + Waveshare PoE PHY)
    // TODO: mDNS _scrypted-viewport._tcp.local
    // TODO: HTTP server: /health /config /frame /sleep /brightness
    // TODO: MIPI-DSI panel init (800x480 IPS)
    // TODO: JPEG decode -> framebuffer
    // TODO: Capacitive touch -> callback POST
}
