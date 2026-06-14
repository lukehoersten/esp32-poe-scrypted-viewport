#include "viewport_state.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static viewport_state_t s_state;
static SemaphoreHandle_t s_mutex;

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
