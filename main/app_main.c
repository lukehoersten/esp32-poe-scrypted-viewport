#include "button.h"
#include "display.h"
#include "http_api.h"
#include "jpeg_decoder.h"
#include "local_screens.h"
#include "mdns_service.h"
#include "net_eth.h"
#include "nvs_config.h"
#include "state_client.h"
#include "state_machine.h"
#include "touch.h"
#include "viewport_state.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "viewport";

// Log a status line for one subsystem and accumulate a one-letter flag into
// `status` (uppercase = up, lowercase = degraded/unavailable). Lets the
// boot log end with a single line summarizing what's actually live, so
// running with no LAN / no panel is a self-describing degraded mode rather
// than a panic.
static inline void mark(esp_err_t err, char up, char *slot)
{
    *slot = (err == ESP_OK) ? up : (char)(up + 0x20);  // 'A' -> 'a' on failure
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    viewport_state_init();
    nvs_config_load();  // apply persisted config over defaults (best-effort)
    ESP_LOGI(TAG, "Scrypted Viewport boot (v%s)", VIEWPORT_VERSION);

    // ------------------------------------------------------------------
    // Networking. Driver starts even with no cable plugged in; we just
    // don't get a DHCP lease. mDNS + HTTP advertise / bind anyway and will
    // start serving the moment the link comes up.
    // ------------------------------------------------------------------
    char flags[8] = { '-','-','-','-','-','-','-', 0 };  // E M H D J T B
    esp_err_t eth_err = net_eth_init();
    mark(eth_err, 'E', &flags[0]);

    bool got_ip = false;
    if (eth_err == ESP_OK) {
        got_ip = (net_eth_wait_for_ip(15 * 1000) == ESP_OK);
        if (got_ip) {
            ESP_LOGI(TAG, "online at %s", net_eth_get_ip_str());
        } else {
            ESP_LOGW(TAG, "no DHCP lease after 15s — Ethernet driver will keep "
                          "retrying in the background; mDNS + HTTP up anyway");
        }
    }

    ESP_ERROR_CHECK(state_machine_init());
    ESP_ERROR_CHECK(state_client_init());
    mark(mdns_service_start(), 'M', &flags[1]);
    mark(http_api_start(),     'H', &flags[2]);

    // ------------------------------------------------------------------
    // Display + I²C-bound peripherals. Missing/miswired panel must not
    // kill networking + /state. Failures here log a warning and continue.
    // ------------------------------------------------------------------
    esp_err_t dsp_err = display_init();
    mark(dsp_err, 'D', &flags[3]);
    if (dsp_err == ESP_OK) {
        local_screens_init();
        viewport_state_lock();
        viewport_run_state_t s = viewport_state_get()->state;
        viewport_state_unlock();
        if (s == VIEWPORT_STATE_ASLEEP) {
            display_sleep();
        } else {
            local_screens_show_ip();
        }
    }

    mark(jpeg_decoder_init(), 'J', &flags[4]);

    if (dsp_err == ESP_OK) {
        mark(touch_init(), 'T', &flags[5]);
    } else {
        ESP_LOGI(TAG, "touch skipped (display not up)");
    }

    mark(button_init(), 'B', &flags[6]);

    // ------------------------------------------------------------------
    // Boot summary. Uppercase letter = subsystem live; lowercase = down.
    // E=Ethernet M=mDNS H=HTTP D=Display J=JPEG T=Touch B=BootButton
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "boot complete — subsystems [%s]  ip=%s",
             flags, got_ip ? net_eth_get_ip_str() : "(no link)");
}
