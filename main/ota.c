#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"

static const char *TAG = "ota";

#define OTA_HEALTHY_DELAY_US (30ULL * 1000 * 1000)

static void healthy_cb(void *arg)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK
        && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "image marked valid (rollback cancelled)");
        } else {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
        }
    }
}

void ota_arm_healthy_timer(void)
{
    const esp_timer_create_args_t args = {
        .callback = &healthy_cb,
        .name     = "ota_healthy",
    };
    // The handle is deliberately never deleted: one ~50-byte allocation
    // per boot, and deleting from inside the callback would race the
    // dispatcher.
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) != ESP_OK) return;
    esp_timer_start_once(t, OTA_HEALTHY_DELAY_US);
}

const char *ota_running_state_str(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return "unknown";
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) return "unknown";
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
        default:                         return "unknown";
    }
}
