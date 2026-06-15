#include "nvs_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "viewport_state.h"

static const char *TAG = "nvs_config";
static const char *NS  = "viewport";

static const char *K_VIEWPORT = "viewport";
static const char *K_SCRYPTED = "scrypted";
static const char *K_IDLE_MS  = "idle_ms";
static const char *K_ORIENT   = "orient";    // 0 = portrait, 1 = landscape
static const char *K_BRIGHT   = "bright";

esp_err_t nvs_config_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config — first boot");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    size_t len = sizeof(st->viewport_name);
    err = nvs_get_str(h, K_VIEWPORT, st->viewport_name, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto done;

    len = sizeof(st->scrypted_url);
    err = nvs_get_str(h, K_SCRYPTED, st->scrypted_url, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto done;

    uint32_t u32 = 0;
    err = nvs_get_u32(h, K_IDLE_MS, &u32);
    if (err == ESP_OK) st->idle_timeout_ms = u32;
    else if (err != ESP_ERR_NVS_NOT_FOUND) goto done;

    uint8_t u8 = 0;
    err = nvs_get_u8(h, K_ORIENT, &u8);
    if (err == ESP_OK) {
        st->orientation = (u8 == 1) ? VIEWPORT_ORIENTATION_LANDSCAPE
                                    : VIEWPORT_ORIENTATION_PORTRAIT;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        goto done;
    }

    err = nvs_get_u8(h, K_BRIGHT, &u8);
    if (err == ESP_OK) st->brightness = u8;
    else if (err != ESP_ERR_NVS_NOT_FOUND) goto done;

    err = ESP_OK;

    // "Configured" = a scrypted URL has been registered. viewport_name
    // always has a value (MAC-derived default seeded in viewport_state_init).
    st->configured = (st->scrypted_url[0] != '\0');
    ESP_LOGI(TAG, "loaded config: viewport=%s scrypted=%s "
                  "idle_ms=%u orient=%s bright=%u (%s)",
             st->viewport_name,
             st->scrypted_url[0] ? st->scrypted_url : "(none)",
             (unsigned)st->idle_timeout_ms,
             st->orientation == VIEWPORT_ORIENTATION_LANDSCAPE
                 ? "landscape" : "portrait",
             st->brightness,
             st->configured ? "configured" : "no scrypted URL");

done:
    viewport_state_unlock();
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    if ((err = nvs_set_str(h, K_VIEWPORT, st->viewport_name)) != ESP_OK) goto done;
    if ((err = nvs_set_str(h, K_SCRYPTED, st->scrypted_url))  != ESP_OK) goto done;
    if ((err = nvs_set_u32(h, K_IDLE_MS,  st->idle_timeout_ms)) != ESP_OK) goto done;
    if ((err = nvs_set_u8 (h, K_ORIENT,
                          (st->orientation == VIEWPORT_ORIENTATION_LANDSCAPE)
                              ? 1 : 0)) != ESP_OK) goto done;
    if ((err = nvs_set_u8 (h, K_BRIGHT,   st->brightness))     != ESP_OK) goto done;

    err = nvs_commit(h);

done:
    viewport_state_unlock();
    nvs_close(h);
    return err;
}

