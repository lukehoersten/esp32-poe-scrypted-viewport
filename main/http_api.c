#include "http_api.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <stdatomic.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "display.h"
#include "jpeg_decoder.h"
#include "mdns_service.h"
#include "net_eth.h"
#include "nvs_config.h"
#include "ota.h"
#include "stream_server.h"
#include "state_machine.h"
#include "viewport_state.h"

static const char *TAG = "http_api";

#define MAX_BODY_BYTES   2048
#define MIN_IDLE_TIMEOUT 5000

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
    cJSON_AddStringToObject(root, "name", st->viewport_name);
    cJSON_AddStringToObject(root, "mac",  st->mac_str);
    cJSON_AddStringToObject(root, "version", VIEWPORT_VERSION);
    cJSON_AddStringToObject(root, "ota_state", ota_running_state_str());
    cJSON_AddBoolToObject  (root, "configured", st->configured);
    cJSON_AddStringToObject(root, "state",
        (st->state == VIEWPORT_STATE_AWAKE) ? "awake" : "asleep");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)up_ms);
    cJSON_AddItemToObject  (root, "last_frame_ms_ago",
        last_age_ms < 0 ? cJSON_CreateNull()
                        : cJSON_CreateNumber((double)last_age_ms));
    cJSON_AddNumberToObject(root, "frames_received", (double)st->frames_received);
    cJSON_AddNumberToObject(root, "decode_errors", (double)st->decode_errors);
    cJSON_AddNumberToObject(root, "state_post_failures", (double)st->state_post_failures);
    cJSON_AddStringToObject(root, "resolution", viewport_state_resolution_str());
    // Panel-native dimensions are stable per-board (Scrypted uses them as
    // the ffmpeg scale target; orientation drives whether to transpose
    // before scaling). Separate from "resolution" which is the effective
    // dimensions after the orientation rotation.
    cJSON_AddNumberToObject(root, "panel_width",  (double)VIEWPORT_PANEL_WIDTH);
    cJSON_AddNumberToObject(root, "panel_height", (double)VIEWPORT_PANEL_HEIGHT);
    cJSON_AddStringToObject(root, "ip", net_eth_get_ip_str());
    cJSON_AddNumberToObject(root, "free_heap",
        (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    viewport_state_unlock();

    // Most recent closed window of live-stream stats. Populated every
    // 30 painted stream frames; zero before the first window rolls.
    // last_paint_event_us_low is the low 32 bits of the Scrypted
    // host monotonic µs at camera-event arrival, passed verbatim
    // through the stream header. Script computes glass-to-glass as
    //   (now_us_low - last_paint_event_us_low) with wrap.
    stream_server_stats_t stream;
    stream_server_snapshot_stats(&stream);
    cJSON *s = cJSON_CreateObject();
    cJSON_AddNumberToObject(s, "frames",        (double)stream.frames);
    cJSON_AddNumberToObject(s, "bytes",         (double)stream.bytes);
    cJSON_AddNumberToObject(s, "window_us",     (double)stream.window_us);
    cJSON_AddNumberToObject(s, "window_end_us", (double)stream.window_end_us);
    cJSON_AddNumberToObject(s, "recv_min_us",   (double)stream.recv_min_us);
    cJSON_AddNumberToObject(s, "recv_avg_us",   (double)stream.recv_avg_us);
    cJSON_AddNumberToObject(s, "recv_max_us",   (double)stream.recv_max_us);
    cJSON_AddNumberToObject(s, "dec_min_us",    (double)stream.dec_min_us);
    cJSON_AddNumberToObject(s, "dec_avg_us",    (double)stream.dec_avg_us);
    cJSON_AddNumberToObject(s, "dec_max_us",    (double)stream.dec_max_us);
    cJSON_AddNumberToObject(s, "paint_min_us",  (double)stream.pnt_min_us);
    cJSON_AddNumberToObject(s, "paint_avg_us",  (double)stream.pnt_avg_us);
    cJSON_AddNumberToObject(s, "paint_max_us",  (double)stream.pnt_max_us);
    cJSON_AddNumberToObject(s, "idle_min_us",   (double)stream.idle_min_us);
    cJSON_AddNumberToObject(s, "idle_avg_us",   (double)stream.idle_avg_us);
    cJSON_AddNumberToObject(s, "idle_max_us",   (double)stream.idle_max_us);
    // Recv-throughput diagnostics. See stream_server.h for semantics.
    cJSON_AddNumberToObject(s, "queued_min",      (double)stream.queued_min);
    cJSON_AddNumberToObject(s, "queued_avg",      (double)stream.queued_avg);
    cJSON_AddNumberToObject(s, "queued_max",      (double)stream.queued_max);
    cJSON_AddNumberToObject(s, "recv_calls_min",  (double)stream.recv_calls_min);
    cJSON_AddNumberToObject(s, "recv_calls_avg",  (double)stream.recv_calls_avg);
    cJSON_AddNumberToObject(s, "recv_calls_max",  (double)stream.recv_calls_max);
    cJSON_AddNumberToObject(s, "recv_chunk_min",  (double)stream.recv_chunk_min);
    cJSON_AddNumberToObject(s, "recv_chunk_avg",  (double)stream.recv_chunk_avg);
    cJSON_AddNumberToObject(s, "recv_chunk_max",  (double)stream.recv_chunk_max);
    cJSON_AddNumberToObject(s, "so_rcvbuf",       (double)stream.so_rcvbuf);
    cJSON_AddNumberToObject(s, "last_paint_event_us_low",
                            (double)stream.last_paint_event_us_low);
    cJSON_AddItemToObject(root, "stream", s);

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
    cJSON_AddStringToObject(root, "viewport", st->viewport_name);
    cJSON_AddItemToObject(root, "scrypted",
        st->scrypted_url[0]  ? cJSON_CreateString(st->scrypted_url)
                             : cJSON_CreateNull());
    cJSON_AddNumberToObject(root, "idle_timeout_ms", (double)st->idle_timeout_ms);
    cJSON_AddStringToObject(root, "orientation",
        (st->orientation == VIEWPORT_ORIENTATION_LANDSCAPE) ? "landscape" : "portrait");
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

    // "Configured" means a scrypted URL has been registered — that's
    // what gates outbound POSTs and the loading-vs-info screen content
    // choice. The viewport_name always has a value (MAC-derived default
    // until POST /config overrides), so it doesn't factor in here.
    st->configured = (st->scrypted_url[0] != '\0');

    viewport_state_unlock();

    esp_err_t save_err = nvs_config_save();
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_config_save failed: %s", esp_err_to_name(save_err));
        // Don't fail the request — config is applied in RAM and will reapply
        // on next save. Caller can re-POST.
    }

    if (brightness_changed && display_is_up()) {
        display_set_brightness(bright);
    }
    if (name_or_orient_changed) {
        mdns_service_refresh();
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ============================================================================
// POST /state
// ============================================================================
static esp_err_t state_post_handler(httpd_req_t *req)
{
    char buf[64];
    if (read_body(req, buf, sizeof(buf)) != ESP_OK)
        return respond_400(req, "missing or oversized body");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return respond_400(req, "invalid JSON");

    cJSON *j = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(j)) {
        cJSON_Delete(root);
        return respond_400(req, "state must be 'wake' or 'sleep'");
    }

    viewport_run_state_t target;
    if (strcmp(j->valuestring, "wake") == 0) {
        target = VIEWPORT_STATE_AWAKE;
    } else if (strcmp(j->valuestring, "sleep") == 0) {
        target = VIEWPORT_STATE_ASLEEP;
    } else {
        cJSON_Delete(root);
        return respond_400(req, "state must be 'wake' or 'sleep'");
    }
    cJSON_Delete(root);

    esp_err_t err = state_machine_set(target);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ============================================================================
// POST /frame
// ============================================================================
static esp_err_t respond_status(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, body, body ? HTTPD_RESP_USE_STRLEN : 0);
}

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    // Content-Type must be image/jpeg.
    char ct[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "image/jpeg", 10) != 0) {
        return respond_status(req, "400 Bad Request", "Content-Type must be image/jpeg");
    }

    if (req->content_len == 0) {
        return respond_status(req, "400 Bad Request", "empty body");
    }
    if (req->content_len > JPEG_DECODER_MAX_INPUT_BYTES) {
        return respond_status(req, "413 Payload Too Large", "JPEG > 1 MB");
    }

    if (!display_is_up()) {
        return respond_status(req, "500 Internal Server Error", "display not initialized");
    }

    // /frame requires AWAKE. Asleep → 409.
    if (state_machine_current() != VIEWPORT_STATE_AWAKE) {
        return respond_status(req, "409 Conflict",
            "device asleep — POST /state {\"state\":\"wake\"} first");
    }

    int64_t t_entry = esp_timer_get_time();

    // Fence /frame snapshot against an in-flight stream decode. The
    // live stream task owns the decoder via stream_server; a snapshot
    // landing mid-decode gets 503 here and Scrypted silently drops it
    // (the snapshot is a fast-path nicety; the stream is the real
    // data plane).
    if (!jpeg_decoder_try_lock(0)) {
        return respond_status(req, "503 Service Unavailable", "frame in flight");
    }

    int64_t t_lock = esp_timer_get_time();

    esp_err_t result = ESP_OK;
    uint8_t *in = jpeg_decoder_input_buffer();
    size_t got = 0;
    int64_t t_first_byte = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, (char *)(in + got), req->content_len - got);
        if (n <= 0) { result = ESP_FAIL; break; }
        if (got == 0) t_first_byte = esp_timer_get_time();
        got += n;
    }

    if (result != ESP_OK) {
        jpeg_decoder_unlock();
        return respond_status(req, "400 Bad Request", "body read failed");
    }

    int64_t t_recv = esp_timer_get_time();

    // Decode straight into the panel's back framebuffer — no scratch
    // buffer, no later memcpy. The flip below is a cache writeback +
    // index swap inside the IDF DPI driver.
    size_t back_size = 0;
    void *back = display_back_buffer(&back_size);
    uint16_t w = 0, h = 0;
    esp_err_t dec_err = jpeg_decoder_decode(got, back, back_size, &w, &h);
    int64_t t_decode = esp_timer_get_time();
    if (dec_err != ESP_OK) {
        viewport_state_lock();
        viewport_state_get()->decode_errors++;
        viewport_state_unlock();
        jpeg_decoder_unlock();
        return respond_status(req, "400 Bad Request", "JPEG decode failed");
    }

    // /frame always expects the panel-native 800x480 BGR888 layout.
    // Scrypted does the rotation + scale (snapshot: sharp / mediaManager
    // / ffmpeg cascade; stream: ffmpeg -vf transpose+scale). Orientation
    // is informational for /state reporting only — firmware never
    // rotates pixels.
    if (w != VIEWPORT_PANEL_WIDTH || h != VIEWPORT_PANEL_HEIGHT) {
        viewport_state_lock();
        viewport_state_get()->decode_errors++;
        viewport_state_unlock();
        jpeg_decoder_unlock();
        char msg[80];
        snprintf(msg, sizeof(msg), "expected %ux%u, got %ux%u",
                 VIEWPORT_PANEL_WIDTH, VIEWPORT_PANEL_HEIGHT, w, h);
        return respond_status(req, "400 Bad Request", msg);
    }

    esp_err_t paint_err = display_flip_back_buffer();
    int64_t t_paint = esp_timer_get_time();
    if (paint_err != ESP_OK) {
        jpeg_decoder_unlock();
        return respond_status(req, "500 Internal Server Error", "display paint failed");
    }

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    st->frames_received++;
    st->last_frame_us = esp_timer_get_time();
    viewport_state_unlock();

    jpeg_decoder_unlock();

    // Snapshot stage timings, one line per /frame POST. /frame fires
    // at most once per wake event, so logging every snapshot (not
    // every 10) is the right cadence.
    ESP_LOGI(TAG,
        "snapshot: lock=%lldus ttfb=%lldus body=%lldus dec=%lldus paint=%lldus total=%lldms (jpeg=%uKB)",
        (long long)(t_lock       - t_entry),
        (long long)(t_first_byte - t_lock),
        (long long)(t_recv       - t_first_byte),
        (long long)(t_decode     - t_recv),
        (long long)(t_paint      - t_decode),
        (long long)((t_paint - t_entry) / 1000),
        (unsigned)(got / 1024));

    state_machine_frame_painted();  // reset idle timer

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ============================================================================
// POST /firmware  — push raw app .bin, flip OTA slot, reboot
// ============================================================================
static atomic_flag s_ota_in_progress = ATOMIC_FLAG_INIT;

static void reboot_cb(void *arg)
{
    ESP_LOGW(TAG, "rebooting into new image");
    esp_restart();
}

static esp_err_t firmware_post_handler(httpd_req_t *req)
{
    if (atomic_flag_test_and_set(&s_ota_in_progress)) {
        return respond_status(req, "409 Conflict", "OTA already in progress");
    }

    esp_err_t result   = ESP_OK;
    const char *err_msg = NULL;
    const char *err_status = "500 Internal Server Error";
    esp_ota_handle_t handle = 0;
    bool handle_open = false;

    if (req->content_len <= 0) {
        err_status = "400 Bad Request";
        err_msg = "missing Content-Length";
        result = ESP_FAIL;
        goto done;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        err_msg = "no OTA target partition";
        result = ESP_FAIL;
        goto done;
    }
    if ((size_t)req->content_len > target->size) {
        err_status = "413 Payload Too Large";
        err_msg = "image exceeds partition size";
        result = ESP_FAIL;
        goto done;
    }

    ESP_LOGI(TAG, "ota: begin target=%s size=%d running=%s",
             target->label, req->content_len,
             running ? running->label : "?");

    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        err_msg = esp_err_to_name(err);
        result = ESP_FAIL;
        goto done;
    }
    handle_open = true;

    uint8_t buf[4096];
    int remaining = req->content_len;
    int last_logged_pct = -1;
    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            err_status = "400 Bad Request";
            err_msg = "body read failed";
            result = ESP_FAIL;
            goto done;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            err_msg = esp_err_to_name(err);
            result = ESP_FAIL;
            goto done;
        }
        remaining -= n;
        int pct = (int)(100LL * (req->content_len - remaining) / req->content_len);
        if (pct / 10 != last_logged_pct / 10) {
            ESP_LOGI(TAG, "ota: %d%% (%d/%d bytes)",
                     pct, req->content_len - remaining, req->content_len);
            last_logged_pct = pct;
        }
    }

    err = esp_ota_end(handle);
    handle_open = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        err_status = (err == ESP_ERR_OTA_VALIDATE_FAILED)
            ? "400 Bad Request" : "500 Internal Server Error";
        err_msg = esp_err_to_name(err);
        result = ESP_FAIL;
        goto done;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        err_msg = esp_err_to_name(err);
        result = ESP_FAIL;
        goto done;
    }

    ESP_LOGI(TAG, "ota: success — booting %s on reboot", target->label);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    const esp_app_desc_t *running_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "previous",
        running_desc ? running_desc->version : "?");
    esp_app_desc_t next_desc;
    if (esp_ota_get_partition_description(target, &next_desc) == ESP_OK) {
        cJSON_AddStringToObject(root, "next", next_desc.version);
    } else {
        cJSON_AddStringToObject(root, "next", "?");
    }
    cJSON_AddStringToObject(root, "slot", target->label);
    cJSON_AddNumberToObject(root, "reboot_in_ms", 500);
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    cJSON_Delete(root);

    // Reboot after the response has time to flush. One-shot esp_timer so
    // this handler can return cleanly and httpd can finish the socket.
    const esp_timer_create_args_t args = {
        .callback = &reboot_cb,
        .name     = "ota_reboot",
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 500 * 1000);
    } else {
        esp_restart();
    }
    // Leave s_ota_in_progress set — we're rebooting.
    return ESP_OK;

done:
    if (handle_open) esp_ota_abort(handle);
    atomic_flag_clear(&s_ota_in_progress);
    if (result != ESP_OK) {
        return respond_status(req, err_status, err_msg ? err_msg : "OTA failed");
    }
    return ESP_OK;
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
static const httpd_uri_t s_state_post = {
    .uri = "/state",  .method = HTTP_POST, .handler = state_post_handler,
};
static const httpd_uri_t s_frame_post = {
    .uri = "/frame",  .method = HTTP_POST, .handler = frame_post_handler,
};
static const httpd_uri_t s_firmware_post = {
    .uri = "/firmware", .method = HTTP_POST, .handler = firmware_post_handler,
};

esp_err_t http_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 8192;  // POST /config alone has ~2.4 KiB of stack locals
    // /frame is snapshot-only and the live stream owns its own TCP
    // socket on port 81. Two HTTP sockets cover: one snapshot POST
    // concurrent with one /state or /config request.
    cfg.max_open_sockets = 2;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_state_get),
                       TAG, "register GET /state");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_config_get),
                       TAG, "register GET /config");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_config_post),
                       TAG, "register POST /config");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_state_post),
                       TAG, "register POST /state");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_frame_post),
                       TAG, "register POST /frame");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_firmware_post),
                       TAG, "register POST /firmware");

    ESP_LOGI(TAG, "http server listening on :80 "
                  "(GET/POST /state, GET/POST /config, POST /frame, POST /firmware)");
    return ESP_OK;
}
