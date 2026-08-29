#ifndef __ONENET_OTA_H__
#define __ONENET_OTA_H__

#include <stdbool.h>
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
 *        内部创建独立任务：检测任务 → 下载固件(带进度上报/MD5校验)
 *        → 写入Flash → 上报升级成功 → 重启
 *        带防重入保护，重复调用会被忽略
 */
void onenet_ota_start(void);

/**
 * @brief 查询 OTA 是否正在进行（下载/刷写期间为 true）
 *        其他任务可据此暂停周期性上报，避免资源竞争
 */
bool onenet_ota_is_running(void);

/**
 * @brief 预重启回调类型：OTA 写入成功、即将 esp_restart 前调用
 *        用于优雅关闭 MQTT / WiFi，避免强制重启掐断 socket 产生错误日志
 */
typedef void (*onenet_pre_reboot_cb_t)(void);

/**
 * @brief 注册预重启回调（由应用层实现，内部关闭 MQTT/WiFi）
 */
void onenet_ota_set_pre_reboot_cb(onenet_pre_reboot_cb_t cb);

#endif /* __ONENET_OTA_H__ */
