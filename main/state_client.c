#include "state_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "viewport_state.h"

static const char *TAG = "state_client";

#define WORKER_STACK     4096
#define WORKER_PRIO      5
#define HTTP_TIMEOUT_MS  1000
#define USER_AGENT       "ScryptedViewport/" VIEWPORT_VERSION

// Depth-1 queue holding the next desired state. xQueueOverwrite() gives us
// the replace-on-full semantics from the spec.
static QueueHandle_t s_queue;

static void perform_post(viewport_run_state_t state)
{
    char url[320];
    char body[160];
    char viewport_name[64];

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    if (st->scrypted_url[0] == '\0') {
        viewport_state_unlock();
        return;  // not configured — silently drop
    }
    snprintf(url, sizeof(url), "%s/state", st->scrypted_url);
    strncpy(viewport_name, st->viewport_name, sizeof(viewport_name) - 1);
    viewport_name[sizeof(viewport_name) - 1] = '\0';
    viewport_state_unlock();

    const char *state_str = (state == VIEWPORT_STATE_AWAKE) ? "wake" : "sleep";
    snprintf(body, sizeof(body),
             "{\"viewport\":\"%s\",\"state\":\"%s\"}",
             viewport_name, state_str);

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        viewport_state_lock();
        viewport_state_get()->state_post_failures++;
        viewport_state_unlock();
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent",   USER_AGENT);
    esp_http_client_set_header(client, "Connection",   "close");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST %s failed: err=%s status=%d", url, esp_err_to_name(err), status);
        viewport_state_lock();
        viewport_state_get()->state_post_failures++;
        viewport_state_unlock();
    } else {
        ESP_LOGI(TAG, "POST %s {state:%s} -> %d", url, state_str, status);
    }
    esp_http_client_cleanup(client);
}

static void worker(void *arg)
{
    for (;;) {
        viewport_run_state_t pending;
        if (xQueueReceive(s_queue, &pending, portMAX_DELAY) == pdTRUE) {
            perform_post(pending);
        }
    }
}

esp_err_t state_client_init(void)
{
    s_queue = xQueueCreate(1, sizeof(viewport_run_state_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(worker, "state_client", WORKER_STACK,
                                NULL, WORKER_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

void state_client_post(viewport_run_state_t state)
{
    if (state != VIEWPORT_STATE_AWAKE && state != VIEWPORT_STATE_ASLEEP) return;
    // xQueueOverwrite is depth-1 replace-on-full. The in-flight POST in the
    // worker is unaffected — it finishes naturally, then drains the new entry.
    xQueueOverwrite(s_queue, &state);
}
