#include "state_machine.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "local_screens.h"
#include "state_client.h"

static const char *TAG = "state";

static esp_timer_handle_t s_idle_timer;

static void arm_idle_timer_unlocked(void)
{
    viewport_state_lock();
    uint32_t ms = viewport_state_get()->idle_timeout_ms;
    viewport_state_unlock();
    if (ms == 0) return;
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, (uint64_t)ms * 1000ULL);
}

static void disarm_idle_timer(void)
{
    esp_timer_stop(s_idle_timer);
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
    viewport_state_unlock();

    if (target == VIEWPORT_STATE_AWAKE) {
        if (display_is_up()) {
            display_wake();
            // Content choice: configured devices show "Loading…" until
            // Scrypted pushes a /frame; unconfigured devices show the
            // info screen since there's no Scrypted to push anything.
            if (configured) local_screens_show_loading();
            else            local_screens_show_info();
        }
        arm_idle_timer_unlocked();
        ESP_LOGI(TAG, "AWAKE (%s)", configured ? "configured" : "unconfigured");
    } else {
        disarm_idle_timer();
        if (display_is_up()) display_sleep();
        ESP_LOGI(TAG, "ASLEEP");
    }
    return ESP_OK;
}

void state_machine_frame_painted(void)
{
    viewport_state_lock();
    bool awake = (viewport_state_get()->state == VIEWPORT_STATE_AWAKE);
    viewport_state_unlock();
    if (awake) arm_idle_timer_unlocked();
}

void state_machine_set_local(viewport_run_state_t target)
{
    esp_err_t err = state_machine_set(target);
    if (err != ESP_OK) return;
    // Only POST when there's a Scrypted to talk to. Unconfigured tap toggles
    // change the local display state without notifying anyone.
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
