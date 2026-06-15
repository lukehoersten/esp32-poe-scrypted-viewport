#include "touch.h"

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "local_screens.h"
#include "state_machine.h"
#include "viewport_state.h"

static const char *TAG = "touch";

#define FT5426_ADDR          0x38
#define FT5426_I2C_FREQ_HZ   400000

#define FT_REG_DEV_MODE      0x00
#define FT_REG_TD_STATUS     0x02
#define FT_REG_P1_XH         0x03

#define POLL_PERIOD_MS       30
#define TAP_MAX_MS           500     // < this on release = short tap (toggle)
#define LONG_PRESS_MS        1500    // ≥ this while held = info overlay
#define OVERLAY_MS           15000
#define TAP_DEBOUNCE_MS      150

static i2c_master_dev_handle_t s_dev;
static TaskHandle_t            s_task;

static esp_err_t ft_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static void on_short_tap(void)
{
    if (local_screens_overlay_active()) local_screens_overlay_dismiss();
    viewport_run_state_t target =
        (state_machine_current() == VIEWPORT_STATE_AWAKE)
            ? VIEWPORT_STATE_ASLEEP
            : VIEWPORT_STATE_AWAKE;
    ESP_LOGI(TAG, "tap -> %s", target == VIEWPORT_STATE_AWAKE ? "wake" : "sleep");
    state_machine_set_local(target);
}

static void on_long_press(void)
{
    ESP_LOGI(TAG, "long-press → info overlay for %dms", OVERLAY_MS);
    if (display_is_up()) local_screens_overlay(OVERLAY_MS);
}

// If we see N consecutive bogus polls (TD_STATUS > 5, i.e. the FT5426 is
// returning 0xff), pulse PC_RST_TP_N to try to bring it back. PoE-only
// cold boots leave the touch IC in a wedged state for ~seconds before it
// recovers; this lets us keep nudging it without blocking init.
#define WEDGE_THRESHOLD   (3000 / POLL_PERIOD_MS)   // ~3 s of bad polls
#define WEDGE_BACKOFF_MS  2000

static void touch_task(void *arg)
{
    bool     was_down       = false;
    uint64_t down_us        = 0;
    bool     long_fired     = false;
    uint64_t last_tap_us    = 0;
    uint8_t  buf[7];
    unsigned wedged_cycles  = 0;
    uint64_t last_reset_us  = 0;
    bool     announced_up   = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));

        if (ft_read(FT_REG_DEV_MODE, buf, sizeof(buf)) != ESP_OK) continue;
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        uint8_t touches = buf[FT_REG_TD_STATUS] & 0x0F;
        // FT5x06 supports max 5 simultaneous points. A reading above that
        // is bogus (typically the bus is parking 0xff because the IC isn't
        // responding) — drop the cycle and bump the wedge counter.
        if (touches > 5) {
            if (++wedged_cycles >= WEDGE_THRESHOLD &&
                now_us - last_reset_us > (uint64_t)WEDGE_BACKOFF_MS * 1000ULL) {
                ESP_LOGW(TAG, "touch wedged — pulsing reset");
                display_touch_reset_pulse();
                last_reset_us  = now_us;
                wedged_cycles  = 0;
            }
            continue;
        }
        if (wedged_cycles > 0 || !announced_up) {
            ESP_LOGI(TAG, "touch online (TD_STATUS=%u)", touches);
            wedged_cycles = 0;
            announced_up  = true;
        }
        uint64_t now    = now_us;

        if (touches > 0) {
            if (!was_down) {
                was_down   = true;
                down_us    = now;
                long_fired = false;
            } else {
                uint64_t held_ms = (now - down_us) / 1000ULL;
                if (!long_fired && held_ms >= LONG_PRESS_MS) {
                    long_fired = true;
                    on_long_press();
                }
            }
        } else if (was_down) {
            was_down = false;
            uint64_t held_ms = (now - down_us) / 1000ULL;
            if (now - last_tap_us < (uint64_t)TAP_DEBOUNCE_MS * 1000ULL) continue;

            // Short tap only fires on release; long-press already fired
            // while the finger was still down.
            if (!long_fired && held_ms > 0 && held_ms <= TAP_MAX_MS) {
                last_tap_us = now;
                on_short_tap();
            }
        }
    }
}

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = display_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "I2C bus unavailable — display didn't come up; skipping touch");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FT5426_ADDR,
        .scl_speed_hz    = FT5426_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev),
                       TAG, "i2c add FT5426");

    // Force DEV_MODE=0x00 (Working Mode) — chip might have powered up in
    // System Info mode (0x40) or Factory Test mode (0x40) which would
    // explain steady 0x00 TD_STATUS but no touch detection. Cheap to
    // attempt; ignore result since some firmware revisions reject it
    // pre-reset and the polling loop handles the alternative anyway.
    {
        uint8_t set_working[2] = { FT_REG_DEV_MODE, 0x00 };
        i2c_master_transmit(s_dev, set_working, sizeof(set_working), 50);
    }

    // Pulse the touch reset line and don't read dev_mode at init time:
    // on PoE-only boots the FT5426 hasn't finished its internal firmware
    // load when touch_init runs, and any read returns 0xff. The polling
    // loop already drops cycles where TD_STATUS > 5, so a wedged chip is
    // harmless; once its firmware finishes (often after a few hundred ms
    // of being prodded by polling reads), real touches start landing.
    display_touch_reset_pulse();
    ESP_LOGI(TAG, "FT5426 polling started "
                  "(tap=toggle wake/sleep, %dms hold=info overlay)",
             LONG_PRESS_MS);

    BaseType_t ok = xTaskCreate(touch_task, "touch", 3072, NULL, 4, &s_task);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
