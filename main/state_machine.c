#include "state_machine.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
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

    if (!st->configured) {
        viewport_state_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (st->state == target) {
        viewport_state_unlock();
        return ESP_OK;  // idempotent no-op
    }
    st->state = target;
    viewport_state_unlock();

    if (target == VIEWPORT_STATE_AWAKE) {
        if (display_is_up()) display_wake();
        arm_idle_timer_unlocked();
        ESP_LOGI(TAG, "AWAKE");
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
    // For idempotent no-op (already in target), state_machine_set returns OK
    // without changing state; we still call state_client to keep Scrypted in
    // sync if it's drifted. State POSTs are idempotent on both ends.
    esp_err_t err = state_machine_set(target);
    if (err != ESP_OK) return;  // unconfigured / invalid
    state_client_post(target);
}

viewport_run_state_t state_machine_current(void)
{
    viewport_state_lock();
    viewport_run_state_t s = viewport_state_get()->state;
    viewport_state_unlock();
    return s;
}
