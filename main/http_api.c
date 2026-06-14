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

#include "net_eth.h"
#include "viewport_state.h"

static const char *TAG = "http_api";

static const char *state_name(viewport_run_state_t s)
{
    switch (s) {
    case VIEWPORT_STATE_AWAKE:  return "awake";
    case VIEWPORT_STATE_ASLEEP: return "asleep";
    default:                    return "unconfigured";
    }
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    uint64_t now_us  = (uint64_t)esp_timer_get_time();
    uint64_t up_ms   = (now_us - st->boot_us) / 1000;
    int64_t last_age_ms = (st->last_frame_us < 0)
                              ? -1
                              : (int64_t)((now_us - (uint64_t)st->last_frame_us) / 1000);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "name",
        st->viewport_name[0] ? cJSON_CreateString(st->viewport_name)
                             : cJSON_CreateNull());
    cJSON_AddStringToObject(root, "version", VIEWPORT_VERSION);
    cJSON_AddBoolToObject(root, "configured", st->configured);
    cJSON_AddStringToObject(root, "state", state_name(st->state));
    cJSON_AddNumberToObject(root, "uptime_ms", (double)up_ms);
    cJSON_AddItemToObject(root, "last_frame_ms_ago",
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

static const httpd_uri_t s_state_get = {
    .uri      = "/state",
    .method   = HTTP_GET,
    .handler  = state_get_handler,
    .user_ctx = NULL,
};

esp_err_t http_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = 80;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_state_get),
                       TAG, "register /state");

    ESP_LOGI(TAG, "http server listening on :80 (GET /state)");
    return ESP_OK;
}
