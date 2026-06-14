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
static void   *s_out_buf;
static size_t  s_out_cap;

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

    // DMA-aligned PSRAM scratch buffers. Allocate once, reuse.
    jpeg_decode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };

    s_in_buf = jpeg_alloc_decoder_mem(JPEG_DECODER_MAX_INPUT_BYTES,  &in_cfg,  &s_in_cap);
    if (!s_in_buf) {
        ESP_LOGE(TAG, "jpeg input buf alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_out_buf = jpeg_alloc_decoder_mem(JPEG_DECODER_MAX_OUTPUT_BYTES, &out_cfg, &s_out_cap);
    if (!s_out_buf) {
        ESP_LOGE(TAG, "jpeg output buf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "decoder ready (in=%u bytes, out=%u bytes)",
             (unsigned)s_in_cap, (unsigned)s_out_cap);
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

esp_err_t jpeg_decoder_decode(size_t   jpeg_len,
                              void   **out_rgb565,
                              uint16_t *out_width,
                              uint16_t *out_height)
{
    if (jpeg_len == 0 || jpeg_len > s_in_cap) return ESP_ERR_INVALID_SIZE;

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        // TODO: known color bug on M5 — solid red renders correctly but solid
        // green renders ~black and solid blue renders green. Likely a JPEG
        // colorspace / channel-permutation issue, not a byte-order one
        // (swapping to BGR turns red into blue, confirming RGB is the right
        // element ordering). Needs follow-up.
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    jpeg_decode_picture_info_t info = {0};
    esp_err_t err = jpeg_decoder_get_info(s_in_buf, jpeg_len, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_get_info: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t out_size = 0;
    err = jpeg_decoder_process(s_engine, &dec_cfg,
                               s_in_buf,  jpeg_len,
                               s_out_buf, s_out_cap, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_process: %s", esp_err_to_name(err));
        return err;
    }

    *out_rgb565 = s_out_buf;
    *out_width  = info.width;
    *out_height = info.height;
    return ESP_OK;
}
