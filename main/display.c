#include "display.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
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
#define PANEL_HSYNC_PULSE     18
#define PANEL_HSYNC_BP        20
#define PANEL_HSYNC_FP        62
#define PANEL_VSYNC_PULSE     4
#define PANEL_VSYNC_BP        27
#define PANEL_VSYNC_FP        18
#define PANEL_PIXEL_CLOCK_MHZ 30
#define DSI_LANE_RATE_MBPS    480
#define DSI_NUM_DATA_LANES    2

// ============================================================================
// Module state
// ============================================================================
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_panel_mcu;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_handle_t   s_panel;
static bool                     s_up;
static uint8_t                  s_last_pwm;

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
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_POWERON, 1), TAG, "POWERON=1");
    vTaskDelay(pdMS_TO_TICKS(PANEL_POWERON_DELAY_MS));
    ESP_LOGI(TAG, "panel powered on");
    return ESP_OK;
}

// ============================================================================
// DSI bring-up (DPI video mode — Pi-style panels need no DSI command init)
// ============================================================================
static esp_err_t dsi_bring_up(void)
{
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_NUM_DATA_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus),
                       TAG, "esp_lcd_new_dsi_bus");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_PIXEL_CLOCK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
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
        .flags.use_dma2d = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_panel),
                       TAG, "esp_lcd_new_panel_dpi");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");

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

    s_up = true;

    // Default brightness from shared state (set by /config in M4; 80 on
    // first boot).
    viewport_state_lock();
    uint8_t b = viewport_state_get()->brightness;
    viewport_state_unlock();
    display_set_brightness(b);

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
    return mcu_write_u8(REG_PWM, 0);
}

esp_err_t display_wake(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    return mcu_write_u8(REG_PWM, s_last_pwm);
}

esp_err_t display_fill(uint16_t rgb565)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb),
                       TAG, "get frame buffer");
    uint16_t *px = (uint16_t *)fb;
    size_t n = PANEL_H_ACTIVE * PANEL_V_ACTIVE;
    for (size_t i = 0; i < n; ++i) px[i] = rgb565;
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE, fb);
}

esp_err_t display_present_rgb565(const uint16_t *src,
                                 uint16_t        src_w,
                                 uint16_t        src_h)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;

    viewport_state_lock();
    viewport_orientation_t orient = viewport_state_get()->orientation;
    viewport_state_unlock();

    void *fb = NULL;
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb);
    if (err != ESP_OK) return err;
    uint16_t *dst = (uint16_t *)fb;

    if (orient == VIEWPORT_ORIENTATION_LANDSCAPE) {
        if (src_w != PANEL_H_ACTIVE || src_h != PANEL_V_ACTIVE)
            return ESP_ERR_INVALID_SIZE;
        memcpy(dst, src, (size_t)src_w * src_h * sizeof(uint16_t));
    } else {
        // Portrait: src is 480x800, rotate 90° CW into the 800x480 panel.
        // dst dims = src_h x src_w. dst_stride = src_h.
        // src(x,y) -> dst(x, src_h - 1 - y)
        if (src_w != PANEL_V_ACTIVE || src_h != PANEL_H_ACTIVE)
            return ESP_ERR_INVALID_SIZE;
        const uint16_t dst_stride = src_h;
        for (uint16_t y = 0; y < src_h; ++y) {
            const uint16_t *srow = src + (size_t)y * src_w;
            const uint16_t dst_col = (uint16_t)(src_h - 1 - y);
            for (uint16_t x = 0; x < src_w; ++x) {
                dst[(size_t)x * dst_stride + dst_col] = srow[x];
            }
        }
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE, fb);
}

esp_err_t display_test_pattern(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb),
                       TAG, "get frame buffer");
    uint16_t *px = (uint16_t *)fb;

    // 8 vertical color bars: white, yellow, cyan, green, magenta, red, blue, black.
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
        0xF81F, 0xF800, 0x001F, 0x0000,
    };
    const int bar_w = PANEL_H_ACTIVE / 8;
    for (int y = 0; y < PANEL_V_ACTIVE; ++y) {
        for (int x = 0; x < PANEL_H_ACTIVE; ++x) {
            int b = x / bar_w;
            if (b > 7) b = 7;
            px[y * PANEL_H_ACTIVE + x] = bars[b];
        }
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE, fb);
}
