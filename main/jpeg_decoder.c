#include "jpeg_decoder.h"

#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "jpeg";

static jpeg_decoder_handle_t s_engine;
static SemaphoreHandle_t     s_mutex;
static void   *s_in_buf;
static size_t  s_in_cap;

esp_err_t jpeg_decoder_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms    = 200,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&cfg, &s_engine),
                       TAG, "jpeg_new_decoder_engine");

    // DMA-aligned PSRAM scratch buffer for the inbound JPEG body. The
    // OUTPUT buffer is no longer owned here — callers pass in a target
    // (the display back-buffer) so the decoder writes the BGR888 pixels
    // straight into the panel framebuffer with no intermediate copy.
    jpeg_decode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    s_in_buf = jpeg_alloc_decoder_mem(JPEG_DECODER_MAX_INPUT_BYTES,  &in_cfg,  &s_in_cap);
    if (!s_in_buf) {
        ESP_LOGE(TAG, "jpeg input buf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "decoder ready (in=%u bytes)", (unsigned)s_in_cap);
    return ESP_OK;
}

bool jpeg_decoder_try_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void jpeg_decoder_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

void *jpeg_decoder_input_buffer(void) { return s_in_buf; }

void *jpeg_decoder_alloc_input_buffer(size_t *out_cap)
{
    jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t cap = 0;
    void  *buf = jpeg_alloc_decoder_mem(JPEG_DECODER_MAX_INPUT_BYTES, &in_cfg, &cap);
    if (out_cap) *out_cap = buf ? cap : 0;
    return buf;
}

esp_err_t jpeg_decoder_decode(void     *in_buf,
                              size_t    jpeg_len,
                              void     *out_buf,
                              size_t    out_cap,
                              uint16_t *out_width,
                              uint16_t *out_height)
{
    if (!in_buf)                  return ESP_ERR_INVALID_ARG;
    if (jpeg_len == 0 ||
        jpeg_len > JPEG_DECODER_MAX_INPUT_BYTES) return ESP_ERR_INVALID_SIZE;
    if (!out_buf || out_cap == 0) return ESP_ERR_INVALID_ARG;

    // Hardware decode directly into the caller's BGR888 buffer (the
    // panel back-framebuffer in the /frame path). _BGR rgb_order swaps
    // the channel layout so memory ends up as [B, G, R] per pixel —
    // exactly what the ESP32-P4 DSI engine + TC358762 + Pi panel
    // pipeline wants. Zero firmware-side pixel work.
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    jpeg_decode_picture_info_t info = {0};
    esp_err_t err = jpeg_decoder_get_info(in_buf, jpeg_len, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_get_info: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t out_size = 0;
    err = jpeg_decoder_process(s_engine, &dec_cfg,
                               in_buf, jpeg_len,
                               out_buf, out_cap, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_process: %s", esp_err_to_name(err));
        return err;
    }

    *out_width  = info.width;
    *out_height = info.height;
    return ESP_OK;
}
