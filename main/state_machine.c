#include "state_machine.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "local_screens.h"
#include "state_client.h"

static const char *TAG = "state";

static esp_timer_handle_t s_idle_timer;

// ms = idle_timeout_ms snapshotted under the caller's state lock.
// ms == 0 disables the timer (caller's idle_timeout_ms feature).
static void arm_idle_timer(uint32_t ms)
{
    esp_timer_stop(s_idle_timer);
    if (ms == 0) return;
    esp_timer_start_once(s_idle_timer, (uint64_t)ms * 1000ULL);
}

static void idle_timer_fired(void *arg)
{
    ESP_LOGI(TAG, "idle timer expired — sleeping");
    state_machine_set_local(VIEWPORT_STATE_ASLEEP);
}

esp_err_t state_machine_init(void)
{
    esp_timer_create_args_t args = {
        .callback = &idle_timer_fired,
        .name     = "viewport_idle",
    };
    return esp_timer_create(&args, &s_idle_timer);
}

esp_err_t state_machine_set(viewport_run_state_t target)
{
    if (target != VIEWPORT_STATE_AWAKE && target != VIEWPORT_STATE_ASLEEP)
        return ESP_ERR_INVALID_ARG;

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    if (st->state == target) {
        viewport_state_unlock();
        return ESP_OK;  // idempotent no-op
    }
    st->state = target;
    bool configured = st->configured;
    uint32_t idle_ms = st->idle_timeout_ms;
    viewport_state_unlock();

    if (target == VIEWPORT_STATE_AWAKE) {
        if (display_is_up()) {
            // Paint the placeholder BEFORE turning the backlight on, then
            // wait ~33 ms (two 60 Hz refresh cycles) so the DSI engine
            // actually pushes the new framebuffer to the panel before the
            // backlight comes up. Without the delay, esp_lcd_panel_draw_bitmap
            // returns before the panel has been refreshed and the user sees
            // a flash of the previous /frame's contents.
            if (configured) local_screens_show_loading();
            else            local_screens_show_info();
            vTaskDelay(pdMS_TO_TICKS(33));
            display_wake();
        }
        arm_idle_timer(idle_ms);
        ESP_LOGI(TAG, "AWAKE (%s)", configured ? "configured" : "no scrypted URL");
    } else {
        arm_idle_timer(0);
        if (display_is_up()) display_sleep();
        ESP_LOGI(TAG, "ASLEEP");
    }
    return ESP_OK;
}

void state_machine_frame_painted(void)
{
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    bool awake = (st->state == VIEWPORT_STATE_AWAKE);
    uint32_t idle_ms = st->idle_timeout_ms;
    viewport_state_unlock();
    if (awake) arm_idle_timer(idle_ms);
}

void state_machine_set_local(viewport_run_state_t target)
{
    esp_err_t err = state_machine_set(target);
    if (err != ESP_OK) return;
    // Only POST when there's a Scrypted to talk to. Without a scrypted URL,
    // tap toggles change the local display state without notifying anyone.
    viewport_state_lock();
    bool configured = viewport_state_get()->configured;
    viewport_state_unlock();
    if (configured) state_client_post(target);
}

viewport_run_state_t state_machine_current(void)
{
    viewport_state_lock();
    viewport_run_state_t s = viewport_state_get()->state;
    viewport_state_unlock();
    return s;
}
