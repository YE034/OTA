#ifndef __WIFI_H__
#define __WIFI_H__

/* 必须先包含 FreeRTOS.h，再包含 event_groups.h，否则标准类型未定义 */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

extern EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0

void wifi_init_sta(const char *ssid, const char *password);

#endif /* __WIFI_H__ */
