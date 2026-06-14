#include "local_screens.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "net_eth.h"
#include "viewport_state.h"

static const char *TAG = "screens";

#define PANEL_W      800
#define PANEL_H      480
#define MAX_BUF_PX   (PANEL_W * PANEL_H)

// 8x8 bitmap font, lit pixels = foreground. Covers the characters used by
// any text the device draws locally: lowercase a–z, digits, period, colon,
// dash, slash, plus uppercase L for "Loading...". Unsupported chars render
// blank. The table is a contiguous 95-char × 8-byte block; designators
// keep it readable.
static const uint8_t FONT[95][8] = {
    [' '  - 0x20] = {0,0,0,0,0,0,0,0},
    ['-'  - 0x20] = {0,0,0,0x7E,0,0,0,0},
    ['.'  - 0x20] = {0,0,0,0,0,0x18,0x18,0},
    ['/'  - 0x20] = {0x06,0x0C,0x0C,0x18,0x18,0x30,0x30,0x60},
    [':'  - 0x20] = {0,0x18,0x18,0,0,0x18,0x18,0},

    ['0'  - 0x20] = {0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x3C},
    ['1'  - 0x20] = {0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x7E},
    ['2'  - 0x20] = {0x3C,0x66,0x06,0x0C,0x18,0x30,0x60,0x7E},
    ['3'  - 0x20] = {0x3C,0x66,0x06,0x1C,0x06,0x06,0x66,0x3C},
    ['4'  - 0x20] = {0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x0C},
    ['5'  - 0x20] = {0x7E,0x60,0x60,0x7C,0x06,0x06,0x66,0x3C},
    ['6'  - 0x20] = {0x1C,0x30,0x60,0x7C,0x66,0x66,0x66,0x3C},
    ['7'  - 0x20] = {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x30},
    ['8'  - 0x20] = {0x3C,0x66,0x66,0x3C,0x66,0x66,0x66,0x3C},
    ['9'  - 0x20] = {0x3C,0x66,0x66,0x66,0x3E,0x06,0x0C,0x38},

    ['L'  - 0x20] = {0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E},

    ['a'  - 0x20] = {0,0,0x3C,0x06,0x3E,0x66,0x66,0x3E},
    ['b'  - 0x20] = {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C},
    ['c'  - 0x20] = {0,0,0x3C,0x66,0x60,0x60,0x66,0x3C},
    ['d'  - 0x20] = {0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x3E},
    ['e'  - 0x20] = {0,0,0x3C,0x66,0x7E,0x60,0x66,0x3C},
    ['f'  - 0x20] = {0x1C,0x36,0x30,0x7C,0x30,0x30,0x30,0x30},
    ['g'  - 0x20] = {0,0,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    ['h'  - 0x20] = {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66},
    ['i'  - 0x20] = {0x18,0,0x18,0x18,0x18,0x18,0x18,0x18},
    ['j'  - 0x20] = {0x06,0,0x06,0x06,0x06,0x06,0x66,0x3C},
    ['k'  - 0x20] = {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x66},
    ['l'  - 0x20] = {0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x3C},
    ['m'  - 0x20] = {0,0,0x66,0x7F,0x7F,0x6B,0x63,0x63},
    ['n'  - 0x20] = {0,0,0x7C,0x66,0x66,0x66,0x66,0x66},
    ['o'  - 0x20] = {0,0,0x3C,0x66,0x66,0x66,0x66,0x3C},
    ['p'  - 0x20] = {0,0,0x7C,0x66,0x66,0x7C,0x60,0x60},
    ['q'  - 0x20] = {0,0,0x3E,0x66,0x66,0x3E,0x06,0x06},
    ['r'  - 0x20] = {0,0,0x7C,0x66,0x60,0x60,0x60,0x60},
    ['s'  - 0x20] = {0,0,0x3E,0x60,0x3C,0x06,0x06,0x7C},
    ['t'  - 0x20] = {0x18,0x18,0x3C,0x18,0x18,0x18,0x18,0x1C},
    ['u'  - 0x20] = {0,0,0x66,0x66,0x66,0x66,0x66,0x3E},
    ['v'  - 0x20] = {0,0,0x66,0x66,0x66,0x66,0x3C,0x18},
    ['w'  - 0x20] = {0,0,0x66,0x66,0x66,0x6E,0x7E,0x36},
    ['x'  - 0x20] = {0,0,0x66,0x3C,0x18,0x18,0x3C,0x66},
    ['y'  - 0x20] = {0,0,0x66,0x66,0x66,0x3E,0x0C,0x78},
    ['z'  - 0x20] = {0,0,0x7E,0x0C,0x18,0x30,0x60,0x7E},
};

#define FG  0xFFFF   // white
#define BG  0x0000   // black

#define INFO_MAX_LINES   16
#define INFO_LINE_BYTES  80   // generous: viewport_name is 64 chars max

static uint16_t           *s_fb;            // PSRAM scratch at PANEL_W × PANEL_H
static esp_timer_handle_t  s_overlay_timer;
static volatile bool       s_overlay_active;

static void clear(uint16_t color)
{
    for (int i = 0; i < MAX_BUF_PX; ++i) s_fb[i] = color;
}

// Draw a single 8x8 glyph at (ox, oy) into the effective-dim buffer with
// integer scale. Pixels outside the buffer are clipped.
static void draw_char(uint16_t fb_w, uint16_t fb_h, int ox, int oy,
                      char c, int scale)
{
    if (c < 0x20 || c >= 0x7F) c = ' ';
    const uint8_t *g = FONT[c - 0x20];
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col)) {
                for (int dy = 0; dy < scale; ++dy) {
                    int y = oy + row * scale + dy;
                    if (y < 0 || y >= fb_h) continue;
                    for (int dx = 0; dx < scale; ++dx) {
                        int x = ox + col * scale + dx;
                        if (x < 0 || x >= fb_w) continue;
                        s_fb[(size_t)y * fb_w + x] = FG;
                    }
                }
            }
        }
    }
}

static void draw_centered(uint16_t fb_w, uint16_t fb_h, int y,
                          const char *s, int scale)
{
    int n = (int)strlen(s);
    int char_w = 8 * scale;
    int total  = n * char_w;
    int x = (fb_w - total) / 2;
    for (int i = 0; i < n; ++i) {
        draw_char(fb_w, fb_h, x + i * char_w, y, s[i], scale);
    }
}

// Draw a left-aligned line at (ox, oy).
static void draw_left(uint16_t fb_w, uint16_t fb_h, int ox, int oy,
                      const char *s, int scale)
{
    int n = (int)strlen(s);
    int char_w = 8 * scale;
    for (int i = 0; i < n; ++i) {
        draw_char(fb_w, fb_h, ox + i * char_w, oy, s[i], scale);
    }
}

static void overlay_expired_cb(void *arg);

esp_err_t local_screens_init(void)
{
    s_fb = (uint16_t *)heap_caps_malloc(MAX_BUF_PX * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "PSRAM alloc failed for scratch FB");
        return ESP_ERR_NO_MEM;
    }
    esp_timer_create_args_t args = {
        .callback = &overlay_expired_cb,
        .name     = "info_overlay",
    };
    esp_timer_create(&args, &s_overlay_timer);
    return ESP_OK;
}

static void overlay_expired_cb(void *arg)
{
    s_overlay_active = false;
    // Backlight off after the overlay. Whether the device is UNCONFIGURED
    // or ASLEEP, the result is the same: dark panel until next interaction.
    // For AWAKE we leave the backlight on — Scrypted's next /frame
    // overwrites the overlay anyway.
    viewport_state_lock();
    viewport_run_state_t s = viewport_state_get()->state;
    viewport_state_unlock();
    if (s != VIEWPORT_STATE_AWAKE && display_is_up()) {
        display_sleep();
    }
}

esp_err_t local_screens_overlay(uint32_t duration_ms)
{
    if (!display_is_up()) return ESP_ERR_INVALID_STATE;
    esp_timer_stop(s_overlay_timer);
    display_wake();
    esp_err_t err = local_screens_show_info();
    if (err != ESP_OK) return err;
    s_overlay_active = true;
    return esp_timer_start_once(s_overlay_timer,
                                (uint64_t)duration_ms * 1000ULL);
}

bool local_screens_overlay_active(void)
{
    return s_overlay_active;
}

void local_screens_overlay_dismiss(void)
{
    esp_timer_stop(s_overlay_timer);
    s_overlay_active = false;
    if (display_is_up()) display_sleep();
}

esp_err_t local_screens_show_info(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;

    // Snapshot every interesting value under a single lock acquisition so
    // the render reflects a consistent point in time. The big buffers live
    // in BSS so the call doesn't blow the caller's stack — the touch task
    // is on a 3 KiB stack and can't carry ~1.6 KiB of locals here. Single
    // caller protected by display ownership, so static is fine.
    static char vp_name[64];
    static char scrypt[256];
    static char lines[INFO_MAX_LINES][INFO_LINE_BYTES];
    bool configured;
    viewport_run_state_t state;
    viewport_orientation_t orient;
    uint8_t bright;
    uint32_t idle_ms;
    uint64_t uptime_ms;
    uint64_t frames, decode_err, post_err;

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    strncpy(vp_name, st->viewport_name, sizeof(vp_name));
    strncpy(scrypt,  st->scrypted_url,  sizeof(scrypt));
    vp_name[sizeof(vp_name) - 1] = '\0';
    scrypt[sizeof(scrypt)   - 1] = '\0';
    configured = st->configured;
    state      = st->state;
    orient     = st->orientation;
    bright     = st->brightness;
    idle_ms    = st->idle_timeout_ms;
    uptime_ms  = ((uint64_t)esp_timer_get_time() - st->boot_us) / 1000;
    frames     = st->frames_received;
    decode_err = st->decode_errors;
    post_err   = st->state_post_failures;
    viewport_state_unlock();

    const char *ip = net_eth_get_ip_str();
    uint32_t free_heap  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // ----- build label/value lines as "label value", left-aligned -----
    int n_lines = 0;

    // Capped at 32 chars so it doesn't blow past INFO_LINE_BYTES once the
    // 8-char label is prefixed. Long names get visibly truncated; ok for
    // an info dump on a 480-wide screen.
    char host[32];
    if (vp_name[0]) snprintf(host, sizeof(host), "viewport-%.15s.local", vp_name);
    else            snprintf(host, sizeof(host), "viewport.local");

    char scrypt_short[32];
    if (!scrypt[0]) {
        snprintf(scrypt_short, sizeof(scrypt_short), "none");
    } else {
        // Skip "http://" prefix to recover ~7 chars, then truncate.
        const char *s = scrypt;
        if (strncmp(s, "http://", 7) == 0) s += 7;
        size_t len = strlen(s);
        if (len > sizeof(scrypt_short) - 4) {
            snprintf(scrypt_short, sizeof(scrypt_short), "%.*s...",
                     (int)(sizeof(scrypt_short) - 5), s);
        } else {
            // Cap with precision so GCC can prove the snprintf bound.
            snprintf(scrypt_short, sizeof(scrypt_short), "%.*s",
                     (int)(sizeof(scrypt_short) - 1), s);
        }
    }

    char idle_str[16];
    if (idle_ms == 0) snprintf(idle_str, sizeof(idle_str), "off");
    else              snprintf(idle_str, sizeof(idle_str), "%us",
                               (unsigned)(idle_ms / 1000));

    // Compact "h:mm:ss" / "m:ss" uptime
    char up_str[20];
    uint64_t up_s = uptime_ms / 1000;
    unsigned hh = (unsigned)(up_s / 3600),
             mm = (unsigned)((up_s % 3600) / 60),
             ss = (unsigned)(up_s % 60);
    if (hh) snprintf(up_str, sizeof(up_str), "%u:%02u:%02u", hh, mm, ss);
    else    snprintf(up_str, sizeof(up_str), "%u:%02u",         mm, ss);

    // Compact byte counts with k / M suffix
    char heap_str[12], psram_str[12];
    #define FMT_BYTES(buf, n) do { \
        if      ((n) >= 1000000) snprintf((buf), sizeof(buf), "%uM", (unsigned)((n)/1000000)); \
        else if ((n) >= 1000)    snprintf((buf), sizeof(buf), "%uk", (unsigned)((n)/1000));    \
        else                     snprintf((buf), sizeof(buf), "%u",  (unsigned)(n));           \
    } while (0)
    FMT_BYTES(heap_str,  free_heap);
    FMT_BYTES(psram_str, free_psram);
    #undef FMT_BYTES

    const char *state_str = (state == VIEWPORT_STATE_AWAKE) ? "awake" : "asleep";

    // Label width is fixed at 8 chars (trailing spaces pad it). Values are
    // left-aligned at column 8. Auto-scaler then picks a font scale to fit.
    #define ADD(fmt, ...) do { \
        if (n_lines < INFO_MAX_LINES) \
            snprintf(lines[n_lines++], INFO_LINE_BYTES, fmt, ##__VA_ARGS__); \
    } while (0)

    ADD("name    %s",  vp_name[0] ? vp_name : "unset");
    ADD("host    %s",  host);
    ADD("ip      %s",  (ip && ip[0]) ? ip : "no network");
    ADD("state   %s",  state_str);
    ADD("config  %s",  configured ? "yes" : "no");
    ADD("scrypt  %s",  scrypt_short);
    ADD("orient  %s",  (orient == VIEWPORT_ORIENTATION_LANDSCAPE) ? "landscape" : "portrait");
    ADD("bright  %u",  (unsigned)bright);
    ADD("idle    %s",  idle_str);
    ADD("fw      %s",  VIEWPORT_VERSION);
    ADD("up      %s",  up_str);
    ADD("frames  %llu", (unsigned long long)frames);
    ADD("errs    %llu", (unsigned long long)(decode_err + post_err));
    ADD("heap    %s",  heap_str);
    ADD("psram   %s",  psram_str);

    #undef ADD

    // Auto-scale: largest scale where the longest line fits within 90% of
    // width and all lines + spacing fit within 90% of height.
    uint16_t w, h;
    viewport_state_effective_dims(&w, &h);

    size_t longest = 0;
    for (int i = 0; i < n_lines; ++i) {
        size_t l = strlen(lines[i]);
        if (l > longest) longest = l;
    }

    int scale = 1;
    for (int s = 6; s >= 1; --s) {
        int line_w  = (int)longest * 8 * s;
        int total_h = n_lines * 8 * s + (n_lines - 1) * 4 * s;
        if (line_w <= (w * 9) / 10 && total_h <= (h * 9) / 10) {
            scale = s;
            break;
        }
    }

    int line_h  = 8 * scale;
    int spacing = 4 * scale;
    int total_h = n_lines * line_h + (n_lines - 1) * spacing;
    int y0      = (h - total_h) / 2;
    int x0      = (w - (int)longest * 8 * scale) / 2;

    clear(BG);
    for (int i = 0; i < n_lines; ++i) {
        draw_left(w, h, x0, y0 + i * (line_h + spacing), lines[i], scale);
    }
    return display_present_rgb565(s_fb, w, h);
}

esp_err_t local_screens_show_loading(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;

    uint16_t w, h;
    viewport_state_effective_dims(&w, &h);
    int scale = (w < 800) ? 4 : 5;
    int line_h = 8 * scale;
    int y = (h - line_h) / 2;

    clear(BG);
    draw_centered(w, h, y, "Loading...", scale);

    return display_present_rgb565(s_fb, w, h);
}
