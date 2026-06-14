#include "local_screens.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "display.h"
#include "net_eth.h"
#include "viewport_state.h"

static const char *TAG = "screens";

#define PANEL_W      800
#define PANEL_H      480
#define MAX_BUF_PX   (PANEL_W * PANEL_H)

// 8x8 bitmap font, lit pixels = foreground. Only the characters used by the
// IP and Loading screens are defined; the rest stay zero ('?' shows up as a
// blank box for unsupported characters). Designators keep the data sparse-
// looking but the table is just a contiguous block (95 chars × 8 bytes).
static const uint8_t FONT[95][8] = {
    [' '  - 0x20] = {0,0,0,0,0,0,0,0},
    ['.'  - 0x20] = {0,0,0,0,0,0x18,0x18,0},
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
    ['c'  - 0x20] = {0,0,0x3C,0x66,0x60,0x60,0x66,0x3C},
    ['d'  - 0x20] = {0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x3E},
    ['e'  - 0x20] = {0,0,0x3C,0x66,0x7E,0x60,0x66,0x3C},
    ['g'  - 0x20] = {0,0,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    ['i'  - 0x20] = {0x18,0,0x18,0x18,0x18,0x18,0x18,0x18},
    ['l'  - 0x20] = {0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x3C},
    ['n'  - 0x20] = {0,0,0x7C,0x66,0x66,0x66,0x66,0x66},
    ['o'  - 0x20] = {0,0,0x3C,0x66,0x66,0x66,0x66,0x3C},
    ['p'  - 0x20] = {0,0,0x7C,0x66,0x66,0x7C,0x60,0x60},
    ['r'  - 0x20] = {0,0,0x7C,0x66,0x60,0x60,0x60,0x60},
    ['t'  - 0x20] = {0x18,0x18,0x3C,0x18,0x18,0x18,0x18,0x1C},
    ['v'  - 0x20] = {0,0,0x66,0x66,0x66,0x66,0x3C,0x18},
    ['w'  - 0x20] = {0,0,0x66,0x66,0x66,0x6E,0x7E,0x36},
};

#define FG  0xFFFF   // white
#define BG  0x0000   // black

static uint16_t *s_fb;          // PSRAM scratch at PANEL_W × PANEL_H

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

esp_err_t local_screens_init(void)
{
    s_fb = (uint16_t *)heap_caps_malloc(MAX_BUF_PX * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "PSRAM alloc failed for scratch FB");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t local_screens_show_ip(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;

    uint16_t w, h;
    effective_dims(&w, &h);
    int scale = (w < 800) ? 3 : 4;   // 3x for portrait, 4x for landscape
    int line_h = 8 * scale;
    int spacing = line_h / 2;
    int total_h = 2 * line_h + spacing;
    int y0 = (h - total_h) / 2;

    clear(BG);
    draw_centered(w, h, y0,                       "viewport.local", scale);
    draw_centered(w, h, y0 + line_h + spacing,    net_eth_get_ip_str(), scale);

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

esp_err_t local_screens_restore_for_state(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;

    viewport_state_lock();
    viewport_run_state_t s = viewport_state_get()->state;
    viewport_state_unlock();

    if (s == VIEWPORT_STATE_UNCONFIGURED) return local_screens_show_ip();

    // Awake or asleep: clear the FB to black. For asleep the backlight is
    // off anyway; for awake the next /frame will overwrite.
    uint16_t w, h;
    effective_dims(&w, &h);
    clear(BG);
    return display_present_rgb565(s_fb, w, h);
}
