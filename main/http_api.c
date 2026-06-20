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
#include "lwip/sockets.h"  // TCP_NODELAY setsockopt for /frame socket

#include "display.h"
#include "jpeg_decoder.h"
#include "mdns_service.h"
#include "net_eth.h"
#include "nvs_config.h"
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
// Defined alongside other /frame static state further down — forward
// referenced here because state_post_handler resets it on wake.
static uint32_t s_last_painted_seq;

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
        // Reset the pipelined-POST sequence comparator so the next
        // stream's first frame isn't rejected as stale just because
        // the previous stream's counter happened to be higher.
        s_last_painted_seq = 0;
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

// Tracks when the previous /frame response finished so we can log the
// idle gap until the next request enters. Large idle = Scrypted/network
// upstream is the bottleneck; small idle = we're saturating the link.
static int64_t  s_last_post_us;
// (s_last_painted_seq is forward-declared above state_post_handler;
// description there.)

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    // Nagle off on this socket. /frame is a single large POST followed
    // by a tiny (empty 204) response — the worst case for Nagle. The
    // last partial-MTU packet of the body, and the response packet,
    // both sit in the kernel send buffer up to 40ms waiting for an
    // ACK that the peer's also delaying-ACKing. setsockopt is cheap
    // and idempotent; if keep-alive is ever added the flag persists
    // for the connection's life.
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        int one = 1;
        (void)setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

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

    // Read the optional X-Frame-Seq header used to keep pipelined
    // POSTs in monotonic paint order. Missing or zero header = no
    // ordering enforcement (treat every frame as newer than the
    // previous one, same behaviour we had before pipelining).
    uint32_t seq = 0;
    char seq_str[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Frame-Seq", seq_str, sizeof(seq_str)) == ESP_OK) {
        seq = (uint32_t)strtoul(seq_str, NULL, 10);
    }

    // Single in-flight frame. Concurrent posts get 503 (spec).
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

    // Pipelined-POST stale-frame check. With the X-Frame-Seq header
    // set, drop anything we've already advanced past — we hold the
    // decoder lock, so re-checking here is race-free. Without the
    // header (seq==0) every frame paints, preserving pre-pipelining
    // behaviour.
    if (seq != 0 && seq <= s_last_painted_seq) {
        jpeg_decoder_unlock();
        // 200 OK with an empty body keeps the keep-alive socket
        // healthy and lets Scrypted's per-fetch wall-clock log
        // stay accurate; we just won't count this toward
        // frames_received.
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_hdr(req, "X-Frame-Drop", "stale-seq");
        return httpd_resp_send(req, NULL, 0);
    }

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

    // /frame always expects the panel-native 800x480 BGR888 layout —
    // Scrypted does the rotation + scale, the firmware just decodes and
    // paints. Orientation lives in viewport_state for /state reporting
    // and the Scrypted-side ffmpeg pipeline only.
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
    int64_t t_post  = 0;
    if (paint_err != ESP_OK) {
        jpeg_decoder_unlock();
        return respond_status(req, "500 Internal Server Error", "display paint failed");
    }

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    st->frames_received++;
    st->last_frame_us = esp_timer_get_time();
    uint64_t fr = st->frames_received;
    viewport_state_unlock();

    if (seq != 0) s_last_painted_seq = seq;

    jpeg_decoder_unlock();

    // Track the bookkeeping tail too so we can see if anything between
    // draw_bitmap-returns and the response-send adds up.
    t_post = esp_timer_get_time();

    // Idle gap = time from the PREVIOUS /frame response finishing to
    // THIS request landing on the handler. Large gap = upstream
    // (Scrypted / ffmpeg / TCP slow-start on a fresh socket) was idle;
    // we're not the bottleneck. Skipped on the very first frame.
    int64_t idle_us = s_last_post_us ? (t_entry - s_last_post_us) : 0;
    s_last_post_us = t_post;

    // Timing log every 10 frames so the user can see where each /frame's
    // wall-clock budget is going. Buckets:
    //   idle  : time since the previous response finished (upstream gap)
    //   lock  : mutex acquire
    //   ttfb  : lock-acquired -> first body byte (TCP setup)
    //   body  : remaining body bytes (wire time)
    //   dec   : hardware JPEG decode
    //   paint : esp_lcd_panel_draw_bitmap → DSI fast-path
    //   post  : state counters + unlock
    if (fr % 10 == 0) {
        ESP_LOGI(TAG,
            "frame %llu: idle=%lldus lock=%lldus ttfb=%lldus body=%lldus "
            "dec=%lldus paint=%lldus post=%lldus total=%lldms (jpeg=%uKB)",
            (unsigned long long)fr,
            (long long)idle_us,
            (long long)(t_lock       - t_entry),
            (long long)(t_first_byte - t_lock),
            (long long)(t_recv       - t_first_byte),
            (long long)(t_decode     - t_recv),
            (long long)(t_paint      - t_decode),
            (long long)(t_post       - t_paint),
            (long long)((t_post - t_entry) / 1000),
            (unsigned)(got / 1024));
    }

    state_machine_frame_painted();  // reset idle timer

    // Server-Timing per-stage breakdown so the Scrypted side can build
    // a unified end-to-end trace per frame (joined by X-Frame-Seq).
    // Units are ms with 0.1ms precision — paint is sub-millisecond
    // so the decimal matters there. Scrypted subtracts the sum from
    // its own (fetch_start → Response_headers) measurement to derive
    // the network up + handler-dispatch overhead it can't see directly.
    char st_hdr[160];
    snprintf(st_hdr, sizeof(st_hdr),
             "recv;dur=%.1f, dec;dur=%.1f, paint;dur=%.1f, post;dur=%.1f, handle;dur=%.1f",
             (t_recv   - t_first_byte) / 1000.0,
             (t_decode - t_recv)       / 1000.0,
             (t_paint  - t_decode)     / 1000.0,
             (t_post   - t_paint)      / 1000.0,
             (t_post   - t_entry)      / 1000.0);
    httpd_resp_set_hdr(req, "Server-Timing", st_hdr);

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
static const httpd_uri_t s_state_post = {
    .uri = "/state",  .method = HTTP_POST, .handler = state_post_handler,
};
static const httpd_uri_t s_frame_post = {
    .uri = "/frame",  .method = HTTP_POST, .handler = frame_post_handler,
};

esp_err_t http_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 8192;  // POST /config alone has ~2.4 KiB of stack locals
    // Allow Scrypted to keep two POST /frame in flight at once so the
    // body upload of frame N+1 overlaps with the JPEG decode of frame
    // N. The decoder mutex still serialises decode itself; this only
    // unblocks the network half of the pipeline. +1 socket reserve for
    // a concurrent /state or /config request landing during a stream.
    cfg.max_open_sockets = 4;

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

    ESP_LOGI(TAG, "http server listening on :80 "
                  "(GET/POST /state, GET/POST /config, POST /frame)");
    return ESP_OK;
}
