/*
 * 自定义 BLE GATT 服务（纯通用 GATT，nRF Connect 即可读写，无需 EspBlufi）
 *
 * Service  0xE000  IoT Demo
 *   ├─ 0xE001 Temperature (Read/Notify)  温度字符串 "42.8"
 *   │     └─ 0x2902 CCCD（订阅温度推送）
 *   ├─ 0xE002 Command     (Write/WriteNoRsp) 写命令：
 *   │                       WIFI:ssid,password 配网；ON/OFF/颜色 控灯；STATE? 查询
 *   └─ 0xE003 Status      (Read/Notify)  配网结果/IP/状态文本反馈
 *         └─ 0x2902 CCCD（订阅状态推送）
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"

#include "ble_gatt.h"

static const char *TAG = "ble_gatt";

#define PROFILE_APP_ID          0x10
#define GATTS_NUM_HANDLE        9
#define MAX_CMD_LEN             64
#define MAX_STATUS_LEN          96

/* 属性表索引（attribute handle = index + 1） */
enum {
    IDX_SVC = 0,          /* h1 */
    IDX_TEMP_CHAR_DECL,   /* h2 */
    IDX_TEMP_VAL,         /* h3 */
    IDX_TEMP_CCCD,        /* h4 */
    IDX_CMD_CHAR_DECL,    /* h5 */
    IDX_CMD_VAL,          /* h6 */
    IDX_STA_CHAR_DECL,    /* h7 */
    IDX_STA_VAL,          /* h8 */
    IDX_STA_CCCD,         /* h9 */
};

/* ---- UUID / 描述符（属性表只存指针，必须静态存储） ---- */
static uint16_t g_primary_svc_uuid = ESP_GATT_UUID_PRI_SERVICE;   /* 0x2800 */
static uint16_t g_char_decl_uuid   = ESP_GATT_UUID_CHAR_DECLARE;   /* 0x2803 */
static uint16_t g_cccd_uuid        = ESP_GATT_UUID_CHAR_CLIENT_CONFIG; /* 0x2902 */
static uint16_t g_svc_uuid         = 0xE000;
static uint16_t g_temp_char_uuid   = 0xE001;
static uint16_t g_cmd_char_uuid    = 0xE002;
static uint16_t g_sta_char_uuid    = 0xE003;

/* Characteristic Declaration：[属性位][value handle 小端][UUID 小端] */
static uint8_t g_temp_decl[5] = { 0x12, 0x03, 0x00, 0x01, 0xE0 }; /* R|N, h3, E001 */
static uint8_t g_cmd_decl[5]  = { 0x0C, 0x06, 0x00, 0x02, 0xE0 }; /* W|WNR, h6, E002 */
static uint8_t g_sta_decl[5]  = { 0x12, 0x08, 0x00, 0x03, 0xE0 }; /* R|N, h8, E003 */

static char     g_temp_value[20] = "--.-";
static char     g_status_value[MAX_STATUS_LEN] = "ready";
static uint16_t g_temp_cccd = 0;
static uint16_t g_sta_cccd  = 0;
static uint8_t  g_cmd_value[MAX_CMD_LEN] = {0};

static const esp_gatts_attr_db_t g_attr_db[GATTS_NUM_HANDLE] = {
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_primary_svc_uuid, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(g_svc_uuid), (uint8_t *)&g_svc_uuid}},

    [IDX_TEMP_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_char_decl_uuid, ESP_GATT_PERM_READ,
         5, 5, g_temp_decl}},

    [IDX_TEMP_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_temp_char_uuid, ESP_GATT_PERM_READ,
         sizeof(g_temp_value), 4, (uint8_t *)g_temp_value}},

    [IDX_TEMP_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_cccd_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), sizeof(g_temp_cccd), (uint8_t *)&g_temp_cccd}},

    [IDX_CMD_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_char_decl_uuid, ESP_GATT_PERM_READ,
         5, 5, g_cmd_decl}},

    [IDX_CMD_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_cmd_char_uuid, ESP_GATT_PERM_WRITE,
         MAX_CMD_LEN, 0, g_cmd_value}},

    [IDX_STA_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_char_decl_uuid, ESP_GATT_PERM_READ,
         5, 5, g_sta_decl}},

    [IDX_STA_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_sta_char_uuid, ESP_GATT_PERM_READ,
         sizeof(g_status_value), 5, (uint8_t *)g_status_value}},

    [IDX_STA_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&g_cccd_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), sizeof(g_sta_cccd), (uint8_t *)&g_sta_cccd}},
};

/* ---- 运行时状态 ---- */
static esp_gatt_if_t     s_gatts_if;
static uint16_t          s_handle_table[GATTS_NUM_HANDLE];
static uint16_t          s_conn_id;
static bool              s_connected = false;
static bool              s_temp_notify = false;
static bool              s_sta_notify = false;
static ble_gatt_write_cb_t s_write_cb = NULL;
static ble_gatt_conn_cb_t  s_conn_cb = NULL;
static ble_gatt_ready_cb_t s_ready_cb = NULL;

void ble_gatt_register_write_cb(ble_gatt_write_cb_t cb)
{
    s_write_cb = cb;
}

void ble_gatt_register_conn_cb(ble_gatt_conn_cb_t cb)
{
    s_conn_cb = cb;
}

void ble_gatt_register_ready_cb(ble_gatt_ready_cb_t cb)
{
    s_ready_cb = cb;
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "register failed status=%d", param->reg.status);
            break;
        }
        s_gatts_if = gatts_if;
        esp_ble_gatts_create_attr_tab(g_attr_db, gatts_if, GATTS_NUM_HANDLE, 0);
        /* GATTS 注册成功，说明 Bluedroid 已就绪，此时再配置并启动广播最稳妥 */
        if (s_ready_cb) {
            s_ready_cb();
        }
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK ||
            param->add_attr_tab.num_handle != GATTS_NUM_HANDLE) {
            ESP_LOGE(TAG, "create attr tab failed status=%d num=%d",
                     param->add_attr_tab.status, param->add_attr_tab.num_handle);
            break;
        }
        memcpy(s_handle_table, param->add_attr_tab.handles,
               sizeof(uint16_t) * param->add_attr_tab.num_handle);
        esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
        ESP_LOGI(TAG, "GATT service 0xE000 started (Temp/Command/Status)");
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        ESP_LOGI(TAG, "Phone connected conn_id=%d", s_conn_id);
        if (s_conn_cb) s_conn_cb(true);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_temp_notify = false;
        s_sta_notify = false;
        ESP_LOGI(TAG, "Phone disconnected");
        if (s_conn_cb) s_conn_cb(false);
        break;

    case ESP_GATTS_MTU_EVT:
        /* 手机连上后发起 MTU 协商；协商成功后单包上限 = mtu-3（可远超20字节） */
        ESP_LOGI(TAG, "Negotiated MTU=%d (notify max %d bytes/frame)",
                 param->mtu.mtu, param->mtu.mtu - 3);
        break;

    case ESP_GATTS_WRITE_EVT: {
        uint16_t h = param->write.handle;
        /* 温度 CCCD */
        if (h == s_handle_table[IDX_TEMP_CCCD] && param->write.len == 2) {
            s_temp_notify = (param->write.value[0] & 0x01) != 0;
            ESP_LOGI(TAG, "Temperature notify %s", s_temp_notify ? "on" : "off");
        }
        /* 状态 CCCD */
        else if (h == s_handle_table[IDX_STA_CCCD] && param->write.len == 2) {
            s_sta_notify = (param->write.value[0] & 0x01) != 0;
            ESP_LOGI(TAG, "Status notify %s", s_sta_notify ? "on" : "off");
        }
        /* 命令写入 */
        else if (h == s_handle_table[IDX_CMD_VAL] && param->write.len > 0) {
            char cmd[MAX_CMD_LEN + 1];
            int n = param->write.len;
            if (n > MAX_CMD_LEN) n = MAX_CMD_LEN;
            memcpy(cmd, param->write.value, n);
            cmd[n] = '\0';
            while (n > 0 && (cmd[n-1]=='\r'||cmd[n-1]=='\n'||cmd[n-1]==' '||cmd[n-1]=='\t')) {
                cmd[--n] = '\0';
            }
            ESP_LOGI(TAG, "Command: '%s'", cmd);
            if (s_write_cb && cmd[0]) {
                s_write_cb(cmd);
            }
        }
        break;
    }

    default:
        break;
    }
}

/* 向指定特征写入新值并（在订阅时）Notify，内部通用函数 */
static void update_and_notify(uint16_t val_handle, const char *text, bool subscribed)
{
    if (val_handle == 0) {
        return;
    }
    size_t len = strlen(text);
    esp_ble_gatts_set_attr_value(val_handle, len, (uint8_t *)text);
    if (s_connected && subscribed) {
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, val_handle, len,
                                   (uint8_t *)text, false);
    }
}

void ble_gatt_init(void)
{
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));
    esp_ble_gatt_set_local_mtu(200);
}

void ble_gatt_set_temperature(float temp_c)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "%.1f", temp_c);
    update_and_notify(s_handle_table[IDX_TEMP_VAL], buf, s_temp_notify);
}

void ble_gatt_notify_status(const char *text)
{
    if (text == NULL) {
        return;
    }
    char buf[MAX_STATUS_LEN];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ESP_LOGI(TAG, "[status] %s", buf);
    update_and_notify(s_handle_table[IDX_STA_VAL], buf, s_sta_notify);
}
