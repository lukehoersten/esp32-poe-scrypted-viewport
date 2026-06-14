#include "http_api.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "mdns_service.h"
#include "net_eth.h"
#include "nvs_config.h"
#include "viewport_state.h"

static const char *TAG = "http_api";

#define MAX_BODY_BYTES   2048
#define MIN_IDLE_TIMEOUT 5000

static const char *state_name(viewport_run_state_t s)
{
    switch (s) {
    case VIEWPORT_STATE_AWAKE:  return "awake";
    case VIEWPORT_STATE_ASLEEP: return "asleep";
    default:                    return "unconfigured";
    }
}

static const char *orientation_name(viewport_orientation_t o)
{
    return (o == VIEWPORT_ORIENTATION_LANDSCAPE) ? "landscape" : "portrait";
}

// ============================================================================
// GET /state
// ============================================================================
static esp_err_t state_get_handler(httpd_req_t *req)
{
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    uint64_t now_us  = (uint64_t)esp_timer_get_time();
    uint64_t up_ms   = (now_us - st->boot_us) / 1000;
    int64_t  last_age_ms = (st->last_frame_us < 0)
                              ? -1
                              : (int64_t)((now_us - (uint64_t)st->last_frame_us) / 1000);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "name",
        st->viewport_name[0] ? cJSON_CreateString(st->viewport_name)
                             : cJSON_CreateNull());
    cJSON_AddStringToObject(root, "version", VIEWPORT_VERSION);
    cJSON_AddBoolToObject  (root, "configured", st->configured);
    cJSON_AddStringToObject(root, "state", state_name(st->state));
    cJSON_AddNumberToObject(root, "uptime_ms", (double)up_ms);
    cJSON_AddItemToObject  (root, "last_frame_ms_ago",
        last_age_ms < 0 ? cJSON_CreateNull()
                        : cJSON_CreateNumber((double)last_age_ms));
    cJSON_AddNumberToObject(root, "frames_received", (double)st->frames_received);
    cJSON_AddNumberToObject(root, "decode_errors", (double)st->decode_errors);
    cJSON_AddNumberToObject(root, "state_post_failures", (double)st->state_post_failures);
    cJSON_AddStringToObject(root, "resolution", viewport_state_resolution_str());
    cJSON_AddStringToObject(root, "ip", net_eth_get_ip_str());
    cJSON_AddNumberToObject(root, "free_heap",
        (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    viewport_state_unlock();

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

// ============================================================================
// GET /config
// ============================================================================
static esp_err_t config_get_handler(httpd_req_t *req)
{
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "viewport",
        st->viewport_name[0] ? cJSON_CreateString(st->viewport_name)
                             : cJSON_CreateNull());
    cJSON_AddItemToObject(root, "scrypted",
        st->scrypted_url[0]  ? cJSON_CreateString(st->scrypted_url)
                             : cJSON_CreateNull());
    cJSON_AddNumberToObject(root, "idle_timeout_ms", (double)st->idle_timeout_ms);
    cJSON_AddStringToObject(root, "orientation", orientation_name(st->orientation));
    cJSON_AddNumberToObject(root, "brightness", (double)st->brightness);

    viewport_state_unlock();

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

// ============================================================================
// POST /config — partial-update, atomic, validated
// ============================================================================
static esp_err_t respond_400(httpd_req_t *req, const char *reason)
{
    ESP_LOGW(TAG, "/config 400: %s", reason);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) return ESP_FAIL;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char body[MAX_BODY_BYTES];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return respond_400(req, "missing or oversized body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return respond_400(req, "invalid JSON");

    // Stage validated values; commit atomically at the end. Each "have_*"
    // flag records whether the field was present in the request.
    bool have_vp = false, have_sc = false, have_idle = false;
    bool have_orient = false, have_bright = false;

    char    vp[64]      = {0};
    char    sc[256]     = {0};
    uint32_t idle_ms    = 0;
    viewport_orientation_t orient = VIEWPORT_ORIENTATION_PORTRAIT;
    uint8_t bright      = 80;

    cJSON *j;

    if ((j = cJSON_GetObjectItemCaseSensitive(root, "viewport"))) {
        if (!cJSON_IsString(j) || j->valuestring[0] == '\0' ||
            strlen(j->valuestring) >= sizeof(vp)) {
            cJSON_Delete(root);
            return respond_400(req, "viewport must be a non-empty string < 64 chars");
        }
        strncpy(vp, j->valuestring, sizeof(vp) - 1);
        have_vp = true;
    }

    if ((j = cJSON_GetObjectItemCaseSensitive(root, "scrypted"))) {
        if (!cJSON_IsString(j) || strncmp(j->valuestring, "http://", 7) != 0 ||
            strlen(j->valuestring) >= sizeof(sc)) {
            cJSON_Delete(root);
            return respond_400(req, "scrypted must be http://... and < 256 chars");
        }
        strncpy(sc, j->valuestring, sizeof(sc) - 1);
        have_sc = true;
    }

    if ((j = cJSON_GetObjectItemCaseSensitive(root, "idle_timeout_ms"))) {
        if (!cJSON_IsNumber(j) || j->valuedouble < 0 ||
            j->valuedouble > 0xFFFFFFFFULL) {
            cJSON_Delete(root);
            return respond_400(req, "idle_timeout_ms must be a u32");
        }
        idle_ms = (uint32_t)j->valuedouble;
        if (idle_ms != 0 && idle_ms < MIN_IDLE_TIMEOUT) {
            cJSON_Delete(root);
            return respond_400(req, "idle_timeout_ms must be 0 or >= 5000");
        }
        have_idle = true;
    }

    if ((j = cJSON_GetObjectItemCaseSensitive(root, "orientation"))) {
        if (!cJSON_IsString(j)) {
            cJSON_Delete(root);
            return respond_400(req, "orientation must be a string");
        }
        if (strcmp(j->valuestring, "portrait") == 0) {
            orient = VIEWPORT_ORIENTATION_PORTRAIT;
        } else if (strcmp(j->valuestring, "landscape") == 0) {
            orient = VIEWPORT_ORIENTATION_LANDSCAPE;
        } else {
            cJSON_Delete(root);
            return respond_400(req, "orientation must be portrait or landscape");
        }
        have_orient = true;
    }

    if ((j = cJSON_GetObjectItemCaseSensitive(root, "brightness"))) {
        if (!cJSON_IsNumber(j) || j->valuedouble < 0 || j->valuedouble > 100) {
            cJSON_Delete(root);
            return respond_400(req, "brightness must be 0..100");
        }
        bright = (uint8_t)j->valuedouble;
        have_bright = true;
    }

    cJSON_Delete(root);

    // Apply atomically.
    bool brightness_changed   = false;
    bool name_or_orient_changed = false;

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    if (have_vp) {
        if (strcmp(st->viewport_name, vp) != 0) name_or_orient_changed = true;
        strncpy(st->viewport_name, vp, sizeof(st->viewport_name) - 1);
    }
    if (have_sc) strncpy(st->scrypted_url, sc, sizeof(st->scrypted_url) - 1);
    if (have_idle) st->idle_timeout_ms = idle_ms;
    if (have_orient) {
        if (st->orientation != orient) name_or_orient_changed = true;
        st->orientation = orient;
    }
    if (have_bright) {
        if (st->brightness != bright) brightness_changed = true;
        st->brightness = bright;
    }

    // A configured device has both a viewport name and a scrypted URL.
    if (st->viewport_name[0] && st->scrypted_url[0] && !st->configured) {
        st->configured = true;
        if (st->state == VIEWPORT_STATE_UNCONFIGURED) st->state = VIEWPORT_STATE_ASLEEP;
    }

    viewport_state_unlock();

    esp_err_t save_err = nvs_config_save();
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_config_save failed: %s", esp_err_to_name(save_err));
        // Don't fail the request — config is applied in RAM and will reapply
        // on next save. Caller can re-POST.
    }

    if (brightness_changed && display_is_up()) {
        viewport_state_lock();
        uint8_t b = viewport_state_get()->brightness;
        viewport_state_unlock();
        display_set_brightness(b);
    }
    if (name_or_orient_changed) {
        mdns_service_refresh();
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ============================================================================
// Route table + start
// ============================================================================
static const httpd_uri_t s_state_get = {
    .uri = "/state", .method = HTTP_GET,  .handler = state_get_handler,
};
static const httpd_uri_t s_config_get = {
    .uri = "/config", .method = HTTP_GET,  .handler = config_get_handler,
};
static const httpd_uri_t s_config_post = {
    .uri = "/config", .method = HTTP_POST, .handler = config_post_handler,
};

esp_err_t http_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_state_get),
                       TAG, "register GET /state");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_config_get),
                       TAG, "register GET /config");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_config_post),
                       TAG, "register POST /config");

    ESP_LOGI(TAG, "http server listening on :80 (GET /state, GET/POST /config)");
    return ESP_OK;
}
