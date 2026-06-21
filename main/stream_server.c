#include "stream_server.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sys/ioctl.h"

#include "display.h"
#include "jpeg_decoder.h"
#include "state_machine.h"
#include "viewport_state.h"

static const char *TAG = "stream";

// Why TCP and not UDP.
//
// JPEGs at panel-native q:v 1 are ~180 KB → ~123 IP datagrams at 1500
// MTU. On hardwired Gigabit LAN with a single managed switch, fabric
// loss is < 1e-9/packet, so per-frame corruption is ≈1.2e-7 (~1 bad
// frame every 95 days at 30 fps). UDP's theoretical wins don't apply
// to this deployment:
//
//   - Nagle/SACK/window-scaling overhead: socket.write p50 < 1ms in
//     steady state (see Scrypted-side log). TCP_NODELAY is on. No
//     per-frame ACK round-trip exists in either direction.
//   - "Latest wins" semantics: implemented for free above via the
//     FIONREAD check below. UDP would have to reassemble fragments
//     first before deciding to skip, defeating the latency win.
//
// UDP's costs would be real: 200-400 LOC of app-layer fragmentation
// + reassembly + partial-frame timeout, loss of `nc localhost 81 |
// xxd` for debug, and the FIONREAD-skip trick stops working because
// UDP recvmsg counts fragments not whole messages.
//
// Revisit only if a Wi-Fi-only variant ships (loss rate 1e-3..1e-5
// would make TCP retransmits painful enough to consider the rewrite).

#define HEADER_V0_BYTES 8    // legacy: jpeg_len + seq
#define HEADER_V1_BYTES 16   // current: magic + jpeg_len + seq + event_us_low
#define HEADER_BYTES    HEADER_V0_BYTES   // used for FIONREAD threshold —
                                          // "another header may already
                                          // be queued" check needs only
                                          // the smaller of the two.
#define MAGIC_V1        0x56505254u   // "VPRT" big-endian

static uint16_t s_port;

// Last-window stats snapshot, written by handle_client at window roll
// and read by /state via stream_server_snapshot_stats. portMUX keeps
// the writer atomic against a concurrent reader on the other core.
static portMUX_TYPE          s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static stream_server_stats_t s_stats;

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

    // One-shot probe of the kernel recv buffer this connection got.
    // Stash in s_stats and log so we can confirm sdkconfig values
    // actually reached the build — TCP_WND_DEFAULT discrepancies are
    // invisible from source alone.
    int       so_rcvbuf     = 0;
    socklen_t so_rcvbuf_len = sizeof(so_rcvbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &so_rcvbuf, &so_rcvbuf_len) == 0) {
        ESP_LOGI(TAG, "client %s SO_RCVBUF=%d", peer, so_rcvbuf);
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.so_rcvbuf = (uint32_t)so_rcvbuf;
        portEXIT_CRITICAL(&s_stats_mux);
    }

    // Sequence counter is per-connection — Scrypted resets to 1 on
    // each new socket so we follow.
    uint32_t last_painted_seq = 0;
    uint64_t frames_decoded   = 0;
    int64_t  t_window_start   = esp_timer_get_time();
    uint64_t bytes_in_window  = 0;
    int64_t  t_prev_paint_done = 0;        // for idle-gap measurement

    // Per-window min/max accumulators (microseconds).
    int64_t  recv_min = INT64_MAX, recv_max = 0, recv_sum = 0;
    int64_t  dec_min  = INT64_MAX, dec_max  = 0, dec_sum  = 0;
    int64_t  pnt_min  = INT64_MAX, pnt_max  = 0, pnt_sum  = 0;
    int64_t  idle_min = INT64_MAX, idle_max = 0, idle_sum = 0;
    int64_t  lock_min = INT64_MAX, lock_max = 0, lock_sum = 0;
    // Recv-throughput diagnostics (see stream_server.h for meaning).
    uint32_t queued_min     = UINT32_MAX, queued_max     = 0; uint64_t queued_sum     = 0;
    uint32_t calls_min      = UINT32_MAX, calls_max      = 0; uint64_t calls_sum      = 0;
    uint32_t chunk_min      = UINT32_MAX, chunk_max      = 0; uint64_t chunk_total_calls = 0;
    uint64_t window_samples = 0;
    uint32_t last_event_us_low = 0;   // captured from v1 headers

    while (1) {
        // Sniff the first 4 bytes — they're either the v1 magic
        // "VPRT" or the v0 jpeg_len field. Decide once per frame.
        uint8_t first4[4];
        if (read_n(fd, first4, 4) != ESP_OK) {
            ESP_LOGI(TAG, "client %s disconnected (header read)", peer);
            return;
        }
        uint32_t first_word = ((uint32_t)first4[0] << 24) | ((uint32_t)first4[1] << 16)
                            | ((uint32_t)first4[2] << 8)  |  (uint32_t)first4[3];

        uint32_t jpeg_len, seq, event_us_low;
        if (first_word == MAGIC_V1) {
            // v1: 16-byte header total, 12 more bytes after the magic.
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
            // v0: 8-byte header, 4 more bytes after the first word.
            // first_word is jpeg_len; read seq.
            uint8_t rest[HEADER_V0_BYTES - 4];
            if (read_n(fd, rest, sizeof(rest)) != ESP_OK) {
                ESP_LOGI(TAG, "client %s disconnected (v0 header read)", peer);
                return;
            }
            jpeg_len     = first_word;
            seq          = ((uint32_t)rest[0] << 24) | ((uint32_t)rest[1] << 16)
                         | ((uint32_t)rest[2] << 8)  |  (uint32_t)rest[3];
            event_us_low = 0;   // legacy clients don't supply it
        }

        if (jpeg_len == 0 || jpeg_len > JPEG_DECODER_MAX_INPUT_BYTES) {
            ESP_LOGW(TAG, "bad frame length %u from %s — closing connection",
                     (unsigned)jpeg_len, peer);
            return;
        }

        int64_t t_entry = esp_timer_get_time();
        // Idle gap: time between the previous paint completing and
        // this header landing. Large idle = ffmpeg/network is the
        // bottleneck; near-zero idle = we're the bottleneck, source
        // is queued up waiting.
        int64_t idle_us = t_prev_paint_done ? (t_entry - t_prev_paint_done) : 0;

        if (!jpeg_decoder_try_lock(2000)) {
            ESP_LOGW(TAG, "decoder busy 2s — skipping seq %u", (unsigned)seq);
            if (drain_n(fd, jpeg_len) != ESP_OK) return;
            continue;
        }
        int64_t t_lock_done = esp_timer_get_time();

        // Sample how much of the body the kernel has already absorbed.
        // Large value (close to jpeg_len) = wire delivered the whole
        // frame during the previous decode+paint; we're decode/paint-
        // bound. Small value = wire is throttled (window/buffer too
        // small to absorb a full frame in our paint window).
        int queued_at_body = 0;
        (void)ioctl(fd, FIONREAD, &queued_at_body);

        uint8_t *in = jpeg_decoder_input_buffer();
        // Instrumented body read. Counts recv() syscalls and tracks
        // min/max chunk size to characterize how the wire delivered
        // this frame.
        uint32_t frame_calls       = 0;
        uint32_t frame_chunk_min   = UINT32_MAX;
        uint32_t frame_chunk_max   = 0;
        {
            size_t got = 0;
            while (got < jpeg_len) {
                ssize_t r = recv(fd, in + got, jpeg_len - got, 0);
                if (r <= 0) {
                    jpeg_decoder_unlock();
                    ESP_LOGI(TAG, "client %s disconnected (body read)", peer);
                    return;
                }
                uint32_t rb = (uint32_t)r;
                frame_calls++;
                if (rb < frame_chunk_min) frame_chunk_min = rb;
                if (rb > frame_chunk_max) frame_chunk_max = rb;
                got += (size_t)r;
            }
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

        // If the kernel already has another header queued, this frame
        // is no longer the freshest; skip decode + paint, loop back to
        // the head-of-queue header.
        int queued = 0;
        if (ioctl(fd, FIONREAD, &queued) == 0 && queued >= HEADER_BYTES) {
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
        if (event_us_low != 0) {
            last_event_us_low = event_us_low;
            // Live update: /state needs this fresh on every painted
            // frame so the script's g2g reflects the actual age of
            // the currently-displayed frame, not the age at last
            // window roll (which can be up to ~1.5s stale). The
            // other window stats stay at roll cadence — they
            // genuinely need a full window to compute.
            portENTER_CRITICAL(&s_stats_mux);
            s_stats.last_paint_event_us_low = event_us_low;
            portEXIT_CRITICAL(&s_stats_mux);
        }
        jpeg_decoder_unlock();
        state_machine_frame_painted();
        frames_decoded++;

        // Stage timings for this frame, in microseconds.
        int64_t lock_us = t_lock_done - t_entry;
        int64_t recv_us = t_recv      - t_lock_done;
        int64_t dec_us  = t_decode    - t_recv;
        int64_t pnt_us  = t_paint     - t_decode;

        if (lock_us < lock_min) lock_min = lock_us;
        if (lock_us > lock_max) lock_max = lock_us;
        lock_sum += lock_us;
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
        // Recv-throughput diagnostics: aggregate this frame's samples.
        if ((uint32_t)queued_at_body < queued_min) queued_min = (uint32_t)queued_at_body;
        if ((uint32_t)queued_at_body > queued_max) queued_max = (uint32_t)queued_at_body;
        queued_sum += (uint32_t)queued_at_body;
        if (frame_calls < calls_min) calls_min = frame_calls;
        if (frame_calls > calls_max) calls_max = frame_calls;
        calls_sum += frame_calls;
        if (frame_chunk_min != UINT32_MAX && frame_chunk_min < chunk_min) chunk_min = frame_chunk_min;
        if (frame_chunk_max > chunk_max) chunk_max = frame_chunk_max;
        chunk_total_calls += frame_calls;
        window_samples++;
        t_prev_paint_done = t_paint;

        // Every 30 frames log a windowed min/avg/max breakdown +
        // sustained throughput. Idle = gap between previous paint
        // and next header arriving (= upstream slack). lock = mutex
        // acquire (should be ~0 with single client). recv = body
        // bytes off the wire. dec = HW JPEG. paint = backbuffer flip.
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
            ESP_LOGI(TAG,
                "%llu frames over %.1fs: %.1ffps %.2fMB/s avg-jpeg=%uKB | "
                "lock min/avg/max=%lld/%lld/%lldus | "
                "recv min/avg/max=%lld/%lld/%lldus | "
                "dec  min/avg/max=%lld/%lld/%lldus | "
                "paint min/avg/max=%lld/%lld/%lldus | "
                "idle min/avg/max=%lld/%lld/%lldus | "
                "queued min/avg/max=%u/%u/%uB | "
                "recv_calls min/avg/max=%u/%u/%u | "
                "recv_chunk min/avg/max=%u/%u/%uB",
                (unsigned long long)window_samples, win_s, fps, mb_per_s,
                (unsigned)((bytes_in_window / window_samples) / 1024),
                (long long)lock_min, (long long)(lock_sum / window_samples), (long long)lock_max,
                (long long)recv_min, (long long)(recv_sum / window_samples), (long long)recv_max,
                (long long)dec_min,  (long long)(dec_sum  / window_samples), (long long)dec_max,
                (long long)pnt_min,  (long long)(pnt_sum  / window_samples), (long long)pnt_max,
                (long long)(idle_min == INT64_MAX ? 0 : idle_min),
                (long long)(idle_sum  / window_samples),
                (long long)idle_max,
                (unsigned)(queued_min == UINT32_MAX ? 0 : queued_min), (unsigned)queued_avg, (unsigned)queued_max,
                (unsigned)(calls_min  == UINT32_MAX ? 0 : calls_min),  (unsigned)calls_avg,  (unsigned)calls_max,
                (unsigned)(chunk_min  == UINT32_MAX ? 0 : chunk_min),  (unsigned)chunk_avg,  (unsigned)chunk_max);

            // Publish the just-closed window so /state can expose it.
            // Skipped under portENTER_CRITICAL — handful of integer
            // moves, completes in single-digit µs.
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
                // so_rcvbuf is set at accept time; preserve across window rolls.
                .so_rcvbuf       = s_stats.so_rcvbuf,
                .last_paint_event_us_low = last_event_us_low,
            };
            portENTER_CRITICAL(&s_stats_mux);
            s_stats = snap;
            portEXIT_CRITICAL(&s_stats_mux);

            t_window_start  = now;
            bytes_in_window = 0;
            recv_min = dec_min = pnt_min = idle_min = lock_min = INT64_MAX;
            recv_max = dec_max = pnt_max = idle_max = lock_max = 0;
            recv_sum = dec_sum = pnt_sum = idle_sum = lock_sum = 0;
            queued_min = calls_min = chunk_min = UINT32_MAX;
            queued_max = calls_max = chunk_max = 0;
            queued_sum = calls_sum = chunk_total_calls = 0;
            window_samples = 0;
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

void stream_server_snapshot_stats(stream_server_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}
