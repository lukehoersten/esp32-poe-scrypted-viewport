#include "stream_server.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sys/ioctl.h"

#include "display.h"
#include "jpeg_decoder.h"
#include "local_screens.h"
#include "state_machine.h"
#include "viewport_state.h"

static const char *TAG = "stream";

// Why TCP and not UDP.
//
// JPEGs at panel-native q:v 1 are ~180 KB → ~123 IP datagrams at 1500
// MTU. On hardwired Gigabit LAN with a single managed switch, fabric
// loss is < 1e-9/packet, so per-frame corruption is ≈1.2e-7 (~1 bad
// frame every 95 days at 30 fps). UDP's theoretical wins don't apply
// to this deployment.

#define HEADER_V0_BYTES 8    // legacy: jpeg_len + seq
#define HEADER_V1_BYTES 16   // current: magic + jpeg_len + seq + event_us_low
#define HEADER_BYTES    HEADER_V0_BYTES   // FIONREAD threshold for
                                          // "another header may already
                                          // be queued" check.
#define MAGIC_V1        0x56505254u   // "VPRT" big-endian
#define NUM_BODY_BUFS   3             // ping-pong ring: 1 recv + 1 pending
                                      // + 1 decode → recv never blocks.

// ─── Architecture ────────────────────────────────────────────────────────────
//
// Two FreeRTOS tasks share the body data plane through a 3-buffer ring:
//
//   recv-task:  owns the TCP socket. Reads header + body into s_bufs[recv_idx].
//               When a body completes, atomically installs it as the latest
//               pending frame and grabs a fresh buffer for the next recv.
//               If pending already held a frame (decode is slow), the older
//               pending is overwritten in place — receiver-side skip-oldest,
//               mirror of what the Scrypted plugin does at the source.
//
//   decode-task: waits on a binary semaphore. When signaled, claims the
//                pending frame, then decodes + paints without holding any
//                shared lock. Frees its prior buffer only by overwriting
//                s_decode_idx on the next claim — so recv-task always sees
//                an accurate "buffer in use by decode" via the indices.
//
// Invariant: {s_recv_idx, s_pending_idx, s_decode_idx} are pairwise distinct
// modulo the -1 sentinel for "unused." With 3 buffers and at most 3 roles,
// pick_free() always finds a buffer outside the used set.
//
// This removes the recv-blocking-on-decode coupling that made the older
// 5760-byte window the only working window: even with the kernel buffer
// near-empty during decode+paint (6ms), recv-task is concurrently draining
// the socket and the sender never sees window-zero advertised. Window-size
// tuning becomes meaningful again (separate iteration).

static uint16_t s_port;

// Last-window stats snapshot, written by decode-task at window roll and
// read by /state via stream_server_snapshot_stats.
static portMUX_TYPE          s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static stream_server_stats_t s_stats;

// 3-buffer ring + 1-slot handoff. All index manipulation under s_slot_mux.
static uint8_t          *s_bufs[NUM_BODY_BUFS];
static size_t            s_buf_cap;
static int               s_recv_idx    = 0;
static int               s_decode_idx  = -1;   // -1 = decoder hasn't claimed yet
static int               s_pending_idx = -1;   // -1 = no frame waiting
static portMUX_TYPE      s_slot_mux    = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_decode_signal;      // binary semaphore recv → decode
static uint32_t          s_recv_dropped_oldest_window = 0;   // reset on window roll
static uint32_t          s_conn_id     = 0;                  // bumped on each new client

// Metadata for the latest pending frame. Written by recv-task before
// signaling; read by decode-task after taking the signal.
typedef struct {
    uint32_t jpeg_len;
    uint32_t seq;
    uint32_t event_us_low;
    uint32_t conn_id;
    // recv diagnostics captured at recv time (aggregated by decode-task
    // into the windowed stats since decode-task owns the window).
    uint32_t queued_at_body;
    uint32_t recv_calls;
    uint32_t recv_chunk_min;
    uint32_t recv_chunk_max;
    int64_t  recv_us;           // body-recv duration on the recv-task side
} slot_meta_t;
static slot_meta_t s_pending_meta;

// Pick a buffer index not currently used by any role. Caller holds s_slot_mux.
// With NUM_BODY_BUFS=3 and at most 2 indices "in use" (recv + decode, since
// pending was just consumed or just installed), there's always exactly one
// free buffer.
static int pick_free_locked(int avoid_a, int avoid_b)
{
    for (int i = 0; i < NUM_BODY_BUFS; i++) {
        if (i != avoid_a && i != avoid_b) return i;
    }
    return -1;   // unreachable with NUM_BODY_BUFS >= 3
}

static esp_err_t alloc_body_bufs(void)
{
    for (int i = 0; i < NUM_BODY_BUFS; i++) {
        size_t cap = 0;
        s_bufs[i] = jpeg_decoder_alloc_input_buffer(&cap);
        if (!s_bufs[i]) {
            ESP_LOGE(TAG, "body buf %d alloc failed", i);
            return ESP_ERR_NO_MEM;
        }
        if (i == 0) s_buf_cap = cap;
    }
    ESP_LOGI(TAG, "stream body ring: %d × %u bytes PSRAM", NUM_BODY_BUFS, (unsigned)s_buf_cap);
    return ESP_OK;
}

// recv() in a loop until n bytes are read or the connection drops.
// Used for short fixed-size reads (header bytes).
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

// ───────────────────────── recv-task ─────────────────────────────────────────

// Per-frame body read with instrumentation: count recv() syscalls and track
// chunk size distribution. Returns ESP_OK on full body; ESP_FAIL on EOF/error.
static esp_err_t read_body_instrumented(int fd, void *buf, size_t n,
                                        uint32_t *out_calls,
                                        uint32_t *out_chunk_min,
                                        uint32_t *out_chunk_max)
{
    uint8_t *p = (uint8_t *)buf;
    size_t   got = 0;
    *out_calls     = 0;
    *out_chunk_min = UINT32_MAX;
    *out_chunk_max = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return ESP_FAIL;
        uint32_t rb = (uint32_t)r;
        (*out_calls)++;
        if (rb < *out_chunk_min) *out_chunk_min = rb;
        if (rb > *out_chunk_max) *out_chunk_max = rb;
        got += (size_t)r;
    }
    return ESP_OK;
}

// Install a just-received frame into the pending slot and rotate the recv
// target to a fresh buffer. If pending already held a frame, the older one
// is overwritten in place (drop-oldest at the receiver).
static void publish_frame(const slot_meta_t *meta)
{
    portENTER_CRITICAL(&s_slot_mux);
    if (s_pending_idx >= 0) {
        // Drop-oldest: swap recv_idx with pending_idx so the just-filled
        // buffer becomes the new pending; the old pending (overwritten)
        // becomes the next recv target.
        int tmp = s_pending_idx;
        s_pending_idx = s_recv_idx;
        s_recv_idx    = tmp;
        s_recv_dropped_oldest_window++;
    } else {
        // No prior pending: install just-filled as pending and pick a
        // free buffer for the next recv. Free = the one not in use by
        // recv or decode.
        s_pending_idx = s_recv_idx;
        s_recv_idx    = pick_free_locked(s_pending_idx, s_decode_idx);
    }
    s_pending_meta = *meta;
    portEXIT_CRITICAL(&s_slot_mux);
    xSemaphoreGive(s_decode_signal);
}

static void handle_client_recv(int fd, const char *peer)
{
    // NODELAY on the accepted socket so our outbound (response-less) ACKs
    // flow immediately. There's no app-layer "response" in this protocol
    // so we must prevent TCP withholding ACKs to piggyback.
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // One-shot probe of the kernel recv buffer this connection got.
    int       so_rcvbuf     = 0;
    socklen_t so_rcvbuf_len = sizeof(so_rcvbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &so_rcvbuf, &so_rcvbuf_len) == 0) {
        ESP_LOGI(TAG, "client %s SO_RCVBUF=%d", peer, so_rcvbuf);
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.so_rcvbuf = (uint32_t)so_rcvbuf;
        portEXIT_CRITICAL(&s_stats_mux);
    }

    // Bump the connection id so decode-task can reset its per-connection
    // seq tracking when frames from this conn start arriving.
    uint32_t conn_id;
    portENTER_CRITICAL(&s_slot_mux);
    conn_id = ++s_conn_id;
    portEXIT_CRITICAL(&s_slot_mux);

    // Clear any stale prior frame to the "Loading..." screen at the start of
    // every stream session, so the panel doesn't sit on an old image during
    // the connect→first-frame gap (several seconds on a cold prebuffer; the
    // Scrypted side no longer sends a bridging snapshot). The wake path also
    // shows this, but a stream can start while already AWAKE (e.g. a restart)
    // where the state-machine wake is a no-op — this covers that case too.
    // Guard with the decoder lock so the RGB565 present can't race a trailing
    // decode-task paint from the previous connection; skip if not awake.
    if (state_machine_current() == VIEWPORT_STATE_AWAKE &&
        jpeg_decoder_try_lock(500)) {
        local_screens_show_loading();
        jpeg_decoder_unlock();
    }

    while (1) {
        uint8_t first4[4];
        if (read_n(fd, first4, 4) != ESP_OK) {
            ESP_LOGI(TAG, "client %s disconnected (header read)", peer);
            return;
        }
        uint32_t first_word = ((uint32_t)first4[0] << 24) | ((uint32_t)first4[1] << 16)
                            | ((uint32_t)first4[2] << 8)  |  (uint32_t)first4[3];

        uint32_t jpeg_len, seq, event_us_low;
        if (first_word == MAGIC_V1) {
            uint8_t rest[HEADER_V1_BYTES - 4];
            if (read_n(fd, rest, sizeof(rest)) != ESP_OK) {
                ESP_LOGI(TAG, "client %s disconnected (v1 header read)", peer);
                return;
            }
            jpeg_len     = ((uint32_t)rest[0]  << 24) | ((uint32_t)rest[1]  << 16)
                         | ((uint32_t)rest[2]  << 8)  |  (uint32_t)rest[3];
            seq          = ((uint32_t)rest[4]  << 24) | ((uint32_t)rest[5]  << 16)
                         | ((uint32_t)rest[6]  << 8)  |  (uint32_t)rest[7];
            event_us_low = ((uint32_t)rest[8]  << 24) | ((uint32_t)rest[9]  << 16)
                         | ((uint32_t)rest[10] << 8)  |  (uint32_t)rest[11];
        } else {
            uint8_t rest[HEADER_V0_BYTES - 4];
            if (read_n(fd, rest, sizeof(rest)) != ESP_OK) {
                ESP_LOGI(TAG, "client %s disconnected (v0 header read)", peer);
                return;
            }
            jpeg_len     = first_word;
            seq          = ((uint32_t)rest[0] << 24) | ((uint32_t)rest[1] << 16)
                         | ((uint32_t)rest[2] << 8)  |  (uint32_t)rest[3];
            event_us_low = 0;
        }

        if (jpeg_len == 0 || jpeg_len > s_buf_cap) {
            ESP_LOGW(TAG, "bad frame length %u from %s — closing connection",
                     (unsigned)jpeg_len, peer);
            return;
        }

        // Sample how much of the body the kernel has already absorbed
        // before we drain it. Close to jpeg_len → wire delivered the
        // whole frame during the previous decode+paint (we're decode-
        // bound). Small → wire is slow (window or buffer too small).
        int queued_at_body = 0;
        (void)ioctl(fd, FIONREAD, &queued_at_body);

        // Read into the current recv buffer. recv-task is the only writer
        // of s_recv_idx so we can read it without the mutex.
        uint8_t *body = s_bufs[s_recv_idx];
        int64_t  t0   = esp_timer_get_time();
        uint32_t calls = 0, chunk_min = 0, chunk_max = 0;
        if (read_body_instrumented(fd, body, jpeg_len,
                                   &calls, &chunk_min, &chunk_max) != ESP_OK) {
            ESP_LOGI(TAG, "client %s disconnected (body read)", peer);
            return;
        }
        int64_t recv_us = esp_timer_get_time() - t0;

        slot_meta_t meta = {
            .jpeg_len       = jpeg_len,
            .seq            = seq,
            .event_us_low   = event_us_low,
            .conn_id        = conn_id,
            .queued_at_body = (uint32_t)queued_at_body,
            .recv_calls     = calls,
            .recv_chunk_min = (chunk_min == UINT32_MAX ? 0 : chunk_min),
            .recv_chunk_max = chunk_max,
            .recv_us        = recv_us,
        };
        publish_frame(&meta);
    }
}

static void recv_task(void *arg)
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

        handle_client_recv(fd, ip);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

// ───────────────────────── decode-task ───────────────────────────────────────

// Claim the latest pending frame and rotate decode's owned buffer. Caller
// gets the buffer pointer + metadata to process. Returns false if there's
// no pending frame (spurious signal — safe to loop and re-wait).
static bool claim_pending(uint8_t **out_buf, slot_meta_t *out_meta)
{
    portENTER_CRITICAL(&s_slot_mux);
    if (s_pending_idx < 0) {
        portEXIT_CRITICAL(&s_slot_mux);
        return false;
    }
    s_decode_idx  = s_pending_idx;   // hand-off + implicit free of prior decode_idx
    s_pending_idx = -1;
    *out_meta     = s_pending_meta;
    portEXIT_CRITICAL(&s_slot_mux);
    *out_buf = s_bufs[s_decode_idx];
    return true;
}

static void decode_task(void *arg)
{
    (void)arg;

    uint32_t last_painted_seq  = 0;
    uint32_t last_conn_id      = 0;
    uint64_t frames_decoded    = 0;
    int64_t  t_window_start    = esp_timer_get_time();
    uint64_t bytes_in_window   = 0;
    int64_t  t_prev_paint_done = 0;
    uint32_t last_event_us_low = 0;

    // Per-window accumulators (microseconds).
    int64_t  recv_min = INT64_MAX, recv_max = 0, recv_sum = 0;
    int64_t  dec_min  = INT64_MAX, dec_max  = 0, dec_sum  = 0;
    int64_t  pnt_min  = INT64_MAX, pnt_max  = 0, pnt_sum  = 0;
    int64_t  idle_min = INT64_MAX, idle_max = 0, idle_sum = 0;
    int64_t  dwait_min = INT64_MAX, dwait_max = 0, dwait_sum = 0;
    uint32_t queued_min = UINT32_MAX, queued_max = 0; uint64_t queued_sum = 0;
    uint32_t calls_min  = UINT32_MAX, calls_max  = 0; uint64_t calls_sum  = 0;
    uint32_t chunk_min  = UINT32_MAX, chunk_max  = 0; uint64_t chunk_total_calls = 0;
    uint64_t window_samples = 0;

    while (1) {
        // Wait for recv-task to publish a frame.
        int64_t t_wait_start = esp_timer_get_time();
        if (xSemaphoreTake(s_decode_signal, portMAX_DELAY) != pdTRUE) continue;
        int64_t t_signal = esp_timer_get_time();

        uint8_t    *body;
        slot_meta_t meta;
        if (!claim_pending(&body, &meta)) continue;

        int64_t t_entry = esp_timer_get_time();

        // Idle = previous paint completion → next frame becoming available.
        // dwait = time spent in xSemaphoreTake (sleep waiting for signal),
        // a strict subset of idle minus claim overhead.
        int64_t idle_us  = t_prev_paint_done ? (t_entry - t_prev_paint_done) : 0;
        int64_t dwait_us = t_signal - t_wait_start;

        bytes_in_window += meta.jpeg_len;

        // New connection? Reset stale-seq tracking. recv-task bumped
        // conn_id at accept; if we observe a different one here, we're
        // starting fresh and seq=1 should paint.
        if (meta.conn_id != last_conn_id) {
            last_conn_id     = meta.conn_id;
            last_painted_seq = 0;
        }

        // While asleep we just discard the frame. The state-machine wake
        // happens via POST /state from the control plane.
        if (state_machine_current() != VIEWPORT_STATE_AWAKE) continue;

        // Stale-seq guard.
        if (meta.seq != 0 && meta.seq <= last_painted_seq) continue;

        // Decoder hardware is shared with http_api.c snapshot path; mutex
        // serializes against concurrent /frame POSTs. Stream owns its own
        // body buffer (no contention there); only decode + paint need it.
        if (!jpeg_decoder_try_lock(2000)) {
            ESP_LOGW(TAG, "decoder busy 2s — skipping seq %u", (unsigned)meta.seq);
            continue;
        }

        size_t   back_size = 0;
        void    *back      = display_back_buffer(&back_size);
        uint16_t w = 0, h = 0;
        if (jpeg_decoder_decode(body, meta.jpeg_len, back, back_size, &w, &h) != ESP_OK) {
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
                     (unsigned)meta.seq, VIEWPORT_PANEL_WIDTH, VIEWPORT_PANEL_HEIGHT, w, h);
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

        last_painted_seq = meta.seq;
        if (meta.event_us_low != 0) {
            last_event_us_low = meta.event_us_low;
            // Live update: /state needs this fresh on every painted frame
            // so the script's g2g reflects the actual age of the
            // currently-displayed frame, not the age at last window roll.
            portENTER_CRITICAL(&s_stats_mux);
            s_stats.last_paint_event_us_low = meta.event_us_low;
            portEXIT_CRITICAL(&s_stats_mux);
        }
        jpeg_decoder_unlock();
        state_machine_frame_painted();
        frames_decoded++;

        int64_t dec_us = t_decode - t_entry;
        int64_t pnt_us = t_paint  - t_decode;
        int64_t recv_us = meta.recv_us;

        if (recv_us < recv_min) recv_min = recv_us;
        if (recv_us > recv_max) recv_max = recv_us;
        recv_sum += recv_us;
        if (dec_us  < dec_min)  dec_min  = dec_us;
        if (dec_us  > dec_max)  dec_max  = dec_us;
        dec_sum  += dec_us;
        if (pnt_us  < pnt_min)  pnt_min  = pnt_us;
        if (pnt_us  > pnt_max)  pnt_max  = pnt_us;
        pnt_sum  += pnt_us;
        if (idle_us > 0) {
            if (idle_us < idle_min) idle_min = idle_us;
            if (idle_us > idle_max) idle_max = idle_us;
            idle_sum += idle_us;
        }
        if (dwait_us > 0) {
            if (dwait_us < dwait_min) dwait_min = dwait_us;
            if (dwait_us > dwait_max) dwait_max = dwait_us;
            dwait_sum += dwait_us;
        }
        if (meta.queued_at_body < queued_min) queued_min = meta.queued_at_body;
        if (meta.queued_at_body > queued_max) queued_max = meta.queued_at_body;
        queued_sum += meta.queued_at_body;
        if (meta.recv_calls < calls_min) calls_min = meta.recv_calls;
        if (meta.recv_calls > calls_max) calls_max = meta.recv_calls;
        calls_sum += meta.recv_calls;
        if (meta.recv_chunk_min < chunk_min) chunk_min = meta.recv_chunk_min;
        if (meta.recv_chunk_max > chunk_max) chunk_max = meta.recv_chunk_max;
        chunk_total_calls += meta.recv_calls;
        window_samples++;
        t_prev_paint_done = t_paint;

        if (frames_decoded % 30 == 0 && window_samples > 0) {
            int64_t now      = esp_timer_get_time();
            double  win_s    = (now - t_window_start) / 1.0e6;
            double  mb_per_s = (win_s > 0)
                ? ((double)bytes_in_window / win_s) / (1024.0 * 1024.0) : 0.0;
            double  fps      = (win_s > 0) ? (double)window_samples / win_s : 0.0;
            uint32_t queued_avg = (uint32_t)(queued_sum / window_samples);
            uint32_t calls_avg  = (uint32_t)(calls_sum  / window_samples);
            uint32_t chunk_avg  = chunk_total_calls > 0
                ? (uint32_t)(bytes_in_window / chunk_total_calls) : 0;

            // Snapshot + reset drop-oldest counter for this window.
            portENTER_CRITICAL(&s_slot_mux);
            uint32_t dropped_oldest = s_recv_dropped_oldest_window;
            s_recv_dropped_oldest_window = 0;
            portEXIT_CRITICAL(&s_slot_mux);

            ESP_LOGI(TAG,
                "%llu frames over %.1fs: %.1ffps %.2fMB/s avg-jpeg=%uKB drop-oldest=%u | "
                "recv min/avg/max=%lld/%lld/%lldus | "
                "dec  min/avg/max=%lld/%lld/%lldus | "
                "paint min/avg/max=%lld/%lld/%lldus | "
                "idle min/avg/max=%lld/%lld/%lldus | "
                "decode_wait min/avg/max=%lld/%lld/%lldus | "
                "queued min/avg/max=%u/%u/%uB | "
                "recv_calls min/avg/max=%u/%u/%u | "
                "recv_chunk min/avg/max=%u/%u/%uB",
                (unsigned long long)window_samples, win_s, fps, mb_per_s,
                (unsigned)((bytes_in_window / window_samples) / 1024),
                (unsigned)dropped_oldest,
                (long long)recv_min, (long long)(recv_sum / window_samples), (long long)recv_max,
                (long long)dec_min,  (long long)(dec_sum  / window_samples), (long long)dec_max,
                (long long)pnt_min,  (long long)(pnt_sum  / window_samples), (long long)pnt_max,
                (long long)(idle_min == INT64_MAX ? 0 : idle_min),
                (long long)(idle_sum  / window_samples),
                (long long)idle_max,
                (long long)(dwait_min == INT64_MAX ? 0 : dwait_min),
                (long long)(dwait_sum / window_samples),
                (long long)dwait_max,
                (unsigned)(queued_min == UINT32_MAX ? 0 : queued_min), (unsigned)queued_avg, (unsigned)queued_max,
                (unsigned)(calls_min  == UINT32_MAX ? 0 : calls_min),  (unsigned)calls_avg,  (unsigned)calls_max,
                (unsigned)(chunk_min  == UINT32_MAX ? 0 : chunk_min),  (unsigned)chunk_avg,  (unsigned)chunk_max);

            stream_server_stats_t snap = {
                .frames        = window_samples,
                .bytes         = bytes_in_window,
                .window_us     = (uint64_t)(now - t_window_start),
                .window_end_us = (uint64_t)now,
                .recv_min_us   = (uint32_t)recv_min,
                .recv_avg_us   = (uint32_t)(recv_sum / window_samples),
                .recv_max_us   = (uint32_t)recv_max,
                .dec_min_us    = (uint32_t)dec_min,
                .dec_avg_us    = (uint32_t)(dec_sum  / window_samples),
                .dec_max_us    = (uint32_t)dec_max,
                .pnt_min_us    = (uint32_t)pnt_min,
                .pnt_avg_us    = (uint32_t)(pnt_sum  / window_samples),
                .pnt_max_us    = (uint32_t)pnt_max,
                .idle_min_us   = (uint32_t)(idle_min == INT64_MAX ? 0 : idle_min),
                .idle_avg_us   = (uint32_t)(idle_sum / window_samples),
                .idle_max_us   = (uint32_t)idle_max,
                .queued_min      = (queued_min == UINT32_MAX ? 0 : queued_min),
                .queued_avg      = queued_avg,
                .queued_max      = queued_max,
                .recv_calls_min  = (calls_min  == UINT32_MAX ? 0 : calls_min),
                .recv_calls_avg  = calls_avg,
                .recv_calls_max  = calls_max,
                .recv_chunk_min  = (chunk_min  == UINT32_MAX ? 0 : chunk_min),
                .recv_chunk_avg  = chunk_avg,
                .recv_chunk_max  = chunk_max,
                .so_rcvbuf       = s_stats.so_rcvbuf,
                .recv_dropped_oldest = dropped_oldest,
                .decode_idle_min_us  = (uint32_t)(dwait_min == INT64_MAX ? 0 : dwait_min),
                .decode_idle_avg_us  = (uint32_t)(dwait_sum / window_samples),
                .decode_idle_max_us  = (uint32_t)dwait_max,
                .last_paint_event_us_low = last_event_us_low,
            };
            portENTER_CRITICAL(&s_stats_mux);
            s_stats = snap;
            portEXIT_CRITICAL(&s_stats_mux);

            t_window_start  = now;
            bytes_in_window = 0;
            recv_min = dec_min = pnt_min = idle_min = dwait_min = INT64_MAX;
            recv_max = dec_max = pnt_max = idle_max = dwait_max = 0;
            recv_sum = dec_sum = pnt_sum = idle_sum = dwait_sum = 0;
            queued_min = calls_min = chunk_min = UINT32_MAX;
            queued_max = calls_max = chunk_max = 0;
            queued_sum = calls_sum = chunk_total_calls = 0;
            window_samples = 0;
        }
    }
}

esp_err_t stream_server_start(uint16_t port)
{
    s_port = port;
    if (alloc_body_bufs() != ESP_OK) return ESP_FAIL;
    s_decode_signal = xSemaphoreCreateBinary();
    if (!s_decode_signal) return ESP_ERR_NO_MEM;
    if (xTaskCreate(decode_task, "stream_dec", 8192, NULL, 5, NULL) != pdPASS) return ESP_FAIL;
    if (xTaskCreate(recv_task,   "stream_rcv", 8192, NULL, 5, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}

void stream_server_snapshot_stats(stream_server_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}
