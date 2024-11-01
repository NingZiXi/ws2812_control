/**
 * @file ws2812_control.c
 * @author 宁子希
 * @brief    WS2812灯条控制
 * @version 0.1
 * @date 2024-08-31
 * 
 * @copyright Copyright (c) 2024
 * 
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt.h"
#include "led_strip.h"
#include "ws2812_control.h"

static const char *TAG = "WS2812_control";

#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define EXAMPLE_CHASE_SPEED_MS (240)


/**
*@brief 简单的辅助函数，将 HSV 颜色空间转换为 RGB 颜色空间
*
*@param h 色相值，范围为[0,360]
*@param s 饱和度值，范围为[0,100]
*@param v 亮度值，范围为[0,100]
*@param r 指向存储红色值的指针
*@param g 指向存储绿色值的指针
*@param b 指向存储蓝色值的指针
*
*/
void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

// 设置 LED 颜色(单个)
void set_led_color(led_strip_t *strip,int index, led_color_t color) {
    if (index >= 0 && index < CONFIG_EXAMPLE_STRIP_LED_NUMBER) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, index,color.red, color.green, color.blue));
    }
    ESP_ERROR_CHECK(strip->refresh(strip, 100));
}

// LED 全部常亮
void led_set_on(led_strip_t *strip, led_color_t color) {
    for (int i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, i, color.red, color.green, color.blue));
    }
    ESP_ERROR_CHECK(strip->refresh(strip, 100));
}

// LED 关闭
void led_set_off(led_strip_t *strip) {
    for (int i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, i, 0, 0, 0)); // 设置为黑色
    }
    ESP_ERROR_CHECK(strip->refresh(strip, 100));
}

// LED 呼吸效果
void led_set_breath(led_strip_t *strip, led_color_t color) {
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < CONFIG_EXAMPLE_STRIP_LED_NUMBER; j++) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, j, (color.red * i) / 255, (color.green * i) / 255, (color.blue * i) / 255));
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        vTaskDelay(pdMS_TO_TICKS(10)); // 调整呼吸速度
    }
    for (int i = 255; i >= 0; i--) {
        for (int j = 0; j < CONFIG_EXAMPLE_STRIP_LED_NUMBER; j++) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, j, (color.red * i) / 255, (color.green * i) / 255, (color.blue * i) / 255));
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        vTaskDelay(pdMS_TO_TICKS(10)); // 调整呼吸速度
    }
}

// LED 缓缓亮起
void led_set_fade_in(led_strip_t *strip, led_color_t color) {
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < CONFIG_EXAMPLE_STRIP_LED_NUMBER; j++) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, j, (color.red * i) / 255, (color.green * i) / 255, (color.blue * i) / 255));
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        vTaskDelay(pdMS_TO_TICKS(20)); // 调整渐变速度
    }
}

// LED 微微闪烁
void led_set_blink_slow(led_strip_t *strip, led_color_t color, int count) {
    while (count--) {
        led_set_on(strip, color);
        vTaskDelay(pdMS_TO_TICKS(500)); // 闪烁间隔
        led_set_off(strip);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// LED 快速闪烁
void led_set_blink_fast(led_strip_t *strip, led_color_t color, int count) {
    while (count--) {
        led_set_on(strip, color);
        vTaskDelay(pdMS_TO_TICKS(100)); // 闪烁间隔
        led_set_off(strip);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// 彩虹效果
void led_set_rainbow(led_strip_t *strip) {
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    uint16_t hue = 0;
    uint16_t start_rgb = 0;

    ESP_LOGI(TAG, "LED Rainbow Chase Start");
    while (true) {
        for (int i = 0; i < 3; i++) {
            for (int j = i; j < CONFIG_EXAMPLE_STRIP_LED_NUMBER; j += 3) {
                // 构建RGB值
                hue = j * 360 / CONFIG_EXAMPLE_STRIP_LED_NUMBER + start_rgb;
                led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
                // 将RGB值写入LED条驱动
                ESP_ERROR_CHECK(strip->set_pixel(strip, j, red, green, blue));
            }
            // 刷新RGB值到LED
            ESP_ERROR_CHECK(strip->refresh(strip, 100));
            vTaskDelay(pdMS_TO_TICKS(EXAMPLE_CHASE_SPEED_MS));
        }
        start_rgb += 60;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void ws2812_set(led_strip_t *strip, led_color_t color, led_effect_t effect){
        // 根据不同的效果值执行相应的 LED 控制操作
    switch (effect) {
        case LED_EFFECT_ON:
            //设置灯条状态为开启，并设置颜色
            led_set_on(strip, color);
            break;
        case LED_EFFECT_OFF:
            //关闭灯条
            led_set_off(strip);
            break;
        case LED_EFFECT_BREATH:
            // 设置呼吸效果
            led_set_breath(strip, color);
            break;
        case LED_EFFECT_FADE_IN:
            //设置渐入效果
            led_set_fade_in(strip, color);
            break;
        case LED_EFFECT_BLINK_SLOW:
            //设置缓慢闪烁效果
            led_set_blink_slow(strip, color,15);
            break;
        case LED_EFFECT_BLINK_FAST:
            // 快速闪烁效果
            led_set_blink_fast(strip, color,15);
            break;
        case LED_EFFECT_RAINBOW:
            // 彩虹效果
            led_set_rainbow(strip);
            break;
    }
}

// 设置 LED 跑马灯（从 index_start 到 index_end）
void set_led_color_gradient(led_strip_t *strip, int index_start, int index_end, led_color_t color, int delay_ms) {

    if (index_start < 0 || index_end < 0 || index_start >= CONFIG_EXAMPLE_STRIP_LED_NUMBER || index_end >= CONFIG_EXAMPLE_STRIP_LED_NUMBER ) {
        ESP_LOGE(TAG, "无效的 LED 索引");
        return; // 参数检查
    }

    // 逐个亮起 LED
    if (index_start < index_end) {
        // 从 index_start 到 index_end
        for (int i = index_start; i <= index_end; i++) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, i, color.red, color.green, color.blue));
            ESP_ERROR_CHECK(strip->refresh(strip, delay_ms)); // 刷新显示
            
            // 延迟以控制亮起速度
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    } else {
        // 从 index_start 到 index_end
        for (int i = index_start; i >= index_end; i--) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, i, color.red, color.green, color.blue));
            ESP_ERROR_CHECK(strip->refresh(strip, delay_ms)); // 刷新显示
            
            // 延迟以控制亮起速度
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    // 确保最终的 LED 关闭
    led_set_off(strip);
}

// 更新LED显示
void update_led_display(led_strip_t *strip) {
    strip->refresh(strip, 100);
}

// 创建WS2812控制句柄
led_strip_t* ws2812_create() {
    // 初始化WS2812控制任务
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(CONFIG_EXAMPLE_RMT_TX_GPIO, RMT_TX_CHANNEL);
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(CONFIG_EXAMPLE_STRIP_LED_NUMBER, (led_strip_dev_t)config.channel);
    led_strip_t *strip = led_strip_new_rmt_ws2812(&strip_config);

    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
    }

    ESP_ERROR_CHECK(strip->clear(strip, 100));
    return strip;
}


