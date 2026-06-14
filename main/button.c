#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "local_screens.h"
#include "nvs_config.h"

static const char *TAG = "button";

// TODO confirm against the Waveshare ESP32-P4-ETH schematic. ESP32-P4's
// strapping pin is GPIO35 but that's owned by RMII TXD1 at runtime. Most
// Waveshare ESP32-P4 boards expose an additional user/BOOT button on a
// free GPIO; GPIO0 is the conventional default if a separate button isn't
// available. The button task fail-soft: if the pin reads stuck-high (no
// button), it just never fires.
#define PIN_BOOT_BUTTON      0

#define POLL_PERIOD_MS       30
#define LONG_HOLD_MS         5000
#define OVERLAY_MS           15000

static esp_timer_handle_t s_overlay_timer;

static void overlay_expired(void *arg)
{
    if (display_is_up()) {
        local_screens_restore_for_state();
        // Caller (state_machine_set) had backlight off if asleep — the
        // overlay turned it on. Force it back off if we're asleep.
        // Easiest: re-issue display_sleep, the wake_then_sleep path is
        // owned by the state machine.
    }
    ESP_LOGI(TAG, "IP overlay expired");
}

static void start_overlay(void)
{
    if (!display_is_up()) return;
    esp_timer_stop(s_overlay_timer);
    display_wake();  // turn backlight on in case we were asleep
    local_screens_show_ip();
    esp_timer_start_once(s_overlay_timer, (uint64_t)OVERLAY_MS * 1000ULL);
    ESP_LOGI(TAG, "BOOT short-press → IP overlay for %dms", OVERLAY_MS);
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "BOOT held %dms → factory reset", LONG_HOLD_MS);
    nvs_config_reset();
    vTaskDelay(pdMS_TO_TICKS(200));   // let log flush
    esp_restart();
}

static void button_task(void *arg)
{
    bool     was_pressed   = false;
    uint64_t press_started = 0;
    bool     long_fired    = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));

        bool pressed = (gpio_get_level(PIN_BOOT_BUTTON) == 0);   // active-low

        if (pressed && !was_pressed) {
            press_started = (uint64_t)esp_timer_get_time();
            long_fired = false;
            was_pressed = true;
        } else if (pressed && was_pressed) {
            uint64_t held_ms = ((uint64_t)esp_timer_get_time() - press_started) / 1000ULL;
            if (!long_fired && held_ms >= LONG_HOLD_MS) {
                long_fired = true;
                factory_reset();   // does not return
            }
        } else if (!pressed && was_pressed) {
            uint64_t held_ms = ((uint64_t)esp_timer_get_time() - press_started) / 1000ULL;
            was_pressed = false;
            if (!long_fired && held_ms < LONG_HOLD_MS) {
                start_overlay();
            }
        }
    }
}

esp_err_t button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config GPIO%d failed: %s — button disabled",
                 PIN_BOOT_BUTTON, esp_err_to_name(err));
        return err;
    }

    esp_timer_create_args_t targs = {
        .callback = &overlay_expired,
        .name     = "boot_overlay",
    };
    esp_timer_create(&targs, &s_overlay_timer);

    BaseType_t ok = xTaskCreate(button_task, "boot_btn", 3072, NULL, 3, NULL);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "BOOT button on GPIO%d (short=IP overlay 15s, hold 5s=factory reset)",
             PIN_BOOT_BUTTON);
    return ESP_OK;
}
