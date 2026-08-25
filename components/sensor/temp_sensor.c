/* ESP-IDF 系统头文件 */
#include "driver/temp_sensor.h"
#include "esp_log.h"

/* 项目头文件 */
#include "temp_sensor.h"

static const char *TAG = "temp_sensor";

void temp_sensor_init(void)
{
    temp_sensor_config_t cfg = TSENS_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(temp_sensor_set_config(cfg));
    ESP_ERROR_CHECK(temp_sensor_start());
    ESP_LOGI(TAG, "Internal temperature sensor started");
}

float temp_sensor_read(void)
{
    float temp = 0.0f;
    if (temp_sensor_read_celsius(&temp) == ESP_OK) {
        return temp;
    }
    ESP_LOGE(TAG, "Failed to read temperature");
    return -1.0f;
}
