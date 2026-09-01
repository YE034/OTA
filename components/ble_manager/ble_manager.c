/*
 * BLE 管理器：纯通用 GATT（nRF Connect 直连即可，无需 EspBlufi）
 *
 * 命令协议（手机往 0xE002 写 UTF-8 文本，大小写不敏感）：
 *   WIFI:ssid,password   保存凭证到 NVS 并立即联网（开放网络可省略逗号和密码）
 *   STATE?               查询当前联网状态（结果从 0xE003 Notify 返回）
 *   ON / OFF / 颜色      控灯命令，转发给应用层注册的回调
 *
 * 配网结果通过 0xE003 状态特征 Notify 回传手机。
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "wifi.h"
#include "ble_gatt.h"
#include "ble_manager.h"

static const char *TAG = "ble_mgr";

#define BLE_DEVICE_NAME_MAX   32
#define SSID_MAX              32
#define PASS_MAX              64
#define CMD_MAX               96

static ble_command_cb_t s_command_cb = NULL;
static char   s_device_name[BLE_DEVICE_NAME_MAX] = "ESP32S3-IoT";
static bool   s_bt_ready = false;
static bool   s_provision_pending = false;   /* 刚下发配网，等待结果中 */
static char   s_net_status[64] = "WiFi not connected";

/* ================= BLE 广播数据 =================
 * 注意：ESP-IDF v4.4.3 的 esp_ble_gap_config_adv_data() 要求 service_uuid_len
 * 必须是 16 的整数倍（仅接受 128-bit UUID），放 16-bit UUID(长度2)会返回
 * ESP_ERR_INVALID_ARG。因此广播包只放 Flags + 设备名；0xE000 服务在手机
 * 连接后的 GATT 服务发现阶段自然可见，不影响 nRF Connect 读写。 */
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0,
    .max_interval = 0,
    .appearance = 0,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void ble_start_advertising(void)
{
    if (!s_bt_ready) {
        return;
    }
    esp_ble_gap_start_advertising(&s_adv_params);
}

/* GAP 事件：广播数据配置完成后启动广播 */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Adv data set, status=%d -> start advertising",
                 param->adv_data_cmpl.status);
        ble_start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, ">>> BLE advertising as '%s' (scan it in nRF Connect)", s_device_name);
        }
        break;
    default:
        break;
    }
}

/* 手机连接状态：断开后重新开始广播，保证可再次被扫描连接 */
static void on_phone_conn_changed(bool connected)
{
    if (!connected) {
        ESP_LOGI(TAG, "Phone gone, restart advertising");
        ble_start_advertising();
    }
}

/* ================= WiFi 状态 → 回传手机 ================= */
static void on_wifi_status_event(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        /* 保持 ≤20 字节：默认 ATT MTU=23(单包20B)，手机未协商 MTU 时也能一次读全。
         * "IP:" + 最长IP(15) = 18 字节 */
        snprintf(s_net_status, sizeof(s_net_status),
                 "IP:" IPSTR, IP2STR(&e->ip_info.ip));
        if (s_provision_pending) {
            s_provision_pending = false;
            ble_gatt_notify_status(s_net_status);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        snprintf(s_net_status, sizeof(s_net_status), "WiFi disconnected");
        if (s_provision_pending) {
            s_provision_pending = false;
            /* 详细原因见串口日志，BLE 文本保持简短 */
            ble_gatt_notify_status("Connect failed");
        }
    }
}

/* ================= 命令解析 ================= */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return s;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) {
        *e-- = '\0';
    }
    return s;
}

static void handle_provision(const char *payload)
{
    char ssid[SSID_MAX + 1] = {0};
    char pass[PASS_MAX + 1] = {0};

    const char *comma = strchr(payload, ',');
    size_t slen;
    if (comma) {
        slen = comma - payload;
    } else {
        slen = strlen(payload);
    }
    if (slen == 0 || slen >= sizeof(ssid)) {
        ble_gatt_notify_status("Use WIFI:ssid,pass");
        return;
    }
    memcpy(ssid, payload, slen);
    ssid[slen] = '\0';
    char *s = trim(ssid);
    memmove(ssid, s, strlen(s) + 1);
    if (ssid[0] == '\0') {
        ble_gatt_notify_status("Empty SSID");
        return;
    }
    if (comma) {
        strncpy(pass, comma + 1, PASS_MAX);
        pass[PASS_MAX] = '\0';
        char *p = trim(pass);
        memmove(pass, p, strlen(p) + 1);
    }

    ble_gatt_notify_status("Connecting...");
    ESP_LOGI(TAG, "Provision WiFi: ssid='%s' pass_len=%d", ssid, (int)strlen(pass));

    s_provision_pending = true;
    wifi_connect_with(ssid, pass[0] ? pass : NULL);
}

static void on_gatt_command(const char *raw)
{
    char cmd[CMD_MAX];
    strncpy(cmd, raw, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    /* 配网命令（前缀大小写不敏感，SSID/密码保留原始大小写） */
    if (strncasecmp(cmd, "WIFI:", 5) == 0) {
        handle_provision(cmd + 5);
        return;
    }
    /* 状态查询 */
    if (strcasecmp(cmd, "STATE?") == 0) {
        ble_gatt_notify_status(s_net_status);
        return;
    }

    /* 其余视为控灯命令：整体大写后交给应用层 */
    for (int i = 0; cmd[i]; i++) {
        if (cmd[i] >= 'a' && cmd[i] <= 'z') cmd[i] -= 32;
    }
    if (s_command_cb) {
        s_command_cb(cmd);
    }
}

/* ================= 初始化 ================= */
void ble_manager_register_command_cb(ble_command_cb_t cb)
{
    s_command_cb = cb;
}

void ble_manager_set_temperature(float temp_c)
{
    ble_gatt_set_temperature(temp_c);
}

/* GATTS 注册事件回来、协议栈确认就绪后，再配置广播数据；
 * 其完成事件(ADV_DATA_SET_COMPLETE)里才真正 start advertising。
 * 不能在 esp_bluedroid_enable() 后同步抢跑，否则广播请求可能被丢弃。 */
static void on_stack_ready(void)
{
    esp_err_t err = esp_ble_gap_config_adv_data(&s_adv_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Stack ready, configuring advertising data");
    }
}

void ble_manager_init(const char *device_name)
{
    if (device_name && device_name[0]) {
        strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
    }

    /* 1. 释放经典蓝牙内存（S3 只用 BLE），初始化 controller 为 BLE-only */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    /* 2. 初始化并使能 Bluedroid 协议栈 */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* 3. 设备名 + GAP 回调 */
    esp_ble_gap_set_device_name(s_device_name);
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    /* 4. 自定义 GATT 服务（温度/命令/状态）。注册就绪回调：等 GATTS 注册
     *    事件回来、协议栈确认就绪后再配置广播，避免抢跑导致广播起不来 */
    ble_gatt_register_write_cb(on_gatt_command);
    ble_gatt_register_conn_cb(on_phone_conn_changed);
    ble_gatt_register_ready_cb(on_stack_ready);
    s_bt_ready = true;
    ble_gatt_init();

    /* 5. 监听 WiFi/IP 结果，用于把配网成功/失败回传手机 */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi_status_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               on_wifi_status_event, NULL));

    ESP_LOGI(TAG, "BLE manager init done, name: %s (adv starts on stack ready)", s_device_name);
}
