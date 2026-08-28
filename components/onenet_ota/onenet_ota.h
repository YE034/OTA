#ifndef __ONENET_OTA_H__
#define __ONENET_OTA_H__

#include "esp_err.h"

/**
 * @brief 初始化 OTA 组件（保存产品ID/设备名/token）
 */
void onenet_ota_init(const char *product_id, const char *device_name, const char *token);

/**
 * @brief 上报设备当前固件版本号到 OneNET（启动时调用一次）
 */
esp_err_t onenet_ota_report_version(void);

/**
 * @brief 触发 OTA 升级流程（收到云端 ota/inform 通知后调用）
 *        内部创建独立任务执行：检测任务 → 下载固件 → 写入Flash → 重启
 */
void onenet_ota_start(void);

#endif /* __ONENET_OTA_H__ */
