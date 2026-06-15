#include "viewport_state.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static viewport_state_t s_state;
static SemaphoreHandle_t s_mutex;

// Populate viewport_name with the full base MAC, colons stripped
// (e.g. e8:f6:0a:e0:90:94 → "e8f60ae09094"). Stable across reboots,
// globally unique, and 12 alphanumeric chars — fits comfortably under
// the 32-char mDNS hostname limit with the "viewport-" prefix.
// POST /config can override it with a friendlier name.
static void seed_mac_and_name(char *mac_str, size_t mac_cap,
                              char *name,    size_t name_cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(mac_str, mac_cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(name, name_cap, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void viewport_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.configured      = false;
    s_state.state           = VIEWPORT_STATE_ASLEEP;
    s_state.brightness      = 80;
    s_state.idle_timeout_ms = 60000;
    s_state.orientation     = VIEWPORT_ORIENTATION_PORTRAIT;
    s_state.boot_us         = (uint64_t)esp_timer_get_time();
    s_state.last_frame_us   = -1;
    seed_mac_and_name(s_state.mac_str,       sizeof(s_state.mac_str),
                      s_state.viewport_name, sizeof(s_state.viewport_name));

    s_mutex = xSemaphoreCreateMutex();
}

viewport_state_t *viewport_state_get(void)
{
    return &s_state;
}

void viewport_state_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void viewport_state_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

const char *viewport_state_resolution_str(void)
{
    return (s_state.orientation == VIEWPORT_ORIENTATION_PORTRAIT)
               ? "480x800"
               : "800x480";
}

void viewport_state_effective_dims(uint16_t *w, uint16_t *h)
{
    viewport_state_lock();
    viewport_orientation_t o = s_state.orientation;
    viewport_state_unlock();
    if (o == VIEWPORT_ORIENTATION_PORTRAIT) { *w = 480; *h = 800; }
    else                                    { *w = 800; *h = 480; }
}
