/* 标准 C 库头文件优先 */
#include <stdio.h>

/* FreeRTOS 头文件（FreeRTOS.h 必须最先） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* ESP-IDF 系统头文件 */
#include "esp_log.h"
#include "nvs_flash.h"

/* 项目头文件 */
#include "wifi.h"
#include "onenet_mqtt.h"
#include "temp_sensor.h"
#include "wifi.h"

static const char *TAG = "main";

/* ================= 用户配置区 ================= */
#define WIFI_SSID           "led"
#define WIFI_PASS           "12345678"

/* 产品ID：OneNET 控制台显示的产品ID（注意区分大写 I 和小写 L） */
#define ONENET_PRODUCT_ID   "FIuz5CTnNf"

/* 设备名称：OneNET 上创建的设备名 */
#define ONENET_DEVICE_NAME  "test"

/* 鉴权 token：从 OneNET 控制台生成，需与 product_id / device_name 一致 */
#define ONENET_TOKEN        "version=2018-10-31&res=products%2FFIuz5CTnNf%2Fdevices%2Ftest&et=1819071134&method=md5&sign=ld1th7nf3WZqt2%2B1czQ4zA%3D%3D"

/* 物模型标识符：截图中温度属性标识符为 temp */
#define PROPERTY_TEMP       "temp"

/* 上报间隔（毫秒） */
#define REPORT_INTERVAL_MS  10000
/* ============================================ */

static void report_task(void *pvParameters)
{
    temp_sensor_init();

    while (1) {
        /* 等待 Wi-Fi 连上 */
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* 检查 MQTT 是否就绪 */
        if (!s_mqtt_connected) {
            ESP_LOGW(TAG, "MQTT not connected yet, retry in 1s");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 读温度 → 上报 */
        float temp = temp_sensor_read();
        if (temp < 0) {
            ESP_LOGE(TAG, "Invalid temperature, skip");
        } else {
            ESP_LOGI(TAG, "Chip internal temp: %.2f C", temp);
            bool ok = onenet_mqtt_report_temp(
                ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, PROPERTY_TEMP, temp);
            if (ok) {
                ESP_LOGI(TAG, "Temperature report sent successfully");
            } else {
                ESP_LOGE(TAG, "Temperature report failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 OneNET Temperature Demo");
    ESP_LOGI(TAG, "Product ID : %s", ONENET_PRODUCT_ID);
    ESP_LOGI(TAG, "Device Name: %s", ONENET_DEVICE_NAME);
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS 初始化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Wi-Fi 初始化（STA 模式） */
    wifi_init_sta(WIFI_SSID, WIFI_PASS);

    /* 3. 等待 Wi-Fi 连上后启动 MQTT */
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected, starting MQTT...");

    onenet_mqtt_start(ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, ONENET_TOKEN);

    /* 4. 创建温度上报任务 */
    xTaskCreate(report_task, "report_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready, reporting every %d ms", REPORT_INTERVAL_MS);
}
