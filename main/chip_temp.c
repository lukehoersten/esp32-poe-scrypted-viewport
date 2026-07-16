#include "chip_temp.h"

#include <math.h>

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "temp";

static temperature_sensor_handle_t s_tsens;

esp_err_t chip_temp_init(void)
{
    // 20-100°C range: best accuracy band for a device that idles warm
    // (PoE + PSRAM at 200 MHz) and whose interesting failures are hot.
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return err;
    }
    float t = chip_temp_read();
    ESP_LOGI(TAG, "on-die sensor up, %.1f°C at boot", (double)t);
    return ESP_OK;
}

float chip_temp_read(void)
{
    if (!s_tsens) return NAN;
    float celsius = NAN;
    if (temperature_sensor_get_celsius(s_tsens, &celsius) != ESP_OK) return NAN;
    return celsius;
}
