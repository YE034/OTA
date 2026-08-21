#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/rmt.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_rgb";

/* ========== Wi-Fi 配置：改成你手机热点的信息 ========== */
#define WIFI_SSID      "led"
#define WIFI_PASS      "12345678"
/* ====================================================== */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_event_group;

/* ========== RGB / WS2812 配置 ========== */
#define WS2812_GPIO_PIN GPIO_NUM_48
#define RMT_TX_CHANNEL  RMT_CHANNEL_0

#define WS2812_T0H_NS  350
#define WS2812_T0L_NS  1000
#define WS2812_T1H_NS  1000
#define WS2812_T1L_NS  350
#define RMT_TICK_NS    100   // clk_div=8, 80MHz/8=10MHz → 1 tick = 100ns

#define NS_TO_TICKS(ns) ((ns) / RMT_TICK_NS)

static uint32_t t0h, t0l, t1h, t1l;

/* RMT 编码器回调：把 GRB 字节流转成 RMT items */
static void ws2812_encode(const void *src, rmt_item32_t *dest,
                          size_t src_size, size_t max_items,
                          size_t *out_size, size_t *out_items)
{
    const uint8_t *p = src;
    rmt_item32_t *d = dest;
    size_t n = 0, s = 0;
    const rmt_item32_t bit0 = {{{ t0h, 1, t0l, 0 }}};
    const rmt_item32_t bit1 = {{{ t1h, 1, t1l, 0 }}};

    while (s < src_size && n < max_items) {
        for (int b = 7; b >= 0; b--, n++, d++) {
            *d = (*p & (1 << b)) ? bit1 : bit0;
        }
        s++; p++;
    }
    *out_size = s;
    *out_items = n;
}

static void ws2812_init_hw(void)
{
    t0h = NS_TO_TICKS(WS2812_T0H_NS);
    t0l = NS_TO_TICKS(WS2812_T0L_NS);
    t1h = NS_TO_TICKS(WS2812_T1H_NS);
    t1l = NS_TO_TICKS(WS2812_T1L_NS);

    rmt_config_t c = {0};
    c.rmt_mode = RMT_MODE_TX;
    c.channel  = RMT_TX_CHANNEL;
    c.gpio_num = WS2812_GPIO_PIN;
    c.clk_div  = 8;
    c.mem_block_num = 1;
    c.tx_config.loop_en = false;
    c.tx_config.carrier_en = false;
    c.tx_config.idle_output_en = true;
    c.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    ESP_ERROR_CHECK(rmt_config(&c));
    ESP_ERROR_CHECK(rmt_driver_install(c.channel, 0, 0));
    ESP_LOGI(TAG, "WS2812 ready on GPIO %d", WS2812_GPIO_PIN);
}

static void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = {g, r, b};          // WS2812 是 GRB 顺序
    rmt_item32_t items[24];
    size_t sz = 0, cnt = 0;
    ws2812_encode(grb, items, 3, 24, &sz, &cnt);
    rmt_write_items(RMT_TX_CHANNEL, items, cnt, true);
    vTaskDelay(pdMS_TO_TICKS(1));         // 复位脉冲
}

/* ========== Wi-Fi 事件处理 ========== */
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA started, connecting...");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, retrying...");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // 断网 → 红色闪烁
        ws2812_set(255, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        ws2812_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "✅ Connected! IP address: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        // 连上 → 蓝色常亮
        ws2812_set(0, 0, 255);
    }
}

/* ========== Wi-Fi 初始化 ========== */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t wcfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi init done, SSID: %s", WIFI_SSID);
}

/* ========== 主函数 ========== */
void app_main(void)
{
    ESP_LOGI(TAG, "Booting...");

    /* 1. 初始化 NVS（Wi-Fi 必须） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 初始化 RGB */
    ws2812_init_hw();
    ws2812_set(255, 255, 0);   // 启动中：黄色

    /* 3. 初始化 Wi-Fi STA */
    wifi_init_sta();

    /* 4. 主循环：等待联网，期间黄灯慢闪 */
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(500));

        if (bits & WIFI_CONNECTED_BIT) {
            // 已连接，保持蓝灯，定期打印 IP
            esp_netif_ip_info_t ip;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
                ESP_LOGI(TAG, "Current IP: " IPSTR, IP2STR(&ip.ip));
            }
            vTaskDelay(pdMS_TO_TICKS(5000));   // 每 5 秒打印一次
        } else {
            // 未连接：黄灯闪烁
            ws2812_set(255, 255, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
            ws2812_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}