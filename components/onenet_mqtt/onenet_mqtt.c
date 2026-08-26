/* 标准 C 库头文件优先 */
#include <string.h>
#include <stdlib.h>

/* ESP-IDF 系统头文件 */
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

/* 项目头文件 */
#include "onenet_mqtt.h"

static const char *TAG = "onenet_mqtt";

bool s_mqtt_connected = false;
static esp_mqtt_client_handle_t s_client = NULL;
static int s_msg_id_counter = 0;

/* 缓存 product_id / device_name，事件回调中拼接 topic 用 */
static char s_product_id[64] = {0};
static char s_device_name[64] = {0};

/* 属性设置下行回调（应用层注册） */
static onenet_property_set_cb_t s_property_cb = NULL;

void onenet_mqtt_register_property_cb(onenet_property_set_cb_t cb)
{
    s_property_cb = cb;
}

/* 判断收到的 topic 是否为「属性设置」下行：$sys/{pid}/{dn}/thing/property/set
 * 注意：MQTT 事件的 topic 不保证以 '\0' 结尾，必须用 topic_len + memcmp 判断，
 * 不能用 strlen/strcmp，否则会读到 topic 后面的垃圾内存导致判断失败。 */
static bool is_property_set_topic(const char *topic, int topic_len)
{
    if (topic == NULL || topic_len <= 0) {
        return false;
    }
    const char suffix[] = "/thing/property/set";
    size_t slen = sizeof(suffix) - 1;   /* 去掉结尾的 '\0' */
    if ((size_t)topic_len < slen) {
        return false;
    }
    return (memcmp(topic + topic_len - slen, suffix, slen) == 0);
}

/* 处理云端下发的属性设置指令 */
static void handle_property_set(esp_mqtt_event_handle_t event)
{
    char *data = malloc(event->data_len + 1);
    if (data == NULL) {
        ESP_LOGE(TAG, "OOM parsing property set");
        return;
    }
    memcpy(data, event->data, event->data_len);
    data[event->data_len] = '\0';
    ESP_LOGI(TAG, "Property set received: %s", data);

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse property set JSON");
        return;
    }

    const char *req_id = "";
    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id_item) && id_item->valuestring != NULL) {
        req_id = id_item->valuestring;
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (cJSON_IsObject(params) && s_property_cb != NULL) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, params) {
            onenet_property_t prop;
            prop.identifier = item->string;
            prop.is_string  = cJSON_IsString(item);
            prop.value      = 0;
            prop.str_value  = NULL;
            if (cJSON_IsNumber(item)) {
                prop.value = item->valuedouble;
            } else if (cJSON_IsBool(item)) {
                prop.value = item->valueint ? 1.0 : 0.0;   /* true -> 1, false -> 0 */
            } else if (cJSON_IsString(item)) {
                prop.str_value = item->valuestring;
            }
            ESP_LOGI(TAG, "  param: %s %s",
                     prop.identifier,
                     prop.is_string ? prop.str_value : "");
            s_property_cb(&prop);
        }
    } else if (!cJSON_IsObject(params)) {
        ESP_LOGW(TAG, "Property set has no valid params object");
    }

    /* 回复云端：property/set_reply
     * 注意 OneNET 协议：属性设置回复 topic 是 set_reply（下划线！），
     * 不是 set/reply（斜杠）。写错 topic 会被 broker 直接断开连接！ */
    char reply_topic[160];
    snprintf(reply_topic, sizeof(reply_topic),
             "$sys/%s/%s/thing/property/set_reply",
             s_product_id, s_device_name);
    char reply[192];
    snprintf(reply, sizeof(reply),
             "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", req_id);
    esp_mqtt_client_publish(s_client, reply_topic, reply, 0, 0, 0);
    ESP_LOGI(TAG, "Sent set reply: %s", reply);

    cJSON_Delete(root);
}

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
                /* 订阅温度上报回复 */
                snprintf(topic, sizeof(topic),
                         "$sys/%s/%s/thing/property/post/reply",
                         s_product_id, s_device_name);
                esp_mqtt_client_subscribe(s_client, topic, 0);
                /* 订阅云端属性设置下行（控制 RGB 灯） */
                snprintf(topic, sizeof(topic),
                         "$sys/%s/%s/thing/property/set",
                         s_product_id, s_device_name);
                esp_mqtt_client_subscribe(s_client, topic, 0);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected, will auto-reconnect");
            break;
        case MQTT_EVENT_DATA:
            /* 区分：属性设置下行 vs 其他回复 */
            if (is_property_set_topic(event->topic, event->topic_len)) {
                handle_property_set(event);
            } else {
                ESP_LOGI(TAG, "Reply topic: %.*s", event->topic_len, event->topic);
                ESP_LOGI(TAG, "Reply data:  %.*s", event->data_len, event->data);
            }
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
             "\"%s\":{\"value\":%.1f}}}",
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
