#ifndef __WS2812_H__
#define __WS2812_H__

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/rmt.h"

/**
 * @brief 初始化 WS2812 RGB 灯（RMT 驱动）
 * @param gpio_pin  GPIO 引脚（如 GPIO_NUM_48）
 * @param ch        RMT 通道（如 RMT_CHANNEL_0）
 */
void ws2812_init(gpio_num_t gpio_pin, rmt_channel_t ch);

/**
 * @brief 设置 RGB 灯颜色（GRB 顺序自动转换）
 * @param r 红色 0-255
 * @param g 绿色 0-255
 * @param b 蓝色 0-255（全 0 即熄灭）
 */
void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b);

#endif /* __WS2812_H__ */
