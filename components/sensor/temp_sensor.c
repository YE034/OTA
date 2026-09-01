/*
 * ESP32-S3 片内温度传感器封装
 *
 * 关键点：ESP-IDF v4.4.3 官方驱动 temp_sensor_read_raw() 内部是
 *     while (!SENS.sar_tctrl.tsens_ready);     // 无限忙等
 * 在 WiFi + 蓝牙共存（必须开 modem-sleep）时，该 ready 位可能迟迟不置位，
 * 导致任务卡死、触发任务看门狗。这里改为「带超时的有界等待 + 官方同款换算」，
 * 任何电源状态下都不会卡死；超时则复用上一次读数。
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/temp_sensor.h"
#include "soc/sens_struct.h"
#include "esp_efuse_rtc_calib.h"

#include "temp_sensor.h"

static const char *TAG = "temp_sensor";

/* 与 ESP-IDF v4.4.3 ESP32-S3 rtc_tempsensor.c 内部一致的换算常量 */
#define TSENS_ADC_FACTOR   (0.4386f)
#define TSENS_DAC_FACTOR   (27.88f)
#define TSENS_SYS_OFFSET   (20.52f)
#define TSENS_L2_OFFSET    (0)        /* 默认量程 TSENS_DAC_L2 的 offset = 0 */

static float s_last_temp = 0.0f;
static bool  s_have_last = false;

void temp_sensor_init(void)
{
    /* TSENS_CONFIG_DEFAULT(): dac=L2(-10~80℃)、clk_div=6，与原工程一致 */
    temp_sensor_config_t cfg = TSENS_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(temp_sensor_set_config(cfg));
    ESP_ERROR_CHECK(temp_sensor_start());
    ESP_LOGI(TAG, "Internal temperature sensor started");
}

/* 带超时的原始值读取，替代官方驱动的无限 while 忙等 */
static esp_err_t read_raw_bounded(uint32_t *out)
{
    SENS.sar_tctrl.tsens_dump_out = 1;

    uint32_t guard = 0;
    while (!SENS.sar_tctrl.tsens_ready) {
        /* 有界上限：正常几十微秒即就绪；异常时最多等待约数百毫秒后退出 */
        if (++guard >= 300000) {
            SENS.sar_tctrl.tsens_dump_out = 0;
            return ESP_ERR_TIMEOUT;
        }
        /* 周期性喂狗并让出 CPU，避免饿死 IDLE 任务（这是看门狗的关键） */
        if ((guard & 0x3FF) == 0) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    *out = SENS.sar_tctrl.tsens_out;
    SENS.sar_tctrl.tsens_dump_out = 0;
    return ESP_OK;
}

float temp_sensor_read(void)
{
    uint32_t raw = 0;
    esp_task_wdt_reset();

    if (read_raw_bounded(&raw) != ESP_OK) {
        ESP_LOGW(TAG, "temp ready timeout, reuse last value");
        esp_task_wdt_reset();
        return s_have_last ? s_last_temp : 0.0f;
    }

    /* efuse 出厂温度校准：仅 calib version==1 有效，否则为 0（与官方逻辑一致） */
    float delta_t = 0.0f;
    uint32_t calib_ver = esp_efuse_rtc_calib_get_ver();
    if (calib_ver == 1) {
        delta_t = (float)esp_efuse_rtc_calib_get_cal_temp(calib_ver);
    }

    float celsius = TSENS_ADC_FACTOR * (float)raw
                    - TSENS_DAC_FACTOR * TSENS_L2_OFFSET
                    - TSENS_SYS_OFFSET - delta_t / 10.0f;

    s_last_temp = celsius;
    s_have_last = true;
    esp_task_wdt_reset();
    return celsius;
}
