#ifndef __WIFI_H__
#define __WIFI_H__

/* 必须先包含 FreeRTOS.h，再包含 event_groups.h，否则标准类型未定义 */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include <stdbool.h>

extern EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0

/**
 * @brief 初始化 Wi-Fi STA（netif / 默认事件循环 / wifi 启动）。
 *        自动从 NVS 读取已保存的凭证：有则立即连接，无则只启动 STA 等待 BLE 配网。
 */
void wifi_init_sta(void);

/**
 * @brief NVS 中是否已保存 Wi-Fi 凭证
 */
bool wifi_config_has_saved(void);

/**
 * @brief 保存 Wi-Fi 凭证到 NVS（掉电不丢失，下次开机自动连接）
 */
esp_err_t wifi_config_save(const char *ssid, const char *password);

/**
 * @brief BLE 配网成功后调用：保存凭证并立即发起连接
 */
esp_err_t wifi_connect_with(const char *ssid, const char *password);

#endif /* __WIFI_H__ */
