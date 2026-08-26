/* 标准 C 库头文件优先 */
#include <string.h>

/* ESP-IDF 系统头文件 */
#include "driver/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* 项目头文件 */
#include "ws2812.h"

static const char *TAG = "ws2812";

/* WS2812 时序（ns） */
#define T0H_NS      350
#define T0L_NS      1000
#define T1H_NS      1000
#define T1L_NS      350

#define RMT_CLK_DIV 8      /* 80MHz / 8 = 10MHz -> 1 tick = 100ns */
#define RMT_TICK_NS 100
#define NS_TO_TICKS(ns) ((ns) / RMT_TICK_NS)

static uint32_t s_t0h, s_t0l, s_t1h, s_t1l;
static rmt_channel_t s_rmt_ch = RMT_CHANNEL_0;

/* RMT 编码：把 GRB 字节流转成 RMT item（每个 bit 一个 item） */
static void ws2812_encode(const void *src, rmt_item32_t *dest,
                          size_t src_size, size_t max_items,
                          size_t *out_size, size_t *out_items)
{
    const uint8_t *p = src;
    rmt_item32_t *d = dest;
    size_t n = 0, s = 0;
    const rmt_item32_t bit0 = {{{ s_t0h, 1, s_t0l, 0 }}};
    const rmt_item32_t bit1 = {{{ s_t1h, 1, s_t1l, 0 }}};

    while (s < src_size && n < max_items) {
        for (int b = 7; b >= 0; b--, n++, d++) {
            *d = (*p & (1 << b)) ? bit1 : bit0;
        }
        s++; p++;
    }
    *out_size = s;
    *out_items = n;
}

void ws2812_init(gpio_num_t gpio_pin, rmt_channel_t ch)
{
    s_rmt_ch = ch;
    s_t0h = NS_TO_TICKS(T0H_NS);
    s_t0l = NS_TO_TICKS(T0L_NS);
    s_t1h = NS_TO_TICKS(T1H_NS);
    s_t1l = NS_TO_TICKS(T1L_NS);

    rmt_config_t c = {0};
    c.rmt_mode        = RMT_MODE_TX;
    c.channel         = ch;
    c.gpio_num        = gpio_pin;
    c.clk_div         = RMT_CLK_DIV;
    c.mem_block_num   = 1;
    c.tx_config.loop_en        = false;
    c.tx_config.carrier_en     = false;
    c.tx_config.idle_output_en = true;
    c.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;

    ESP_ERROR_CHECK(rmt_config(&c));
    ESP_ERROR_CHECK(rmt_driver_install(c.channel, 0, 0));
    ESP_LOGI(TAG, "WS2812 ready on GPIO %d, RMT ch%d", gpio_pin, ch);

    ws2812_set_color(0, 0, 0);
}

void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };       /* WS2812 是 GRB 顺序 */
    rmt_item32_t items[24];
    size_t sz = 0, cnt = 0;
    ws2812_encode(grb, items, 3, 24, &sz, &cnt);
    rmt_write_items(s_rmt_ch, items, cnt, true);
    vTaskDelay(pdMS_TO_TICKS(1));       /* 复位脉冲 */
}
