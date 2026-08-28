/*
 * OneNET OTA 升级组件
 *
 * 流程：
 *   1. 设备启动 → HTTP 上报当前版本号
 *   2. 云端创建升级任务 → MQTT 下发 ota/inform 通知
 *   3. 设备收到通知 → HTTP 检测升级任务（获取 tid/版本/大小）
 *   4. HTTP 下载固件 → 写入 OTA 分区 → 设置启动分区 → 重启
 *
 * OneNET OTA HTTP API（文档：open.iot.10086.cn/doc/aiot/develop/detail/1447）：
 *   上报版本: POST /fuse-ota/{pid}/{dev}/version
 *   检测任务: GET  /fuse-ota/{pid}/{dev}/check?type=1&version=x.x
 *   下载固件: GET  /fuse-ota/{pid}/{dev}/{tid}/download
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

#include "onenet_ota.h"

static const char *TAG = "onenet_ota";

/* OneNET OTA HTTP API 基础地址 */
#define ONENET_OTA_BASE_URL  "http://iot-api.heclouds.com/fuse-ota"

/* HTTP 下载缓冲区大小 */
#define OTA_BUF_SIZE         1024

/* OTA 任务栈大小（HTTP + OTA + cJSON 需要较大栈） */
#define OTA_TASK_STACK       16384

/* 设备鉴权信息（由 onenet_ota_init 传入） */
static char s_product_id[64]  = {0};
static char s_device_name[64] = {0};
static char s_token[512]      = {0};

/* HTTP API 响应缓冲区（用于收集 JSON 响应） */
static char s_resp_buf[2048];
static int  s_resp_len = 0;

/* ========== HTTP 事件回调：收集响应体到 s_resp_buf ========== */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy_len = evt->data_len;
        if (s_resp_len + copy_len >= (int)sizeof(s_resp_buf)) {
            copy_len = (int)sizeof(s_resp_buf) - 1 - s_resp_len;
        }
        if (copy_len > 0) {
            memcpy(s_resp_buf + s_resp_len, evt->data, copy_len);
            s_resp_len += copy_len;
            s_resp_buf[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* ========== 通用 HTTP 请求（用于 OTA API 调用，收集 JSON 响应） ========== */
static esp_err_t ota_http_request(const char *url, esp_http_client_method_t method,
                                  const char *post_data)
{
    s_resp_len = 0;
    s_resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", s_token);
    if (post_data) {
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP %s status=%d len=%d", url, status, s_resp_len);
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return (err == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

/* ========== 1. 上报设备当前版本号 ========== */
esp_err_t onenet_ota_report_version(void)
{
    const esp_app_desc_t *desc = esp_ota_get_app_description();

    char url[256], body[128];
    snprintf(url, sizeof(url), "%s/%s/%s/version",
             ONENET_OTA_BASE_URL, s_product_id, s_device_name);
    snprintf(body, sizeof(body),
             "{\"s_version\":\"%s\",\"f_version\":\"%s\"}",
             desc->version, desc->version);

    if (ota_http_request(url, HTTP_METHOD_POST, body) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (code && code->valueint == 0) {
            ESP_LOGI(TAG, "Version reported: %s", desc->version);
            cJSON_Delete(root);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Report version failed: %s", s_resp_buf);
        cJSON_Delete(root);
    }
    return ESP_FAIL;
}

/* ========== 2. 检测升级任务，返回 tid（-1 = 无任务） ========== */
static int ota_check_task(const char *current_version)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/%s/%s/check?type=1&version=%s",
             ONENET_OTA_BASE_URL, s_product_id, s_device_name, current_version);

    if (ota_http_request(url, HTTP_METHOD_GET, NULL) != ESP_OK) {
        return -1;
    }

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "Check task: bad JSON: %s", s_resp_buf);
        return -1;
    }

    int tid = -1;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");

    if (code && code->valueint == 0 && cJSON_IsObject(data)) {
        cJSON *tid_item = cJSON_GetObjectItem(data, "tid");
        cJSON *target   = cJSON_GetObjectItem(data, "target");
        cJSON *size_it  = cJSON_GetObjectItem(data, "size");
        cJSON *status_it = cJSON_GetObjectItem(data, "status");

        if (cJSON_IsNumber(tid_item)) {
            tid = tid_item->valueint;
            ESP_LOGI(TAG, "Upgrade task found: tid=%d target=%s size=%d status=%d",
                     tid,
                     cJSON_IsString(target) ? target->valuestring : "?",
                     cJSON_IsNumber(size_it) ? size_it->valueint : 0,
                     cJSON_IsNumber(status_it) ? status_it->valueint : -1);
        }
    } else {
        ESP_LOGI(TAG, "No upgrade task (code=%d)", code ? code->valueint : -1);
    }
    cJSON_Delete(root);
    return tid;
}

/* ========== 3. 下载固件并写入 OTA 分区 ========== */
static esp_err_t ota_download_and_flash(int tid)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/%s/%s/%d/download",
             ONENET_OTA_BASE_URL, s_product_id, s_device_name, tid);
    ESP_LOGI(TAG, "Downloading firmware: %s", url);

    /* 获取下一个可写的 OTA 分区 */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA partition found! Check partitions.csv");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Target partition: %s at 0x%x (size 0x%x)",
             update_part->label, (unsigned)update_part->address,
             (unsigned)update_part->size);

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* 建立 HTTP 连接（不使用事件回调，直接流式读取） */
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", s_token);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        free(buf);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d", status, content_length);

    if (status != 200 || content_length == 0) {
        ESP_LOGE(TAG, "Download failed: HTTP %d, len=%d", status, content_length);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 开始 OTA 写入 */
    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    int total = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;   /* 下载完成 */
        }
        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        total += read_len;
        /* 每 16KB 打印一次进度 */
        if (content_length > 0 && (total % (16 * 1024)) < OTA_BUF_SIZE) {
            ESP_LOGI(TAG, "Progress: %d/%d bytes (%.1f%%)",
                     total, content_length,
                     (float)total * 100.0f / (float)content_length);
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    ESP_LOGI(TAG, "Download complete: %d bytes, finalizing OTA...", total);

    /* 结束 OTA 写入（会校验镜像合法性） */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image may be invalid)",
                 esp_err_to_name(err));
        return err;
    }

    /* 设置下次启动分区 */
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "OTA SUCCESS! New firmware will boot after restart.");
    ESP_LOGI(TAG, "========================================");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;  /* 不会执行到这里 */
}

/* ========== OTA 任务（在独立任务中执行，避免阻塞 MQTT） ========== */
static void ota_task(void *pvParameters)
{
    const esp_app_desc_t *desc = esp_ota_get_app_description();
    ESP_LOGI(TAG, "OTA task started, current version: %s", desc->version);

    /* 1. 上报当前版本号 */
    onenet_ota_report_version();

    /* 2. 检测是否有升级任务 */
    int tid = ota_check_task(desc->version);
    if (tid < 0) {
        ESP_LOGI(TAG, "No upgrade available, OTA task done");
        vTaskDelete(NULL);
        return;
    }

    /* 3. 下载并刷写固件 */
    ota_download_and_flash(tid);

    vTaskDelete(NULL);
}

/* ========== 触发 OTA 流程（收到 MQTT ota/inform 后调用） ========== */
void onenet_ota_start(void)
{
    ESP_LOGI(TAG, "OTA inform received, starting OTA task...");
    xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK, NULL, 3, NULL);
}

/* ========== 初始化 ========== */
void onenet_ota_init(const char *product_id, const char *device_name,
                     const char *token)
{
    strncpy(s_product_id, product_id, sizeof(s_product_id) - 1);
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
    strncpy(s_token, token, sizeof(s_token) - 1);
    ESP_LOGI(TAG, "OTA init done (pid=%s, dev=%s)", s_product_id, s_device_name);
}
