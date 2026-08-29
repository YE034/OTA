#ifndef __ONENET_MQTT_H__
#define __ONENET_MQTT_H__

#include <stdbool.h>

extern bool s_mqtt_connected;

/**
 * @brief 物模型属性设置下行（单个属性）
 */
typedef struct {
    const char *identifier;   /* 属性标识符，如 "light_switch" / "color" / "temp" */
    bool        is_string;    /* 值是否为字符串类型 */
    double      value;        /* 数值：number 类型用实际值，bool 类型用 1/0 */
    const char *str_value;    /* 字符串值（is_string = true 时有效） */
} onenet_property_t;

/**
 * @brief 属性设置下行回调类型
 */
typedef void (*onenet_property_set_cb_t)(const onenet_property_t *prop);

/**
 * @brief 启动 OneNET MQTT 客户端并连接云平台
 */
void onenet_mqtt_start(const char *product_id, const char *device_name, const char *token);

/**
 * @brief 优雅停止 MQTT 客户端（OTA 重启前调用，避免强制掐断 TCP 产生错误日志）
 */
void onenet_mqtt_stop(void);

/**
 * @brief 上报温度属性到 OneNET 物模型
 */
bool onenet_mqtt_report_temp(const char *product_id, const char *device_name,
                              const char *identifier, float temperature);

/**
 * @brief 注册属性设置下行回调（云端下发指令时被调用）
 */
void onenet_mqtt_register_property_cb(onenet_property_set_cb_t cb);

#endif /* __ONENET_MQTT_H__ */
