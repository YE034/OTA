/* 标准 C 库头文件优先 */
#include <string.h>

/* FreeRTOS 头文件（FreeRTOS.h 必须最先） */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* ESP-IDF 系统头文件 */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

/* 项目头文件 */
#include "wifi.h"

static const char *TAG = "wifi_mgr";

EventGroupHandle_t s_wifi_event_group;

/* NVS 中保存 Wi-Fi 凭证的命名空间与键名 */
#define WIFI_NVS_NAMESPACE  "wifi_cfg"
#define WIFI_NVS_KEY_SSID   "ssid"
#define WIFI_NVS_KEY_PASS   "pass"

/* 当前是否已有可用凭证（没有时断线不盲目重连） */
static bool s_has_config = false;

/* ============ NVS 凭证存取 ============ */
esp_err_t wifi_config_save(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid ? ssid : "");
    nvs_set_str(h, WIFI_NVS_KEY_PASS, password ? password : "");
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Wi-Fi credentials saved to NVS");
    return err;
}

bool wifi_config_has_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool ok = (nvs_get_str(h, WIFI_NVS_KEY_SSID, NULL, &len) == ESP_OK && len > 0);
    nvs_close(h);
    return ok;
}

/* 从 NVS 读取凭证到 wifi_config_t，读到返回 true */
static bool load_config_from_nvs(wifi_config_t *wcfg)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char ssid[33] = {0};
    char pass[65] = {0};
    size_t sl = sizeof(ssid), pl = sizeof(pass);
    esp_err_t e1 = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &sl);
    nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pl);   /* 开放网络可能无密码，忽略错误 */
    nvs_close(h);

    if (e1 != ESP_OK || ssid[0] == '\0') {
        return false;
    }
    memset(wcfg, 0, sizeof(*wcfg));
    strncpy((char *)wcfg->sta.ssid, ssid, sizeof(wcfg->sta.ssid) - 1);
    strncpy((char *)wcfg->sta.password, pass, sizeof(wcfg->sta.password) - 1);
    wcfg->sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    return true;
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_config) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "STA started, connecting...");
        } else {
            ESP_LOGI(TAG, "STA started, NO credentials yet — waiting for BLE provisioning");
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_has_config) {
            ESP_LOGW(TAG, "Disconnected, retrying in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected! IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    /* 默认事件循环可能已被创建，忽略“已存在”错误以增强健壮性 */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* 尝试加载 NVS 中保存的凭证 */
    wifi_config_t wcfg;
    if (load_config_from_nvs(&wcfg)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
        s_has_config = true;
        ESP_LOGI(TAG, "Loaded saved Wi-Fi: %s", wcfg.sta.ssid);
    } else {
        s_has_config = false;
        ESP_LOGI(TAG, "No saved Wi-Fi, enter BLE provisioning mode");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* WiFi+蓝牙共存必须开启 modem-sleep，否则使能蓝牙时 coex 会 abort 重启；
     * 温度读取已在 sensor 组件改为带超时，不再依赖关闭省电模式。
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));   /* WiFi+BT 共存要求 modem-sleep */
}

esp_err_t wifi_connect_with(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. 保存到 NVS，下次开机自动连接 */
    wifi_config_save(ssid, password);

    /* 2. 应用新配置 */
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wcfg.sta.password, password, sizeof(wcfg.sta.password) - 1);
    }
    wcfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    s_has_config = true;

    /* 3. 断开旧连接（若有）后用新凭证重连 */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = esp_wifi_connect();
    ESP_LOGI(TAG, "Connecting to provisioned Wi-Fi: %s", ssid);
    return err;
}
