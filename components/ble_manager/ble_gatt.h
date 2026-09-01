#ifndef __BLE_GATT_H__
#define __BLE_GATT_H__

#include <stdint.h>
#include <stdbool.h>

/* 手机往「命令特征」写入字符串时的统一回调（由 ble_manager 注册并解析协议） */
typedef void (*ble_gatt_write_cb_t)(const char *cmd);

/* 连接状态变化回调：connected=true 手机连上，false 手机断开 */
typedef void (*ble_gatt_conn_cb_t)(bool connected);

/* 协议栈就绪回调：GATTS 注册成功后触发，用于此时再启动广播（官方推荐时序） */
typedef void (*ble_gatt_ready_cb_t)(void);

/* 初始化自定义 GATT 服务，需在 Bluedroid 使能后调用 */
void ble_gatt_init(void);

/* 更新当前温度：刷新特征值，若手机已订阅 Notify 则主动推送 */
void ble_gatt_set_temperature(float temp_c);

/* 向「状态特征」推送文本（配网结果 / IP / 查询应答），已订阅才会真正发出 */
void ble_gatt_notify_status(const char *text);

/* 注册命令写入回调 */
void ble_gatt_register_write_cb(ble_gatt_write_cb_t cb);

/* 注册连接状态变化回调（用于断开后重启广播） */
void ble_gatt_register_conn_cb(ble_gatt_conn_cb_t cb);

/* 注册协议栈就绪回调 */
void ble_gatt_register_ready_cb(ble_gatt_ready_cb_t cb);

#endif /* __BLE_GATT_H__ */
