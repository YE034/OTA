#ifndef __BLE_MANAGER_H__
#define __BLE_MANAGER_H__

/*
 * BLE 管理器（纯通用 GATT，手机用 nRF Connect 即可，无需 EspBlufi）
 *
 * 提供两类能力：
 *   1. 蓝牙配网：手机往命令特征写 "WIFI:热点名,密码"，设备保存到 NVS 并联网
 *   2. 蓝牙直连：读温度/订阅温度推送、写命令控灯
 *
 * 控灯类命令（ON/OFF/颜色）通过 ble_command_cb_t 回调交给应用层处理；
 * WIFI:/STATE? 等配网与查询命令由本组件内部处理，不上抛。
 */

/* 控灯命令回调：参数为大写化后的命令字符串，如 "ON"/"RED"/"255,0,0" */
typedef void (*ble_command_cb_t)(const char *cmd);

/* 初始化 BLE 并开始广播；device_name 为手机扫描到的广播名 */
void ble_manager_init(const char *device_name);

/* 注册控灯命令回调（配网命令不会进入此回调） */
void ble_manager_register_command_cb(ble_command_cb_t cb);

/* 更新当前温度（供温度特征 Read/Notify 使用） */
void ble_manager_set_temperature(float temp_c);

#endif /* __BLE_MANAGER_H__ */
