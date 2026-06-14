#include "display.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "viewport_state.h"

static const char *TAG = "display";

// ============================================================================
// Pin / bus assignments
//
// TODO confirm against the Waveshare ESP32-P4-ETH schematic and the
// 15-pin Pi FPC → 22-pin Waveshare DSI adapter. The two I2C lines below
// must reach the Hosyond panel's 15-pin FPC at the standard Pi positions.
// Waveshare's bundled-panel BSP uses these GPIOs for touch I2C; whether
// they also carry through their DSI connector to the FPC is unconfirmed.
// ============================================================================
#define PIN_I2C_SDA           7
#define PIN_I2C_SCL           8
#define I2C_PORT              I2C_NUM_0
#define I2C_FREQ_HZ           100000      // panel MCU is slow; 100kHz is safe

// Panel I2C addresses (Pi 7" touchscreen architecture, replicated by
// Hosyond/Elecrow/etc. clones for dtoverlay=vc4-kms-dsi-7inch).
#define PANEL_MCU_ADDR        0x45
#define TOUCH_FT5426_ADDR     0x38

// Pi panel MCU register map. See Linux:
//   drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.c
enum {
    REG_ID       = 0x80,   // reads back 0xC3 on a healthy panel
    REG_PORTA    = 0x81,
    REG_PORTB    = 0x82,
    REG_PORTC    = 0x83,
    REG_PORTD    = 0x84,
    REG_POWERON  = 0x85,   // write 1 to power up; 0 to power down
    REG_PWM      = 0x86,   // 0..255 backlight duty
    REG_ADDR_H   = 0x87,
    REG_ADDR_L   = 0x88,
    REG_WRITE_H  = 0x89,
    REG_WRITE_L  = 0x8a,
    REG_READ_H   = 0x8b,
    REG_READ_L   = 0x8c,
};

#define PANEL_ID_EXPECTED     0xC3
#define PANEL_POWERON_DELAY_MS 120  // empirically ~80ms; pad to be safe

// ============================================================================
// DSI / DPI panel timing for 800x480 @ 60Hz
// (Raspberry Pi 7" canonical timings — same panel architecture as Hosyond 5")
// ============================================================================
#define PANEL_H_ACTIVE        800
#define PANEL_V_ACTIVE        480
// Pi 7" canonical timings from the upstream RPi DTS panel binding.
// Hosyond 5" panels are TC358762-based clones and use the same.
#define PANEL_HSYNC_PULSE     1
#define PANEL_HSYNC_BP        46
#define PANEL_HSYNC_FP        16
#define PANEL_VSYNC_PULSE     1
#define PANEL_VSYNC_BP        33
#define PANEL_VSYNC_FP        7
#define PANEL_PIXEL_CLOCK_MHZ 25
// Lane bit rate. ESP32-P4 MIPI-DSI PHY PLL is picky about valid lock
// frequencies; 480 Mbps caused the PLL-lock busy-wait inside
// esp_lcd_new_dsi_bus() to hang indefinitely. 1000 Mbps matches the
// reference example and is well within the TC358762 bridge's max.
// Pi 7"-style panels via the 15-pin FPC traditionally use a single DSI
// data lane; the second data pair in the FPC stays unused. The DSI link
// rate is bumped to compensate (PLL-valid: 720/20 = 36 even).
#define DSI_LANE_RATE_MBPS    720
#define DSI_NUM_DATA_LANES    1

// The ESP32-P4 MIPI-DSI PHY needs VDD_MIPI_DPHY = 2.5V powered before its
// PLL can lock. Internal LDO_VO3 (channel 3) is the source on this SoC,
// per ESP-IDF's mipi_dsi example. Without this the PHY-lock busy-wait
// inside esp_lcd_new_dsi_bus() spins forever.
#define DSI_PHY_LDO_CHAN      3
#define DSI_PHY_LDO_MV        2500

// ============================================================================
// Module state
// ============================================================================
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_panel_mcu;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_handle_t   s_panel;
static bool                     s_up;
static uint8_t                  s_last_pwm;
// RGB888 scratch buffer used as the source to draw_bitmap. 3 bytes/pixel
// matches the DPI driver's framebuffer stride; landscape and portrait
// both go through here so we get a single consistent expand-from-RGB565
// path. 800 * 480 * 3 = ~1.15 MB in PSRAM.
static uint8_t                 *s_rot_buf;

// ============================================================================
// Panel-MCU I2C helpers
// ============================================================================
static esp_err_t mcu_write_u8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_panel_mcu, buf, sizeof(buf), 50);
}

static esp_err_t mcu_read_u8(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_panel_mcu, &reg, 1, out, 1, 50);
}

static esp_err_t panel_mcu_attach(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_PORT,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus),
                       TAG, "i2c_new_master_bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PANEL_MCU_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_panel_mcu),
        TAG, "i2c add device 0x45");

    uint8_t id = 0;
    esp_err_t err = mcu_read_u8(REG_ID, &id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel MCU @0x45 unreachable (%s) — check FPC + adapter",
                 esp_err_to_name(err));
        return err;
    }
    if (id != PANEL_ID_EXPECTED) {
        ESP_LOGW(TAG, "panel MCU id 0x%02x (expected 0x%02x) — wiring "
                      "looks ok but panel might be a different rev",
                 id, PANEL_ID_EXPECTED);
        // Continue anyway; some clones report different IDs.
    } else {
        ESP_LOGI(TAG, "panel MCU id 0x%02x — Pi 7\" architecture ack'd", id);
    }
    return ESP_OK;
}

static esp_err_t panel_power_on(void)
{
    // Full Pi 7" enable sequence, mirroring Linux's panel-raspberrypi-
    // touchscreen.c. Without the PORTA/PORTB/PORTC writes the TC358762
    // converts DSI to DPI fine, but the ATTINY never enables the actual
    // panel chip + backlight — so the panel stays dark.

    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_POWERON, 1), TAG, "POWERON=1");
    vTaskDelay(pdMS_TO_TICKS(PANEL_POWERON_DELAY_MS));

    // Optional: poll REG_PORTB bit 0 for nPWRDWN going high. Bounded
    // wait so we don't hang on a non-responsive panel.
    for (int i = 0; i < 100; ++i) {
        uint8_t portb = 0;
        if (mcu_read_u8(REG_PORTB, &portb) == ESP_OK && (portb & 0x01)) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Enable the panel chip via the ATTINY's GPIO expanders. Values match
    // the upstream Linux driver and the original Pi 7" closed-source FW.
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTA, 0x04), TAG, "PORTA");
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTB, 0x00), TAG, "PORTB");
    // Leave PWM at 0 (backlight off) — the boot sequence calls
    // display_sleep() after init; first interaction (tap, BOOT, /state)
    // brings the backlight up via display_wake().
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PWM,   0x00), TAG, "PWM init");
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTC, 0x01), TAG, "PORTC");

    ESP_LOGI(TAG, "panel powered on (PORTA/B/C + PWM init)");
    return ESP_OK;
}

// ============================================================================
// DSI bring-up (DPI video mode — Pi-style panels need no DSI command init)
// ============================================================================
static esp_err_t dsi_bring_up(void)
{
    // 1. Power up the DSI PHY (VDD_MIPI_DPHY 2.5V) before any DSI calls.
    esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr),
                       TAG, "DSI PHY LDO acquire");
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO%d @ %dmV)",
             DSI_PHY_LDO_CHAN, DSI_PHY_LDO_MV);

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_NUM_DATA_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus),
                       TAG, "esp_lcd_new_dsi_bus");

    // DBI IO handle — the IDF reference example creates this even for
    // panels that don't use DSI commands. Some DSI bridge state machines
    // need the command-channel side initialized before video mode runs.
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    esp_lcd_panel_io_handle_t dbi_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &dbi_io),
                       TAG, "esp_lcd_new_panel_io_dbi");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_PIXEL_CLOCK_MHZ,
        // TC358762 only decodes 24-bit DSI Video Mode packets. Keep the
        // entire pipeline RGB888 so the framebuffer stride (3 B/pixel)
        // matches what the DSI engine sends on the wire. Our internal
        // text/JPEG rendering still produces RGB565; the rotation step
        // in display_present_rgb565() expands to RGB888 inline.
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .in_color_format    = LCD_COLOR_FMT_RGB888,
        .out_color_format   = LCD_COLOR_FMT_RGB888,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = PANEL_H_ACTIVE,
            .v_size            = PANEL_V_ACTIVE,
            .hsync_pulse_width = PANEL_HSYNC_PULSE,
            .hsync_back_porch  = PANEL_HSYNC_BP,
            .hsync_front_porch = PANEL_HSYNC_FP,
            .vsync_pulse_width = PANEL_VSYNC_PULSE,
            .vsync_back_porch  = PANEL_VSYNC_BP,
            .vsync_front_porch = PANEL_VSYNC_FP,
        },
        .flags.use_dma2d = false,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_panel),
                       TAG, "esp_lcd_new_panel_dpi");
    // No esp_lcd_panel_reset() — the IDF DPI driver doesn't implement it
    // (returns NOT_SUPPORTED). It only exists for panels with vendor
    // drivers that toggle a hardware reset pin via DSI commands.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "panel_init");

    ESP_LOGI(TAG, "DSI up: %dx%d %d MHz, %d-lane %d Mbps",
             PANEL_H_ACTIVE, PANEL_V_ACTIVE, PANEL_PIXEL_CLOCK_MHZ,
             DSI_NUM_DATA_LANES, DSI_LANE_RATE_MBPS);
    return ESP_OK;
}

// ============================================================================
// Public API
// ============================================================================
esp_err_t display_init(void)
{
    if (s_up) return ESP_OK;

    ESP_RETURN_ON_ERROR(panel_mcu_attach(), TAG, "panel MCU attach");
    ESP_RETURN_ON_ERROR(panel_power_on(),  TAG, "panel power-on");
    ESP_RETURN_ON_ERROR(dsi_bring_up(),    TAG, "DSI bring-up");

    // PSRAM scratch (panel-native 800x480 RGB888 = 3 bytes/pixel).
    s_rot_buf = (uint8_t *)heap_caps_malloc(
        PANEL_H_ACTIVE * PANEL_V_ACTIVE * 3,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rot_buf) {
        ESP_LOGE(TAG, "rotation scratch alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_up = true;

    // Cache the brightness value but do NOT push it to the panel yet.
    // The device boots with backlight off; first wake (BOOT short-press,
    // tap, or POST /state {wake}) applies the brightness.
    viewport_state_lock();
    s_last_pwm = viewport_state_get()->brightness;
    viewport_state_unlock();
    mcu_write_u8(REG_PWM, 0);

    return ESP_OK;
}

bool display_is_up(void) { return s_up; }

i2c_master_bus_handle_t display_i2c_bus(void) { return s_i2c_bus; }

esp_err_t display_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    // Perceptual: duty = (pct/100)^2.2 * 255.
    float frac = (float)pct / 100.0f;
    float gamma = powf(frac, 2.2f);
    uint8_t duty = (uint8_t)(gamma * 255.0f + 0.5f);

    s_last_pwm = duty;
    if (!s_up) return ESP_ERR_INVALID_STATE;
    return mcu_write_u8(REG_PWM, duty);
}

esp_err_t display_sleep(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    esp_err_t err = mcu_write_u8(REG_PWM, 0);
    ESP_LOGI(TAG, "sleep: PWM=0 (%s)", esp_err_to_name(err));
    return err;
}

esp_err_t display_wake(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    esp_err_t err = mcu_write_u8(REG_PWM, s_last_pwm);
    ESP_LOGI(TAG, "wake: PWM=%u (%s)", s_last_pwm, esp_err_to_name(err));
    return err;
}

// Convert a 16-bit RGB565 to three bytes in B, G, R order.
// The TC358762 → Pi-style panel pipeline expects BGR on the DPI wire,
// matching the panel chip's native byte order. Swapping to RGB produced
// garbled vertical bands; BGR aligns the pixel layout.
static inline void rgb565_to_rgb888(uint16_t px, uint8_t *out)
{
    out[0] = (uint8_t)(( px        & 0x1F) * 255 / 31);  // B
    out[1] = (uint8_t)(((px >> 5)  & 0x3F) * 255 / 63);  // G
    out[2] = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);  // R
}

esp_err_t display_fill(uint16_t rgb565)
{
    if (!s_up || !s_rot_buf) return ESP_ERR_INVALID_STATE;
    uint8_t rgb[3];
    rgb565_to_rgb888(rgb565, rgb);
    size_t n = PANEL_H_ACTIVE * PANEL_V_ACTIVE;
    for (size_t i = 0; i < n; ++i) {
        s_rot_buf[i * 3 + 0] = rgb[0];
        s_rot_buf[i * 3 + 1] = rgb[1];
        s_rot_buf[i * 3 + 2] = rgb[2];
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE,
                                     s_rot_buf);
}

esp_err_t display_present_rgb565(const uint16_t *src,
                                 uint16_t        src_w,
                                 uint16_t        src_h)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;

    viewport_state_lock();
    viewport_orientation_t orient = viewport_state_get()->orientation;
    viewport_state_unlock();

    if (!s_rot_buf) return ESP_ERR_INVALID_STATE;

    if (orient == VIEWPORT_ORIENTATION_LANDSCAPE) {
        if (src_w != PANEL_H_ACTIVE || src_h != PANEL_V_ACTIVE)
            return ESP_ERR_INVALID_SIZE;
        // RGB565 → RGB888, no rotation.
        size_t n = (size_t)src_w * src_h;
        for (size_t i = 0; i < n; ++i) {
            rgb565_to_rgb888(src[i], &s_rot_buf[i * 3]);
        }
    } else {
        // Portrait: src is 480x800; rotate 90° CW into 800x480 panel
        // coordinates and expand to RGB888 in the same pass.
        if (src_w != PANEL_V_ACTIVE || src_h != PANEL_H_ACTIVE)
            return ESP_ERR_INVALID_SIZE;
        const uint16_t dst_stride = src_h;  // 800 panel-rows of 3 bytes each
        for (uint16_t y = 0; y < src_h; ++y) {
            const uint16_t *srow = src + (size_t)y * src_w;
            const uint16_t dst_col = (uint16_t)(src_h - 1 - y);
            for (uint16_t x = 0; x < src_w; ++x) {
                size_t dst_idx = ((size_t)x * dst_stride + dst_col) * 3;
                rgb565_to_rgb888(srow[x], &s_rot_buf[dst_idx]);
            }
        }
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE,
                                     s_rot_buf);
}

esp_err_t display_test_pattern(void)
{
    if (!s_up || !s_rot_buf) return ESP_ERR_INVALID_STATE;
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
        0xF81F, 0xF800, 0x001F, 0x0000,
    };
    const int bar_w = PANEL_H_ACTIVE / 8;
    for (int y = 0; y < PANEL_V_ACTIVE; ++y) {
        for (int x = 0; x < PANEL_H_ACTIVE; ++x) {
            int b = x / bar_w;
            if (b > 7) b = 7;
            size_t idx = ((size_t)y * PANEL_H_ACTIVE + x) * 3;
            rgb565_to_rgb888(bars[b], &s_rot_buf[idx]);
        }
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE,
                                     s_rot_buf);
}
