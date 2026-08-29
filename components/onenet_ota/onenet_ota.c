/*
 * OneNET OTA 升级组件（完整版）
 *
 * 完整流程：
 *   1. 设备启动 → HTTP 上报当前版本号
 *   2. 云端创建升级任务 → MQTT 下发 ota/inform 通知
 *   3. 设备收到通知 → HTTP 检测升级任务（获取 tid/目标版本/大小/md5）
 *   4. HTTP 流式下载固件，边写 Flash 边算 MD5，并按 10% 粒度上报下载进度
 *   5. 下载完成 → 上报 step=100 → MD5 校验 → 镜像合法性校验
 *   6. 设置启动分区 → 上报 step=201 升级成功 → 延时后重启
 *   任何环节失败均上报对应失败状态码，云端可实时看到失败原因
 *
 * OneNET OTA HTTP API（文档：open.iot.10086.cn/doc/aiot/develop/detail/1447~1449）：
 *   上报版本: POST /fuse-ota/{pid}/{dev}/version
 *   检测任务: GET  /fuse-ota/{pid}/{dev}/check?type=1&version=x.x
 *   下载固件: GET  /fuse-ota/{pid}/{dev}/{tid}/download
 *   上报状态: POST /fuse-ota/{pid}/{dev}/{tid}/status   body: {"step":N}
 *     step 0~100 : 下载进度百分比；100=下载完成
 *     102=空间不足 103=内存溢出 104=下载超时 107=下载未知异常
 *     201=升级成功 205=MD5校验失败 206=升级未知异常
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "mbedtls/md5.h"

#include "onenet_ota.h"

static const char *TAG = "onenet_ota";

/* OneNET OTA HTTP API 基础地址 */
#define ONENET_OTA_BASE_URL  "http://iot-api.heclouds.com/fuse-ota"

/* HTTP 下载缓冲区大小：4KB，比 1KB 减少 3/4 的读取次数，下载更快 */
#define OTA_BUF_SIZE         4096

/* HTTP API 重试次数与间隔 */
#define HTTP_MAX_RETRY       3
#define HTTP_RETRY_DELAY_MS  1500

/* OTA 任务栈大小（HTTP + OTA + cJSON + MD5 需要较大栈） */
#define OTA_TASK_STACK       16384

/* 下载进度上报粒度（每 10% 上报一次，避免 HTTP 请求过密） */
#define PROGRESS_REPORT_STEP 10

/* 升级成功上报后、重启前的等待时间，确保状态请求送达云端 */
#define RESTART_DELAY_MS     1500

/* 设备鉴权信息（由 onenet_ota_init 传入） */
static char s_product_id[64]  = {0};
static char s_device_name[64] = {0};
static char s_token[512]      = {0};

/* HTTP API 响应缓冲区（用于收集 JSON 响应） */
static char s_resp_buf[2048];
static int  s_resp_len = 0;

/* OTA 防重入标志：下载/刷写期间拒绝再次触发 */
static volatile bool s_ota_running = false;

/* 预重启回调（应用层注册，用于优雅关闭 MQTT/WiFi） */
static onenet_pre_reboot_cb_t s_pre_reboot_cb = NULL;

void onenet_ota_set_pre_reboot_cb(onenet_pre_reboot_cb_t cb)
{
    s_pre_reboot_cb = cb;
}

/* 升级任务信息（check 接口解析结果） */
typedef struct {
    int  tid;                 /* 任务 ID */
    char target[32];          /* 目标版本号 */
    int  size;                /* 固件大小（字节） */
    char md5[40];             /* 固件 MD5（十六进制小写字符串） */
} ota_task_info_t;

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

/* ========== 单次通用 HTTP 请求（用于 OTA API 调用，收集 JSON 响应） ========== */
static esp_err_t ota_http_perform(const char *url, esp_http_client_method_t method,
                                  const char *post_data)
{
    s_resp_len = 0;
    s_resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
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
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return (err == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

/* ========== 带重试的 HTTP 请求 ========== */
static esp_err_t ota_http_request(const char *url, esp_http_client_method_t method,
                                  const char *post_data)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 1; i <= HTTP_MAX_RETRY; i++) {
        err = ota_http_perform(url, method, post_data);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (i < HTTP_MAX_RETRY) {
            ESP_LOGW(TAG, "HTTP retry %d/%d after %dms", i, HTTP_MAX_RETRY, HTTP_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(HTTP_RETRY_DELAY_MS));
        }
    }
    return err;
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

/* ========== 2. 上报升级状态（下载进度 / 成功 / 失败） ========== */
static void ota_report_step(int tid, int step)
{
    char url[256], body[32];
    snprintf(url, sizeof(url), "%s/%s/%s/%d/status",
             ONENET_OTA_BASE_URL, s_product_id, s_device_name, tid);
    snprintf(body, sizeof(body), "{\"step\":%d}", step);

    /* 状态上报失败不阻断主流程（云端仍可通过版本号推断最终结果），重试2次即可 */
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", s_token);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 2; i++) {
        err = esp_http_client_perform(client);
        if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
            ESP_LOGI(TAG, "Upgrade status step=%d reported", step);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to report step=%d", step);
    }
    esp_http_client_cleanup(client);
}

/* ========== 3. 检测升级任务，解析 tid/目标版本/大小/md5 ========== */
static esp_err_t ota_check_task(const char *current_version, ota_task_info_t *info)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/%s/%s/check?type=1&version=%s",
             ONENET_OTA_BASE_URL, s_product_id, s_device_name, current_version);

    if (ota_http_request(url, HTTP_METHOD_GET, NULL) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "Check task: bad JSON: %s", s_resp_buf);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");

    if (code && code->valueint == 0 && cJSON_IsObject(data)) {
        cJSON *tid_item = cJSON_GetObjectItem(data, "tid");
        cJSON *target   = cJSON_GetObjectItem(data, "target");
        cJSON *size_it  = cJSON_GetObjectItem(data, "size");
        cJSON *md5_it   = cJSON_GetObjectItem(data, "md5");

        if (cJSON_IsNumber(tid_item)) {
            memset(info, 0, sizeof(*info));
            info->tid = tid_item->valueint;
            info->size = cJSON_IsNumber(size_it) ? size_it->valueint : 0;
            if (cJSON_IsString(target) && target->valuestring) {
                strncpy(info->target, target->valuestring, sizeof(info->target) - 1);
            }
            if (cJSON_IsString(md5_it) && md5_it->valuestring) {
                strncpy(info->md5, md5_it->valuestring, sizeof(info->md5) - 1);
            }
            ESP_LOGI(TAG, "Upgrade task: tid=%d target=%s size=%d md5=%s",
                     info->tid, info->target, info->size,
                     info->md5[0] ? info->md5 : "(none)");
            ret = ESP_OK;
        }
    } else {
        ESP_LOGI(TAG, "No upgrade task (code=%d, resp=%s)",
                 code ? code->valueint : -1, s_resp_buf);
    }
    cJSON_Delete(root);
    return ret;
}

/* ========== 4. 下载固件、MD5校验、写入 OTA 分区 ========== */
static esp_err_t ota_download_and_flash(const ota_task_info_t *info)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/%s/%s/%d/download",
             ONENET_OTA_BASE_URL, s_product_id, s_device_name, info->tid);
    ESP_LOGI(TAG, "Downloading firmware: %s", url);

    /* 获取下一个可写的 OTA 分区（当前运行 ota_0 则写 ota_1，反之亦然） */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA partition found! Check partitions.csv");
        ota_report_step(info->tid, 107);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Target partition: %s at 0x%x (size 0x%x = %d KB)",
             update_part->label, (unsigned)update_part->address,
             (unsigned)update_part->size, (int)(update_part->size / 1024));

    /* 云端声明的固件大小不能超过目标分区容量 */
    if (info->size > 0 && (uint32_t)info->size > update_part->size) {
        ESP_LOGE(TAG, "Firmware %d bytes exceeds partition %d bytes, abort",
                 info->size, (int)update_part->size);
        ota_report_step(info->tid, 102);   /* 空间不足 */
        return ESP_ERR_NO_MEM;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "malloc %d bytes failed", OTA_BUF_SIZE);
        ota_report_step(info->tid, 103);   /* 内存溢出 */
        return ESP_ERR_NO_MEM;
    }

    /* 建立 HTTP 连接（流式读取，不使用全局事件回调，避免污染 s_resp_buf） */
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = OTA_BUF_SIZE,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", s_token);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        ota_report_step(info->tid, 104);   /* 下载超时/连接失败 */
        free(buf);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d", status, content_length);

    if (status != 200 || content_length <= 0) {
        ESP_LOGE(TAG, "Download failed: HTTP %d, len=%d", status, content_length);
        ota_report_step(info->tid, 107);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 以 HTTP 实际返回长度为准，再次检查分区容量 */
    if ((uint32_t)content_length > update_part->size) {
        ESP_LOGE(TAG, "Content-Length %d exceeds partition, abort", content_length);
        ota_report_step(info->tid, 102);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* 开始 OTA 写入 */
    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_report_step(info->tid, 206);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    /* 初始化 MD5 流式计算，下载过程中同步累加 */
    mbedtls_md5_context md5_ctx;
    bool md5_ok = true;
    if (info->md5[0]) {
        mbedtls_md5_init(&md5_ctx);
        if (mbedtls_md5_starts_ret(&md5_ctx) != 0) {
            ESP_LOGW(TAG, "MD5 init failed, skip MD5 verification");
            md5_ok = false;
        }
    } else {
        md5_ok = false;   /* 云端未提供 md5，不校验 */
    }

    /* 通知云端进入"下载中" */
    ota_report_step(info->tid, 1);

    int total = 0;
    int last_reported_pct = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error after %d bytes", total);
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
        if (md5_ok) {
            mbedtls_md5_update_ret(&md5_ctx, (const unsigned char *)buf, read_len);
        }
        total += read_len;

        /* 按固定粒度上报下载进度（0~100） */
        int pct = (int)((int64_t)total * 100 / content_length);
        if (pct > 100) pct = 100;
        if (pct >= last_reported_pct + PROGRESS_REPORT_STEP && pct < 100) {
            ESP_LOGI(TAG, "Progress: %d/%d bytes (%d%%)", total, content_length, pct);
            ota_report_step(info->tid, pct);
            last_reported_pct = pct;
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download aborted at %d/%d bytes", total, content_length);
        esp_ota_abort(ota_handle);
        if (md5_ok) mbedtls_md5_free(&md5_ctx);
        ota_report_step(info->tid, 107);
        return err;
    }

    /* 完整性校验 1：实际下载字节数必须与 Content-Length 一致 */
    if (total != content_length) {
        ESP_LOGE(TAG, "Incomplete download: got %d, expected %d", total, content_length);
        esp_ota_abort(ota_handle);
        if (md5_ok) mbedtls_md5_free(&md5_ctx);
        ota_report_step(info->tid, 104);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Download complete: %d bytes", total);

    /* 完整性校验 2：MD5 比对（云端提供了 md5 时） */
    if (md5_ok) {
        unsigned char digest[16];
        char calc_md5[33];
        mbedtls_md5_finish_ret(&md5_ctx, digest);
        mbedtls_md5_free(&md5_ctx);
        for (int i = 0; i < 16; i++) {
            sprintf(calc_md5 + i * 2, "%02x", digest[i]);
        }
        calc_md5[32] = '\0';

        if (strcasecmp(calc_md5, info->md5) != 0) {
            ESP_LOGE(TAG, "MD5 mismatch! calc=%s expected=%s", calc_md5, info->md5);
            esp_ota_abort(ota_handle);
            ota_report_step(info->tid, 205);   /* MD5 校验失败 */
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "MD5 verified OK: %s", calc_md5);
    }

    /* 下载完成：step=100（平台状态转为"升级中"，无需再发 101） */
    ota_report_step(info->tid, 100);

    /* 完整性校验 3：结束 OTA 写入，内部校验 ESP 镜像头与各段合法性 */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (invalid image)", esp_err_to_name(err));
        ota_report_step(info->tid, 206);
        return err;
    }

    /* 设置下次启动分区为刚写入的分区 */
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_report_step(info->tid, 206);
        return err;
    }

    /* 上报升级成功（平台把设备版本更新为目标版本） */
    ota_report_step(info->tid, 201);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "OTA SUCCESS! target=%s, reboot in %dms",
             info->target, RESTART_DELAY_MS);
    ESP_LOGI(TAG, "========================================");

    /* 重启前优雅关闭 MQTT/WiFi，避免强制掐断 TCP 产生 error 113/119 红色日志 */
    if (s_pre_reboot_cb) {
        s_pre_reboot_cb();
    }

    /* 等待网络协议栈清理完成、状态请求送达，再重启进入新固件 */
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
    return ESP_OK;  /* 不会执行到这里 */
}

/* ========== OTA 任务（独立任务，避免阻塞 MQTT） ========== */
static void ota_task(void *pvParameters)
{
    const esp_app_desc_t *desc = esp_ota_get_app_description();
    ESP_LOGI(TAG, "OTA task started, current version: %s", desc->version);

    /* 1. 上报当前版本号（失败不阻断，check 时也会带版本） */
    onenet_ota_report_version();

    /* 2. 检测是否有升级任务 */
    ota_task_info_t info;
    if (ota_check_task(desc->version, &info) != ESP_OK) {
        ESP_LOGI(TAG, "No upgrade available, OTA task done");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* 3. 下载、校验、刷写（内部成功后会重启，失败会返回） */
    esp_err_t err = ota_download_and_flash(&info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA upgrade failed: %s", esp_err_to_name(err));
    }

    /* 只有失败才会走到这里；成功时已在 download 函数内重启 */
    s_ota_running = false;
    vTaskDelete(NULL);
}

/* ========== 触发 OTA 流程（收到 MQTT ota/inform 后调用） ========== */
void onenet_ota_start(void)
{
    if (s_ota_running) {
        ESP_LOGW(TAG, "OTA already in progress, ignore duplicate inform");
        return;
    }
    s_ota_running = true;
    ESP_LOGI(TAG, "OTA inform received, starting OTA task...");

    BaseType_t ok = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Create ota_task failed");
        s_ota_running = false;
    }
}

bool onenet_ota_is_running(void)
{
    return s_ota_running;
}

/* ========== 初始化 ========== */
void onenet_ota_init(const char *product_id, const char *device_name,
                     const char *token)
{
    strncpy(s_product_id, product_id, sizeof(s_product_id) - 1);
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
    strncpy(s_token, token, sizeof(s_token) - 1);
    s_ota_running = false;
    ESP_LOGI(TAG, "OTA init done (pid=%s, dev=%s)", s_product_id, s_device_name);
}
