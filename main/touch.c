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

// FT5x06/FT5426 register map (subset).
#define FT_REG_DEV_MODE      0x00
#define FT_REG_GEST_ID       0x01
#define FT_REG_TD_STATUS     0x02   // low 4 bits = touch count
#define FT_REG_P1_XH         0x03   // X high (bits 0..3) + event flag (bits 6..7)
#define FT_REG_P1_XL         0x04
#define FT_REG_P1_YH         0x05
#define FT_REG_P1_YL         0x06

#define POLL_PERIOD_MS       30
#define TAP_MAX_MS           500
#define TAP_MAX_MOVE_PX      25
#define TAP_DEBOUNCE_MS      150

static i2c_master_dev_handle_t s_dev;
static TaskHandle_t            s_task;

static esp_err_t ft_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static void touch_task(void *arg)
{
    bool     was_down       = false;
    uint64_t down_us        = 0;
    uint16_t down_x         = 0;
    uint16_t down_y         = 0;
    uint64_t last_tap_us    = 0;
    uint8_t  buf[7];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));

        if (ft_read(FT_REG_DEV_MODE, buf, sizeof(buf)) != ESP_OK) continue;

        uint8_t touches = buf[FT_REG_TD_STATUS] & 0x0F;
        if (touches > 0) {
            // Always capture P1; we only care about taps, not multi-touch.
            uint16_t x = ((buf[FT_REG_P1_XH] & 0x0F) << 8) | buf[FT_REG_P1_XL];
            uint16_t y = ((buf[FT_REG_P1_YH] & 0x0F) << 8) | buf[FT_REG_P1_YL];
            if (!was_down) {
                was_down = true;
                down_us  = (uint64_t)esp_timer_get_time();
                down_x   = x;
                down_y   = y;
            }
        } else if (was_down) {
            // Release: was-down → up.
            was_down = false;
            uint64_t now = (uint64_t)esp_timer_get_time();

            if (now - last_tap_us < (uint64_t)TAP_DEBOUNCE_MS * 1000ULL) continue;

            uint64_t dt_ms = (now - down_us) / 1000ULL;
            // No reliable last position from the burst; use down coordinates
            // for the duration check, ignore movement check beyond the down
            // capture. Good enough for tap-vs-press.
            (void)down_x; (void)down_y; (void)TAP_MAX_MOVE_PX;

            if (dt_ms > 0 && dt_ms <= TAP_MAX_MS) {
                last_tap_us = now;
                // Cancel any BOOT-button identity overlay so a tap-to-sleep
                // doesn't fight the overlay timer trying to restore content.
                if (local_screens_overlay_active()) local_screens_overlay_dismiss();
                viewport_run_state_t target =
                    (state_machine_current() == VIEWPORT_STATE_AWAKE)
                        ? VIEWPORT_STATE_ASLEEP
                        : VIEWPORT_STATE_AWAKE;
                ESP_LOGI(TAG, "tap (%lu ms) -> %s", (unsigned long)dt_ms,
                         target == VIEWPORT_STATE_AWAKE ? "wake" : "sleep");
                state_machine_set_local(target);
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

    // Sanity poll: read device mode.
    uint8_t dev_mode = 0;
    esp_err_t err = ft_read(FT_REG_DEV_MODE, &dev_mode, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FT5426 @0x38 unreachable (%s) — check INT jumper",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "FT5426 ack'd (dev_mode=0x%02x)", dev_mode);

    BaseType_t ok = xTaskCreate(touch_task, "touch", 3072, NULL, 4, &s_task);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
