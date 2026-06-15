#include "display.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_types.h"

#include "viewport_state.h"

static const char *TAG = "display";

// ============================================================================
// Pin / bus assignments
// ============================================================================
#define PIN_I2C_SDA           7
#define PIN_I2C_SCL           8
#define I2C_PORT              I2C_NUM_0
#define I2C_FREQ_HZ           400000

#define PANEL_MCU_ADDR        0x45    // ATTINY88 on the Pi 7" panel architecture
#define TOUCH_FT5426_ADDR     0x38

// ATTINY88 register map. Source:
//   linux/drivers/regulator/rpi-panel-attiny-regulator.c (rpi-6.6.y)
enum {
    REG_ID         = 0x80,   // 0xC3 on v1.1 firmware
    REG_PORTA      = 0x81,
    REG_PORTB      = 0x82,
    REG_PORTC      = 0x83,
    REG_PWM        = 0x86,   // 0..255 backlight duty
    REG_ADDR_L     = 0x8c,   // TC358762 target addr low
    REG_ADDR_H     = 0x8d,   //   ...high
    REG_WR_DATA_H  = 0x90,   // TC358762 write data high
    REG_WR_DATA_L  = 0x91,   //   ...low
};

#define PA_LCD_LR         (1u << 2)  // horizontal scan direction
#define PB_LCD_MAIN       (1u << 7)  // main regulator enable
#define PC_LED_EN         (1u << 0)
#define PC_RST_TP_N       (1u << 1)
#define PC_RST_LCD_N      (1u << 2)
#define PC_RST_BRIDGE_N   (1u << 3)

#define PANEL_ID_EXPECTED 0xC3

// ============================================================================
// 800x480 timings. Constants copied verbatim from the embenix reference
// (rpi-7inch-touch-display-v1.c), which calls them "observed stable" on
// ESP32-P4. The Linux upstream modeline differs in HFP/VSW; if pixels look
// shifted left/right these are the first knobs to revisit.
// ============================================================================
#define PANEL_H_ACTIVE        800
#define PANEL_V_ACTIVE        480
#define RPI_HSW                2
#define RPI_HBP               46
#define RPI_HFP              210
#define RPI_VSW               20
#define RPI_VBP                4
#define RPI_VFP               22

#define PANEL_PIXEL_CLOCK_MHZ 26       // Linux modeline = 25.9794 MHz; field is integer MHz
#define DSI_LANE_RATE_MBPS    600      // embenix "observed stable" on ESP32-P4
#define DSI_NUM_DATA_LANES    1        // TC358762 Linux driver is single-lane

#define DSI_PHY_LDO_CHAN      3        // VDD_MIPI_DPHY rail on ESP32-P4
#define DSI_PHY_LDO_MV        2500

// ============================================================================
// TC358762 DSI-to-DPI bridge register map. Source:
//   linux/drivers/gpu/drm/bridge/tc358762.c (rpi-6.6.y) tc358762_init()
// Bridge is configured by sending DSI Generic Long Write (DT=0x29) packets
// to the link with a 6-byte payload: [reg_lo, reg_hi, val_b0..val_b3].
// ============================================================================
#define TC_DSI_LANEENABLE         0x0210  // CLEN | D0EN | D1EN
#define TC_PPI_D0S_CLRSIPOCOUNT   0x0164
#define TC_PPI_D1S_CLRSIPOCOUNT   0x0168
#define TC_PPI_D0S_ATMR           0x0144
#define TC_PPI_D1S_ATMR           0x0148
#define TC_PPI_LPTXTIMECNT        0x0114
#define TC_SPICMR                 0x0450
#define TC_LCDCTRL                0x0420
#define TC_SYSCTRL                0x0464
#define TC_LCD_HS_HBP             0x0424
#define TC_LCD_HDISP_HFP          0x0428
#define TC_LCD_VS_VBP             0x042c
#define TC_LCD_VDISP_VFP          0x0430
#define TC_PPI_STARTPPI           0x0104
#define TC_DSI_STARTDSI           0x0204

// Mirror of esp_lcd_dsi_bus_t (esp-idf/components/esp_lcd/dsi/mipi_dsi_priv.h)
// — needed to reach the HAL/LL APIs for the TC358762 bridge init.
typedef struct {
    int                    bus_id;
    mipi_dsi_hal_context_t hal;
} dsi_bus_shadow_t;

// ============================================================================
// Module state
// ============================================================================
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_panel_mcu;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_handle_t   s_panel;
static bool                     s_up;
static uint8_t                  s_last_pwm;
// BGR888 panel-sized scratch buffer used by local_screens' CPU
// conversion + rotation path (~1.15 MB in PSRAM). Hot /frame path
// instead writes the JPEG decoder output directly into the panel's
// double-buffered framebuffer (see s_panel_fbs).
static uint8_t                 *s_rot_buf;
// The DPI driver owns the two framebuffers when num_fbs = 2. We grab
// pointers to both after panel_init and alternate which one we hand to
// the JPEG decoder + the panel for each /frame. draw_bitmap with a
// pointer that's inside one of these turns into a cache writeback +
// index swap (microseconds), eliminating the previous ~24 ms memcpy.
static uint8_t                 *s_panel_fbs[2];
static int                      s_back_fb;     // index of the fb the next paint will fill

// ============================================================================
// ATTINY88 I2C helpers
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
        ESP_LOGE(TAG, "panel MCU @0x45 unreachable (%s)", esp_err_to_name(err));
        return err;
    }
    if (id != PANEL_ID_EXPECTED) {
        ESP_LOGW(TAG, "panel MCU id 0x%02x (expected 0x%02x)", id, PANEL_ID_EXPECTED);
    } else {
        ESP_LOGI(TAG, "panel MCU id 0x%02x — Pi v1.1 architecture ack'd", id);
    }
    return ESP_OK;
}

// ATTINY88 v1.1 power-on sequence. Mirrors attiny_lcd_power_enable() in
// drivers/regulator/rpi-panel-attiny-regulator.c. No REG_POWERON write —
// that legacy register only exists on the older 0xDE firmware. The bridge
// stays in reset at this point; it's released later in panel_bringup() once
// the DSI HS clock is running and ready to source LP commands.
static esp_err_t panel_power_on(void)
{
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTC, 0x00), TAG, "PORTC=0 (assert all resets)");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTA, PA_LCD_LR), TAG, "PORTA");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTB, PB_LCD_MAIN), TAG, "PORTB");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTC, PC_LED_EN), TAG, "PORTC=LED");
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PWM,   0x00), TAG, "PWM=0");
    ESP_LOGI(TAG, "panel power-on (v1.1 regulator protocol) complete; bridge held in reset");
    return ESP_OK;
}

// Release TC358762 reset and wake the bridge via the ATTINY's SPI proxy.
// Mirrors attiny_gpio_set(RST_BRIDGE_N=1) in the Linux driver, which writes
// TC358762 SYSPMCTRL (0x047c) = 0 to take the bridge out of low-power.
static esp_err_t bridge_release_and_wake(void)
{
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_PORTC,
        PC_LED_EN | PC_RST_LCD_N | PC_RST_BRIDGE_N), TAG, "PORTC bridge release");
    vTaskDelay(pdMS_TO_TICKS(10));

    // 16-bit SPI proxy write of SYSPMCTRL = 0x0000.
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_ADDR_H,    0x04), TAG, "ADDR_H");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_ADDR_L,    0x7c), TAG, "ADDR_L");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_WR_DATA_H, 0x00), TAG, "WR_DATA_H");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(mcu_write_u8(REG_WR_DATA_L, 0x00), TAG, "WR_DATA_L");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "bridge reset released, SYSPMCTRL=0 wake sent via ATTINY proxy");
    return ESP_OK;
}

static esp_err_t touch_release_reset(void)
{
    return mcu_write_u8(REG_PORTC,
        PC_LED_EN | PC_RST_TP_N | PC_RST_LCD_N | PC_RST_BRIDGE_N);
}

// ============================================================================
// TC358762 bridge configuration (DSI Generic Long Write packets)
// Sequence mirrors Linux drivers/gpu/drm/bridge/tc358762.c tc358762_init().
// ============================================================================
static void bridge_write(uint16_t reg, uint32_t val)
{
    dsi_bus_shadow_t *p = (dsi_bus_shadow_t *)s_dsi_bus;
    uint8_t payload[6] = {
        (uint8_t)(reg & 0xff), (uint8_t)(reg >> 8),
        (uint8_t)(val & 0xff), (uint8_t)((val >> 8)  & 0xff),
        (uint8_t)((val >> 16) & 0xff), (uint8_t)((val >> 24) & 0xff),
    };
    mipi_dsi_hal_host_gen_write_long_packet(&p->hal, 0,
        MIPI_DSI_DT_GENERIC_LONG_WRITE, payload, sizeof(payload));
}

static esp_err_t bridge_init(void)
{
    // 1-lane: CLEN | D0EN. Linux driver's TODO note confirms second lane
    // isn't supported by the bridge in this firmware.
    bridge_write(TC_DSI_LANEENABLE,        (1u << 0) | (1u << 1));
    bridge_write(TC_PPI_D0S_CLRSIPOCOUNT,  0x05);
    bridge_write(TC_PPI_D1S_CLRSIPOCOUNT,  0x05);
    bridge_write(TC_PPI_D0S_ATMR,          0x00);
    bridge_write(TC_PPI_D1S_ATMR,          0x00);
    bridge_write(TC_PPI_LPTXTIMECNT,       0x03);
    bridge_write(TC_SPICMR,                0x00);
    // LCDCTRL = VSDELAY=1 | RGB888 | UNK6 | VTGEN — bit pattern from Linux.
    bridge_write(TC_LCDCTRL,               0x00100150);
    bridge_write(TC_SYSCTRL,               0x040f);
    bridge_write(TC_LCD_HS_HBP,    ((uint32_t)RPI_HBP << 16) | RPI_HSW);
    bridge_write(TC_LCD_HDISP_HFP, ((uint32_t)RPI_HFP << 16) | PANEL_H_ACTIVE);
    bridge_write(TC_LCD_VS_VBP,    ((uint32_t)RPI_VBP << 16) | RPI_VSW);
    bridge_write(TC_LCD_VDISP_VFP, ((uint32_t)RPI_VFP << 16) | PANEL_V_ACTIVE);
    vTaskDelay(pdMS_TO_TICKS(100));
    bridge_write(TC_PPI_STARTPPI, 0x01);
    bridge_write(TC_DSI_STARTDSI, 0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TC358762 bridge configured");
    return ESP_OK;
}

// ============================================================================
// DSI bring-up
// ============================================================================
static esp_err_t dsi_bring_up(void)
{
    esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr),
                       TAG, "DSI PHY LDO acquire");
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO%d @ %dmV)", DSI_PHY_LDO_CHAN, DSI_PHY_LDO_MV);

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_NUM_DATA_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus),
                       TAG, "esp_lcd_new_dsi_bus");

    // Create + delete a DBI IO so the DSI controller latches LP speed mode
    // for Generic Long Writes. The setting persists after the IO is
    // deleted; bridge_init() depends on it to schedule its 16 register
    // writes during LP windows of the video stream.
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &dbi_io),
                       TAG, "esp_lcd_new_panel_io_dbi");
    esp_lcd_panel_io_del(dbi_io);

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_PIXEL_CLOCK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .in_color_format    = LCD_COLOR_FMT_RGB888,
        .out_color_format   = LCD_COLOR_FMT_RGB888,
        // Double-buffer the panel: the DSI engine streams one fb while
        // we fill the other. esp_lcd_panel_draw_bitmap(fb) becomes a
        // cache writeback + index swap (~µs) instead of a ~24 ms memcpy.
        .num_fbs            = 2,
        .video_timing = {
            .h_size            = PANEL_H_ACTIVE,
            .v_size            = PANEL_V_ACTIVE,
            .hsync_pulse_width = RPI_HSW,
            .hsync_back_porch  = RPI_HBP,
            .hsync_front_porch = RPI_HFP,
            .vsync_pulse_width = RPI_VSW,
            .vsync_back_porch  = RPI_VBP,
            .vsync_front_porch = RPI_VFP,
        },
        .flags.use_dma2d = false,
        // Allow LP blanking so the host can insert LP Generic Long Write
        // packets into VBI — required by TC358762, which only processes
        // commands during low-power intervals.
        .flags.disable_lp = 0,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_panel),
                       TAG, "esp_lcd_new_panel_dpi");

    // Override two DSI controller defaults that ESP-IDF hardcodes but
    // TC358762 needs different values for:
    //   1. Burst mode: hardcoded to BURST_WITH_SYNC_PULSES, but TC358762
    //      requires NON_BURST_WITH_SYNC_PULSES to match its DPI clock.
    //   2. Frame ACK: TC358762 doesn't BTA on every video frame; leaving
    //      this on stalls the stream.
    dsi_bus_shadow_t *p = (dsi_bus_shadow_t *)s_dsi_bus;
    mipi_dsi_host_ll_dpi_set_video_burst_type(p->hal.host,
        MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
    mipi_dsi_host_ll_dpi_enable_frame_ack(p->hal.host, false);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");

    // Grab pointers to both framebuffers so we can hand them to the
    // JPEG decoder as the direct decode destination — that triggers
    // the IDF DPI driver's fast path in draw_bitmap (no memcpy).
    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1),
                       TAG, "get_frame_buffer");
    s_panel_fbs[0] = fb0;
    s_panel_fbs[1] = fb1;
    s_back_fb      = 1;   // fb0 is the one the driver shows first; we fill fb1 next

    // Continuous HS clock — TC358762's internal FLL needs a stable
    // reference, and we still allow LP data-lane blanking so command
    // packets can be inserted between frames.
    mipi_dsi_host_ll_set_clock_lane_state(p->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    mipi_dsi_host_ll_enable_cmd_ack(p->hal.host, false);

    // Now the HS clock is running — release bridge reset and configure it.
    // Doing this before HS is up would silently drop every LP command.
    ESP_RETURN_ON_ERROR(bridge_release_and_wake(), TAG, "bridge wake");
    ESP_RETURN_ON_ERROR(bridge_init(),             TAG, "bridge init");

    // Release touch reset right before FT5426 will be probed.
    touch_release_reset();

    ESP_LOGI(TAG, "DSI up: %dx%d %u MHz, %d-lane %d Mbps, non-burst",
             PANEL_H_ACTIVE, PANEL_V_ACTIVE, (unsigned)PANEL_PIXEL_CLOCK_MHZ,
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
    ESP_RETURN_ON_ERROR(panel_power_on(),   TAG, "panel power-on");
    ESP_RETURN_ON_ERROR(dsi_bring_up(),     TAG, "DSI bring-up");

    s_rot_buf = (uint8_t *)heap_caps_malloc(
        PANEL_H_ACTIVE * PANEL_V_ACTIVE * 3,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rot_buf) {
        ESP_LOGE(TAG, "rotation scratch alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_up = true;

    // Cache configured brightness, leave backlight off — first wake applies it.
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

// RGB565 → 24-bit pixel in B,G,R memory order — what the DSI engine +
// TC358762 + Pi panel pipeline expects (the "RGB888" label refers to the
// MIPI wire bit order, not byte position in PSRAM).
static inline void rgb565_to_rgb888(uint16_t px, uint8_t *out)
{
    out[0] = (uint8_t)(( px        & 0x1F) * 255 / 31);  // B
    out[1] = (uint8_t)(((px >>  5) & 0x3F) * 255 / 63);  // G
    out[2] = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);  // R
}

esp_err_t display_present_bgr888(const void *bgr888)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                     PANEL_H_ACTIVE, PANEL_V_ACTIVE,
                                     bgr888);
}

void *display_back_buffer(size_t *out_size)
{
    if (!s_up) return NULL;
    if (out_size) *out_size = (size_t)PANEL_H_ACTIVE * PANEL_V_ACTIVE * 3;
    return s_panel_fbs[s_back_fb];
}

esp_err_t display_flip_back_buffer(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    void *fb = s_panel_fbs[s_back_fb];
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                              PANEL_H_ACTIVE, PANEL_V_ACTIVE,
                                              fb);
    if (err == ESP_OK) s_back_fb ^= 1;   // next /frame fills the other one
    return err;
}

esp_err_t display_present_rgb565(const uint16_t *src,
                                 uint16_t        src_w,
                                 uint16_t        src_h)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;

    viewport_state_lock();
    viewport_orientation_t orient = viewport_state_get()->orientation;
    viewport_state_unlock();

    if (orient == VIEWPORT_ORIENTATION_LANDSCAPE) {
        if (src_w != PANEL_H_ACTIVE || src_h != PANEL_V_ACTIVE)
            return ESP_ERR_INVALID_SIZE;
        size_t n = (size_t)src_w * src_h;
        for (size_t i = 0; i < n; ++i) {
            rgb565_to_rgb888(src[i], &s_rot_buf[i * 3]);
        }
    } else {
        if (src_w != PANEL_V_ACTIVE || src_h != PANEL_H_ACTIVE)
            return ESP_ERR_INVALID_SIZE;
        const uint16_t dst_stride = src_h;
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
