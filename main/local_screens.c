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

static uint16_t           *s_fb;            // PSRAM scratch at PANEL_W × PANEL_H
static esp_timer_handle_t  s_overlay_timer;
static volatile bool       s_overlay_active;

static void clear(uint16_t color)
{
    for (int i = 0; i < MAX_BUF_PX; ++i) s_fb[i] = color;
}

// Get current effective dimensions (rotated dims for portrait).
static void effective_dims(uint16_t *w, uint16_t *h)
{
    viewport_state_lock();
    viewport_orientation_t o = viewport_state_get()->orientation;
    viewport_state_unlock();
    if (o == VIEWPORT_ORIENTATION_PORTRAIT) { *w = 480; *h = 800; }
    else                                    { *w = 800; *h = 480; }
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
        .name     = "ident_overlay",
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
    esp_err_t err = local_screens_show_ip();
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

esp_err_t local_screens_show_ip(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;

    // Build the four identity lines from current state. This is the screen
    // shown on first boot, after factory reset, and as a 15s BOOT-button
    // overlay — it's the "who am I" view the operator needs to find the
    // device on the LAN and confirm it's configured the way they expect.
    //
    //   line 1: viewport name      ("mudroom"   / "viewport" if unconfigured)
    //   line 2: mDNS hostname      ("viewport-mudroom.local" or "viewport.local")
    //   line 3: IP address         ("192.168.x.y" or "no network")
    //   line 4: state              ("awake" / "asleep" / "unconfigured")

    char line_name[64], line_host[80], line_ip[24], line_state[24];

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    if (st->viewport_name[0]) {
        snprintf(line_name, sizeof(line_name), "%s", st->viewport_name);
        snprintf(line_host, sizeof(line_host), "viewport-%s.local", st->viewport_name);
    } else {
        snprintf(line_name, sizeof(line_name), "viewport");
        snprintf(line_host, sizeof(line_host), "viewport.local");
    }
    switch (st->state) {
    case VIEWPORT_STATE_AWAKE:        snprintf(line_state, sizeof(line_state), "awake"); break;
    case VIEWPORT_STATE_ASLEEP:       snprintf(line_state, sizeof(line_state), "asleep"); break;
    default:                          snprintf(line_state, sizeof(line_state), "unconfigured"); break;
    }
    viewport_state_unlock();

    const char *ip = net_eth_get_ip_str();
    snprintf(line_ip, sizeof(line_ip), "%s", (ip && ip[0]) ? ip : "no network");

    const char *lines[] = { line_name, line_host, line_ip, line_state };
    const int   n_lines = (int)(sizeof(lines) / sizeof(lines[0]));

    uint16_t w, h;
    effective_dims(&w, &h);

    // Pick the largest integer scale where the longest line fits within
    // 90% of the screen width and all four lines plus inter-line spacing
    // (half a line each) fit within 90% of the screen height. Falls back
    // to scale 1 if the longest line is unusually long.
    size_t longest = 0;
    for (int i = 0; i < n_lines; ++i) {
        size_t l = strlen(lines[i]);
        if (l > longest) longest = l;
    }
    int scale = 1;
    for (int s = 6; s >= 1; --s) {
        int line_w = (int)longest * 8 * s;
        int total_h = n_lines * 8 * s + (n_lines - 1) * 4 * s;
        if (line_w <= (w * 9) / 10 && total_h <= (h * 9) / 10) {
            scale = s;
            break;
        }
    }

    int line_h = 8 * scale;
    int spacing = 4 * scale;
    int total_h = n_lines * line_h + (n_lines - 1) * spacing;
    int y0 = (h - total_h) / 2;

    clear(BG);
    for (int i = 0; i < n_lines; ++i) {
        draw_centered(w, h, y0 + i * (line_h + spacing), lines[i], scale);
    }
    return display_present_rgb565(s_fb, w, h);
}

esp_err_t local_screens_show_loading(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;

    uint16_t w, h;
    effective_dims(&w, &h);
    int scale = (w < 800) ? 4 : 5;
    int line_h = 8 * scale;
    int y = (h - line_h) / 2;

    clear(BG);
    draw_centered(w, h, y, "Loading...", scale);

    return display_present_rgb565(s_fb, w, h);
}

