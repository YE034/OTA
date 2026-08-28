/* 标准 C 库头文件优先 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* FreeRTOS 头文件（FreeRTOS.h 必须最先） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* ESP-IDF 系统头文件 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "esp_ota_ops.h"

/* 项目头文件 */
#include "wifi.h"
#include "onenet_mqtt.h"
#include "onenet_ota.h"
#include "temp_sensor.h"
#include "ws2812.h"

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

/* 物模型标识符（与 OneNET 控制台功能定义一致） */
#define PROPERTY_TEMP       "temp"            /* 温度：float */
#define PROPERTY_COLOR      "color"           /* RGB灯颜色：string */
#define PROPERTY_SWITCH     "light_switch"    /* RGB灯亮灭：bool */

/* 板载 WS2812 RGB 灯（ESP32-S3 常用 GPIO48） */
#define WS2812_GPIO_PIN     GPIO_NUM_48
#define WS2812_RMT_CH       RMT_CHANNEL_0

/* 上报间隔（毫秒）：30 秒一次，避免频繁读取温度触发看门狗 */
#define REPORT_INTERVAL_MS  30000
/* ============================================ */

/* ========== RGB 灯当前状态 ========== */
static bool s_light_on = false;      /* 灯开关 */
static uint8_t s_r = 0, s_g = 0, s_b = 0;   /* 颜色分量 */

/* 按当前状态刷新灯（开关 + 颜色） */
static void rgb_apply(void)
{
    if (s_light_on) {
        ws2812_set_color(s_r, s_g, s_b);
    } else {
        ws2812_set_color(0, 0, 0);
    }
}

/* ========== 颜色字符串解析 ==========
 * 支持三种格式：
 *   1) 颜色英文名：red / green / blue / yellow / cyan / magenta / white / black ...
 *   2) 十六进制：  "#FF0000" 或 "FF0000"
 *   3) 逗号分隔：  "255,0,0"
 * 解析成功返回 true，并输出 r/g/b */
static bool parse_color_string(const char *str, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (str == NULL || str[0] == '\0') {
        return false;
    }

    /* 1) 颜色英文名 */
    static const struct { const char *name; uint8_t r, g, b; } named[] = {
        {"red",     255,   0,   0},
        {"green",     0, 255,   0},
        {"blue",      0,   0, 255},
        {"yellow",  255, 255,   0},
        {"cyan",      0, 255, 255},
        {"magenta", 255,   0, 255},
        {"white",   255, 255, 255},
        {"black",     0,   0,   0},
        {"orange",  255, 165,   0},
        {"pink",    255, 192, 203},
        {"gray",    128, 128, 128},
        {"grey",    128, 128, 128},
        {"off",       0,   0,   0},
        {"none",      0,   0,   0},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strcasecmp(str, named[i].name) == 0) {
            *r = named[i].r; *g = named[i].g; *b = named[i].b;
            return true;
        }
    }

    /* 2) 十六进制：#RRGGBB 或 RRGGBB */
    const char *hex = str;
    if (hex[0] == '#') hex++;
    if (strlen(hex) == 6) {
        char *end = NULL;
        unsigned long val = strtoul(hex, &end, 16);
        if (end != NULL && *end == '\0') {
            *r = (val >> 16) & 0xFF;
            *g = (val >> 8)  & 0xFF;
            *b =  val        & 0xFF;
            return true;
        }
    }

    /* 3) 逗号分隔：r,g,b */
    int ir = -1, ig = -1, ib = -1;
    if (sscanf(str, "%d,%d,%d", &ir, &ig, &ib) == 3) {
        if (ir >= 0 && ir <= 255 && ig >= 0 && ig <= 255 && ib >= 0 && ib <= 255) {
            *r = (uint8_t)ir; *g = (uint8_t)ig; *b = (uint8_t)ib;
            return true;
        }
    }

    return false;
}

/* ========== OneNET 云端下行回调 ==========
 * 注意：签名必须与 onenet_mqtt.h 中 onenet_property_set_cb_t 一致：
 *       void (*)(const onenet_property_t *prop)                    */
static void on_property_set(const onenet_property_t *prop)
{
    if (prop == NULL || prop->identifier == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Cloud set: %s (is_string=%d)",
             prop->identifier, prop->is_string);

    /* light_switch：bool 类型，value 为 1.0/0.0 */
    if (strcmp(prop->identifier, PROPERTY_SWITCH) == 0) {
        s_light_on = (prop->value > 0.5);
        ESP_LOGI(TAG, "Light switch -> %s", s_light_on ? "ON" : "OFF");
        rgb_apply();
    }
    /* color：string 类型，str_value 为颜色字符串 */
    else if (strcmp(prop->identifier, PROPERTY_COLOR) == 0) {
        if (prop->is_string && prop->str_value != NULL) {
            uint8_t r, g, b;
            if (parse_color_string(prop->str_value, &r, &g, &b)) {
                s_r = r; s_g = g; s_b = b;
                ESP_LOGI(TAG, "Color -> (%d,%d,%d)", r, g, b);
                /* 如果灯已开，立即刷新颜色；如果灯关着，记住颜色等开灯时生效 */
                if (s_light_on) {
                    rgb_apply();
                }
            } else {
                ESP_LOGW(TAG, "Unrecognized color string: '%s'", prop->str_value);
            }
        }
    }
    else {
        ESP_LOGW(TAG, "Unknown property: %s", prop->identifier);
    }
}

/* ================= 温度上报任务 ================= */
static void report_task(void *pvParameters)
{
    bool version_reported = false;   /* OTA 版本号只需上报一次 */

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

        /* MQTT 首次连接成功后，上报固件版本号（供 OTA 平台判断是否需要升级） */
        if (!version_reported) {
            const esp_app_desc_t *desc = esp_ota_get_app_description();
            ESP_LOGI(TAG, "Current firmware version: %s", desc->version);
            onenet_ota_report_version();
            version_reported = true;
        }

        /* 读温度 → 上报 */
        float temp = temp_sensor_read();
        if (temp < 0) {
            ESP_LOGE(TAG, "Invalid temperature, skip");
        } else {
            ESP_LOGI(TAG, "Chip internal temp: %.1f C", temp);
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
    ESP_LOGI(TAG, "ESP32-S3 OneNET Temperature + RGB Demo");
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

    /* 标记当前固件为有效（OTA 升级后首次启动时，确认新固件运行正常，
     * 防止因新固件异常导致无限回滚） */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "New firmware pending verify, marking valid...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    ESP_LOGI(TAG, "Running partition: %s", running->label);

    /* 2. 初始化板载 WS2812 RGB 灯（上电先黄灯提示） */
    ws2812_init(WS2812_GPIO_PIN, WS2812_RMT_CH);
    ws2812_set_color(255, 255, 0);

    /* 2.5 初始化内部温度传感器（一次即可，避免任务内重复初始化） */
    temp_sensor_init();

    /* 3. Wi-Fi 初始化（STA 模式） */
    wifi_init_sta(WIFI_SSID, WIFI_PASS);

    /* 4. 等待 Wi-Fi 连上后启动 MQTT */
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected, starting MQTT...");
    ws2812_set_color(0, 0, 255);    /* 联网成功：蓝灯 */

    /* 注册云端下行回调（控制 RGB 灯） */
    onenet_mqtt_register_property_cb(on_property_set);

    onenet_mqtt_start(ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, ONENET_TOKEN);

    /* 5. 创建温度上报任务（低优先级，避免饿死 IDLE 任务） */
    xTaskCreate(report_task, "report_task", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "System ready. Cloud can now control RGB via property set.");
}
