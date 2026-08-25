/* 标准 C 库头文件优先 */
#include <string.h>

/* ESP-IDF 系统头文件 */
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

/* 项目头文件 */
#include "onenet_mqtt.h"

static const char *TAG = "onenet_mqtt";

bool s_mqtt_connected = false;
static esp_mqtt_client_handle_t s_client = NULL;
static int s_msg_id_counter = 0;

/* 缓存 product_id / device_name，事件回调中拼接 topic 用 */
static char s_product_id[64] = {0};
static char s_device_name[64] = {0};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connected to OneNET");
            {
                char topic[160];
                snprintf(topic, sizeof(topic),
                         "$sys/%s/%s/thing/property/post/reply",
                         s_product_id, s_device_name);
                esp_mqtt_client_subscribe(s_client, topic, 0);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected, will auto-reconnect");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Reply topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Reply data:  %.*s", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error, check broker address / token / network");
            break;
        default:
            break;
    }
}

void onenet_mqtt_start(const char *product_id, const char *device_name, const char *token)
{
    strncpy(s_product_id, product_id, sizeof(s_product_id) - 1);
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);

    /* OneNET CMIoT 新版平台：{product_id}.mqtts.acc.cmcconenet.cn
     * 注意域名是 cmcc+onenet = cmcconenet，不是 cmccconet！
     * mqtt://  + 端口1883 = 非加密（新手推荐，无需证书）
     * mqtts:// + 端口8883 = TLS加密（需额外配置CA证书） */
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s.mqtts.acc.cmcconenet.cn", product_id);

    esp_mqtt_client_config_t cfg = {
        .uri       = uri,
        .port      = 1883,
        .client_id = device_name,
        .username  = product_id,
        .password  = token,
        .keepalive = 60,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client starting, broker: %s:1883", uri);
}

bool onenet_mqtt_report_temp(const char *product_id, const char *device_name,
                              const char *identifier, float temperature)
{
    if (!s_mqtt_connected || s_client == NULL) {
        ESP_LOGW(TAG, "MQTT not ready, skip report");
        return false;
    }

    char topic[160];
    snprintf(topic, sizeof(topic),
             "$sys/%s/%s/thing/property/post",
             product_id, device_name);

    char payload[256];
    s_msg_id_counter++;
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{"
             "\"%s\":{\"value\":%.2f}}}",
             s_msg_id_counter, identifier, temperature);

    ESP_LOGI(TAG, "Publishing to %s", topic);
    ESP_LOGI(TAG, "Payload: %s", payload);
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
    if (msg_id == -1) {
        ESP_LOGE(TAG, "Publish failed");
        return false;
    }
    return true;
}
