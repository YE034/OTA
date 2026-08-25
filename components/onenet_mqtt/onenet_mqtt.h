#ifndef __ONENET_MQTT_H__
#define __ONENET_MQTT_H__

#include <stdbool.h>

extern bool s_mqtt_connected;

void onenet_mqtt_start(const char *product_id, const char *device_name, const char *token);
bool onenet_mqtt_report_temp(const char *product_id, const char *device_name,
                              const char *identifier, float temperature);

#endif /* __ONENET_MQTT_H__ */
