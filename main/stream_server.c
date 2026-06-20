#include "stream_server.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "display.h"
#include "jpeg_decoder.h"
#include "state_machine.h"
#include "viewport_state.h"

static const char *TAG = "stream";

#define HEADER_BYTES 8   // 4 jpeg_len + 4 seq

static uint16_t s_port;

// recv() in a loop until n bytes are read or the connection drops.
// Returns ESP_OK on full read, ESP_FAIL on EOF or socket error.
static esp_err_t read_n(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t   got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    return ESP_OK;
}

// Read and discard exactly n bytes — used to stay framed when we
// have to skip a frame (decoder busy, asleep, etc) without dropping
// the connection.
static esp_err_t drain_n(int fd, size_t n)
{
    uint8_t  scratch[256];
    while (n > 0) {
        size_t  want = n > sizeof(scratch) ? sizeof(scratch) : n;
        ssize_t r    = recv(fd, scratch, want, 0);
        if (r <= 0) return ESP_FAIL;
        n -= (size_t)r;
    }
    return ESP_OK;
}

// One client owns the decoder for the lifetime of the connection.
// Loops: header → body → (decode + paint OR skip) → repeat.
static void handle_client(int fd, const char *peer)
{
    // NODELAY on the accepted socket so our outbound (response-less)
    // ACKs flow immediately. There's no app-layer "response" in this
    // protocol so we have to make sure TCP doesn't withhold ACKs
    // waiting to piggyback on data we'll never send.
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Sequence counter is per-connection — Scrypted resets to 1 on
    // each new socket so we follow.
    uint32_t last_painted_seq = 0;
    uint64_t frames_decoded   = 0;
    int64_t  t_window_start   = esp_timer_get_time();
    uint64_t bytes_in_window  = 0;

    while (1) {
        uint8_t hdr[HEADER_BYTES];
        if (read_n(fd, hdr, HEADER_BYTES) != ESP_OK) {
            ESP_LOGI(TAG, "client %s disconnected (header read)", peer);
            return;
        }
        uint32_t jpeg_len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                         | ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
        uint32_t seq      = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16)
                         | ((uint32_t)hdr[6] << 8)  |  (uint32_t)hdr[7];

        if (jpeg_len == 0 || jpeg_len > JPEG_DECODER_MAX_INPUT_BYTES) {
            ESP_LOGW(TAG, "bad frame length %u from %s — closing connection",
                     (unsigned)jpeg_len, peer);
            return;
        }

        int64_t t_entry = esp_timer_get_time();

        if (!jpeg_decoder_try_lock(2000)) {
            ESP_LOGW(TAG, "decoder busy 2s — skipping seq %u", (unsigned)seq);
            if (drain_n(fd, jpeg_len) != ESP_OK) return;
            continue;
        }

        uint8_t *in = jpeg_decoder_input_buffer();
        if (read_n(fd, in, jpeg_len) != ESP_OK) {
            jpeg_decoder_unlock();
            ESP_LOGI(TAG, "client %s disconnected (body read)", peer);
            return;
        }
        int64_t t_recv = esp_timer_get_time();
        bytes_in_window += jpeg_len;

        // While asleep we still drain frames (to stay in sync) but
        // skip the decode + paint. The state machine will wake on a
        // POST /state {wake} from the control plane.
        if (state_machine_current() != VIEWPORT_STATE_AWAKE) {
            jpeg_decoder_unlock();
            continue;
        }

        // Stale-seq guard. Each socket starts at 0 so the first
        // frame (seq=1) always paints.
        if (seq != 0 && seq <= last_painted_seq) {
            jpeg_decoder_unlock();
            continue;
        }

        size_t   back_size = 0;
        void    *back      = display_back_buffer(&back_size);
        uint16_t w = 0, h = 0;
        if (jpeg_decoder_decode(jpeg_len, back, back_size, &w, &h) != ESP_OK) {
            viewport_state_lock();
            viewport_state_get()->decode_errors++;
            viewport_state_unlock();
            jpeg_decoder_unlock();
            continue;
        }
        if (w != VIEWPORT_PANEL_WIDTH || h != VIEWPORT_PANEL_HEIGHT) {
            viewport_state_lock();
            viewport_state_get()->decode_errors++;
            viewport_state_unlock();
            jpeg_decoder_unlock();
            ESP_LOGW(TAG, "dim mismatch seq=%u: expected %ux%u got %ux%u",
                     (unsigned)seq, VIEWPORT_PANEL_WIDTH, VIEWPORT_PANEL_HEIGHT, w, h);
            continue;
        }
        int64_t t_decode = esp_timer_get_time();
        if (display_flip_back_buffer() != ESP_OK) {
            jpeg_decoder_unlock();
            continue;
        }
        int64_t t_paint = esp_timer_get_time();

        viewport_state_lock();
        viewport_state_t *st = viewport_state_get();
        st->frames_received++;
        st->last_frame_us = esp_timer_get_time();
        viewport_state_unlock();

        last_painted_seq = seq;
        jpeg_decoder_unlock();
        state_machine_frame_painted();
        frames_decoded++;

        // Every 30 frames log a per-stage breakdown + window throughput
        // — same shape as the old /frame log so the serial output stays
        // useful for debugging without the script side.
        if (frames_decoded % 30 == 0) {
            int64_t now      = esp_timer_get_time();
            double  win_s    = (now - t_window_start) / 1.0e6;
            double  mb_per_s = (win_s > 0)
                ? ((double)bytes_in_window / win_s) / (1024.0 * 1024.0) : 0.0;
            ESP_LOGI(TAG,
                "frame %llu seq=%u: recv=%lldus dec=%lldus paint=%lldus "
                "total=%lldus (jpeg=%uKB) window=%llumiB/%.1fs (%.2fMB/s)",
                (unsigned long long)frames_decoded, (unsigned)seq,
                (long long)(t_recv   - t_entry),
                (long long)(t_decode - t_recv),
                (long long)(t_paint  - t_decode),
                (long long)(t_paint  - t_entry),
                (unsigned)(jpeg_len / 1024),
                (unsigned long long)(bytes_in_window / (1024 * 1024)),
                win_s, mb_per_s);
            t_window_start  = now;
            bytes_in_window = 0;
        }
    }
}

static void accept_task(void *arg)
{
    (void)arg;
    int listen_sock = -1;

    while (1) {
        if (listen_sock < 0) {
            listen_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (listen_sock < 0) {
                ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int reuse = 1;
            setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            struct sockaddr_in addr = {
                .sin_family      = AF_INET,
                .sin_addr.s_addr = htonl(INADDR_ANY),
                .sin_port        = htons(s_port),
            };
            if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ESP_LOGE(TAG, "bind(%d) failed: errno=%d", s_port, errno);
                close(listen_sock);
                listen_sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (listen(listen_sock, 1) < 0) {
                ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
                close(listen_sock);
                listen_sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "stream server listening on tcp/:%d", s_port);
        }

        struct sockaddr_in client;
        socklen_t          client_len = sizeof(client);
        int fd = accept(listen_sock, (struct sockaddr *)&client, &client_len);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntoa_r(client.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "client connected from %s:%u",
                 ip, (unsigned)ntohs(client.sin_port));

        handle_client(fd, ip);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

esp_err_t stream_server_start(uint16_t port)
{
    s_port = port;
    BaseType_t ok = xTaskCreate(accept_task, "stream", 8192, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
