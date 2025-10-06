/**
 * @file ws2812_control.c
 * @author 宁子希 (1589326497@qq.com)
 * @brief    WS2812灯条和矩阵屏幕控制 依赖led_strip库
 * @version 1.3.0 
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
#include "esp_timer.h"

static const char *TAG = "WS2812_control";

#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define EXAMPLE_CHASE_SPEED_MS (240)

// 定时器回调函数指针类型
typedef void (*led_effect_func_t)(ws2812_strip_t*, led_color_t);

// 定时器状态结构体
typedef struct {
    ws2812_strip_t* strip;
    led_color_t color;
    led_effect_func_t effect_func;
    int counter;
    bool active;
} timer_effect_state_t;

// 全局定时器状态和句柄
static timer_effect_state_t timer_state;
static esp_timer_handle_t led_timer;

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

/**
*@brief 简单的辅助函数，将 RGB 颜色空间转换为 HSV 颜色空间
*
*@param r 红色值，范围为[0,255]
*@param g 绿色值，范围为[0,255]
*@param b 蓝色值，范围为[0,255]
*@param h 指向存储色相值的指针
*@param s 指向存储饱和度值的指针
*@param v 指向存储亮度值的指针
*
*/
void led_strip_rgb2hsv(uint32_t r, uint32_t g, uint32_t b, uint32_t *h, uint32_t *s, uint32_t *v)
{
    uint32_t rgb_max = (r > g && r > b) ? r : (g > b ? g : b);
    uint32_t rgb_min = (r < g && r < b) ? r : (g < b ? g : b);
    uint32_t delta = rgb_max - rgb_min;
    
    // 计算亮度值
    *v = rgb_max / 2.55f;
    
    // 如果最大值为0，则饱和度和色相都为0
    if (rgb_max == 0) {
        *s = 0;
        *h = 0;
        return;
    }
    
    // 计算饱和度
    *s = (delta * 100) / rgb_max;
    
    // 如果delta为0，则色相为0
    if (delta == 0) {
        *h = 0;
        return;
    }
    
    // 计算色相
    if (rgb_max == r) {
        *h = (60 * ((int32_t)(g - b) / (int32_t)delta) + 360) % 360;
    } else if (rgb_max == g) {
        *h = (60 * ((int32_t)(b - r) / (int32_t)delta) + 120) % 360;
    } else {
        *h = (60 * ((int32_t)(r - g) / (int32_t)delta) + 240) % 360;
    }
}

// 定时器回调函数
static void led_timer_callback(void* arg){
    if (timer_state.active && timer_state.effect_func) {
        timer_state.effect_func(timer_state.strip, timer_state.color);
        timer_state.counter++;
    }
}

//ws2812 灯带模式
#ifdef CONFIG_WS2812_MODE_STRIP

// 创建WS2812控制句柄
ws2812_strip_t* ws2812_create() {
    // 初始化WS2812控制任务
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)CONFIG_WS2812_TX_GPIO, RMT_TX_CHANNEL);
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(CONFIG_WS2812_STRIP_LED_NUMBER, (led_strip_dev_t)config.channel);
    ws2812_strip_t *strip = led_strip_new_rmt_ws2812(&strip_config);

    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
    }

    ESP_ERROR_CHECK(strip->clear(strip, 100));

    // 初始化定时器
    esp_timer_create_args_t timer_args = {
        .callback = &led_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &led_timer));

    return strip;
}

// 设置 LED 颜色(单个)
void led_set_pixel(ws2812_strip_t *strip,int index, led_color_t color) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }
    if (index >= 0 && index < CONFIG_WS2812_STRIP_LED_NUMBER) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, index,color.red, color.green, color.blue));
    }
    ESP_ERROR_CHECK(strip->refresh(strip, 100));
}

// LED 全部常亮
void led_set_on(ws2812_strip_t *strip, led_color_t color) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }
    for (int i = 0; i < CONFIG_WS2812_STRIP_LED_NUMBER; i++) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, i, color.red, color.green, color.blue));
    }
    ESP_ERROR_CHECK(strip->refresh(strip, 100));
}

// LED 关闭
void led_set_off(ws2812_strip_t *strip) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }
    for (int i = 0; i < CONFIG_WS2812_STRIP_LED_NUMBER; i++) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, i, 0, 0, 0)); // 设置为黑色
    }
    ESP_ERROR_CHECK(strip->refresh(strip, 100));
}

// LED 呼吸效果
void led_set_breath(ws2812_strip_t *strip, led_color_t color, uint32_t interval_ms) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }

    // 设置定时器状态
    timer_state.strip = strip;
    timer_state.color = color;
    timer_state.active = true;
    timer_state.counter = 0;

    timer_state.effect_func = [](ws2812_strip_t* strip, led_color_t color) {
        static int brightness = 0;
        static bool increasing = true;
        
        // 更新亮度
        if (increasing) {
            brightness++;
            if (brightness >= 255) increasing = false;
        } else {
            brightness--;
            if (brightness <= 0) increasing = true;
        }
        
        // 设置LED亮度
        for (int j = 0; j < CONFIG_WS2812_STRIP_LED_NUMBER; j++) {
            ESP_ERROR_CHECK(strip->set_pixel(
                strip, j,
                (color.red * brightness) / 255,
                (color.green * brightness) / 255,
                (color.blue * brightness) / 255
            ));
        }
        strip->refresh(strip, 100);
    };

    // 启动定时器，使用传入的时间间隔
    ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, interval_ms * 1000)); // 转换为微秒
}

// LED 缓缓亮起
void led_set_fade_in(ws2812_strip_t *strip, led_color_t color) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < CONFIG_WS2812_STRIP_LED_NUMBER; j++) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, j, (color.red * i) / 255, (color.green * i) / 255, (color.blue * i) / 255));
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        vTaskDelay(pdMS_TO_TICKS(20)); // 调整渐变速度
    }
}

// 闪烁效果
void led_set_blink(ws2812_strip_t *strip, led_color_t color, uint32_t interval_ms) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }

    // 设置定时器状态
    timer_state.strip = strip;
    timer_state.color = color;
    timer_state.active = true;
    timer_state.counter = 0;

    timer_state.effect_func = [](ws2812_strip_t* strip, led_color_t color) {
        static bool is_on = false;
        
        if (is_on) {
            for (int i = 0; i < CONFIG_WS2812_STRIP_LED_NUMBER; i++) {
                ESP_ERROR_CHECK(strip->set_pixel(strip, i, 0, 0, 0)); // 设置为黑色
            }
            ESP_ERROR_CHECK(strip->refresh(strip, 100));
        } else {
            for (int i = 0; i < CONFIG_WS2812_STRIP_LED_NUMBER; i++) {
                ESP_ERROR_CHECK(strip->set_pixel(strip, i, color.red, color.green, color.blue));
            }
            ESP_ERROR_CHECK(strip->refresh(strip, 100));
        }
        is_on = !is_on;
    };

    // 启动定时器，使用传入的时间间隔
    ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, interval_ms * 1000)); // 转换为微秒
}

// 彩虹效果
void led_set_rainbow(ws2812_strip_t *strip, uint32_t interval_ms) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }

    // 设置定时器状态
    timer_state.strip = strip;
    timer_state.active = true;
    timer_state.counter = 0;

    timer_state.effect_func = [](ws2812_strip_t* strip, led_color_t color) {
        static uint16_t hue = 0;
        uint32_t red, green, blue;
        
        for (int i = 0; i < CONFIG_WS2812_STRIP_LED_NUMBER; i++) {
            uint16_t led_hue = hue + (i * 360 / CONFIG_WS2812_STRIP_LED_NUMBER);
            led_hue %= 360; // 确保色相值在0-359范围内
            led_strip_hsv2rgb(led_hue, 100, 100, &red, &green, &blue);
            ESP_ERROR_CHECK(strip->set_pixel(strip, i, red, green, blue));
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        hue = (hue + 1) % 360;
    };

    // 启动定时器
    ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, interval_ms * 1000));
}

void ws2812_set(ws2812_strip_t *strip, led_color_t color, led_effect_t effect){
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
            led_set_breath(strip, color ,10);
            break;
        case LED_EFFECT_FADE_IN:
            //设置渐入效果
            led_set_fade_in(strip, color);
            break;
        case LED_EFFECT_BLINK_SLOW:
            //设置缓慢闪烁效果
            led_set_blink(strip, color,500);
            break;
        case LED_EFFECT_BLINK_FAST:
            // 快速闪烁效果
            led_set_blink(strip, color,100);
            break;
        case LED_EFFECT_RAINBOW:
            // 彩虹效果
            led_set_rainbow(strip, 20);
            break;
    }
}

// 设置 LED 跑马灯（从 index_start 到 index_end）
void led_set_marquee(ws2812_strip_t *strip, int index_start, int index_end, led_color_t color, int delay_ms) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }
    if (index_start < 0 || index_end < 0 || index_start >= CONFIG_WS2812_STRIP_LED_NUMBER || index_end >= CONFIG_WS2812_STRIP_LED_NUMBER ) {
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

// 全局变量用于存储渐变效果的状态
static uint32_t gradient_start_h, gradient_start_s, gradient_start_v;
static uint32_t gradient_end_h, gradient_end_s, gradient_end_v;
static uint32_t gradient_steps;
static uint32_t gradient_current_step = 0;

// 渐变效果的定时器回调函数
void gradient_effect_func(ws2812_strip_t* strip, led_color_t color) {
    if (gradient_current_step <= gradient_steps) {
        // 计算当前步骤的HSV值
        uint32_t current_h, current_s, current_v;
        
        // 处理色相的最短路径插值
        int32_t hue_diff = (int32_t)gradient_end_h - (int32_t)gradient_start_h;
        if (hue_diff > 180) {
            hue_diff -= 360;
        } else if (hue_diff < -180) {
            hue_diff += 360;
        }
        
        current_h = (gradient_start_h + (hue_diff * (int32_t)gradient_current_step) / (int32_t)gradient_steps) % 360;
        current_s = gradient_start_s + ((int32_t)((gradient_end_s - gradient_start_s) * gradient_current_step) / (int32_t)gradient_steps);
        current_v = gradient_start_v + ((int32_t)((gradient_end_v - gradient_start_v) * gradient_current_step) / (int32_t)gradient_steps);
        
        // 将HSV转换为RGB
        uint32_t red, green, blue;
        led_strip_hsv2rgb(current_h, current_s, current_v, &red, &green, &blue);
        
        // 设置所有LED的颜色
        for (int i = 0; i < CONFIG_WS2812_STRIP_LED_NUMBER; i++) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, i, red, green, blue));
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        
        gradient_current_step++;
        
        // 如果达到最后一步，停止定时器
        if (gradient_current_step > gradient_steps) {
            gradient_current_step = 0; // 重置步骤计数器
            esp_timer_stop(led_timer);
        }
    }
}

// 从一个颜色渐变到另一个颜色
void led_set_color_gradient(ws2812_strip_t *strip, led_color_t start_color, led_color_t end_color, uint32_t duration_ms) {
    if (timer_state.active) {
        esp_timer_stop(led_timer);
    }

    // 将RGB颜色转换为HSV
    led_strip_rgb2hsv(start_color.red, start_color.green, start_color.blue, &gradient_start_h, &gradient_start_s, &gradient_start_v);
    led_strip_rgb2hsv(end_color.red, end_color.green, end_color.blue, &gradient_end_h, &gradient_end_s, &gradient_end_v);
    
    const uint32_t step_interval_ms = 20; // 50Hz刷新率
    
    gradient_steps = duration_ms / step_interval_ms;
    
    if (gradient_steps == 0) {
        gradient_steps = 1;
    }
    
    // 重置当前步骤计数器
    gradient_current_step = 0;
    
    // 设置定时器状态
    timer_state.strip = strip;
    timer_state.color = start_color;
    timer_state.effect_func = gradient_effect_func;
    timer_state.active = true;
    
    uint32_t interval_us = step_interval_ms * 1000;
    ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, interval_us));
}

// 更新LED显示
void update_led_display(ws2812_strip_t *strip) {
    strip->refresh(strip, 100);
}



#elif defined(CONFIG_WS2812_MODE_MATRIX)

ws2812_matrix_t* ws2812_matrix_create() {
    ws2812_matrix_t *matrix = (ws2812_matrix_t *)malloc(sizeof(ws2812_matrix_t));
    if (!matrix) {
        ESP_LOGE(TAG, "Failed to allocate memory for ws2812_matrix_t");
        return NULL;
    }

    matrix->config.matrixWidth = CONFIG_WS2812_MATRIX_WIDTH;
    matrix->config.matrixHeight = CONFIG_WS2812_MATRIX_HEIGHT;
    matrix->config.pin = CONFIG_WS2812_TX_GPIO;

    // 设置起始角落
    #if defined(CONFIG_WS2812_MATRIX_START_TOP_LEFT)
        matrix->config.matrixType = MATRIX_START_TOP_LEFT;
    #elif defined(CONFIG_WS2812_MATRIX_START_TOP_RIGHT)
        matrix->config.matrixType = MATRIX_START_TOP_RIGHT;
    #elif defined(CONFIG_WS2812_MATRIX_START_BOTTOM_LEFT)
        matrix->config.matrixType = MATRIX_START_BOTTOM_LEFT;
    #elif defined(CONFIG_WS2812_MATRIX_START_BOTTOM_RIGHT)
        matrix->config.matrixType = MATRIX_START_BOTTOM_RIGHT;
    #endif

    // 设置布局方向
    #if defined(CONFIG_WS2812_MATRIX_LAYOUT_HORIZONTAL)
        matrix->config.matrixType |= MATRIX_LAYOUT_HORIZONTAL;
    #elif defined(CONFIG_WS2812_MATRIX_LAYOUT_VERTICAL)
        matrix->config.matrixType |= MATRIX_LAYOUT_VERTICAL;
    #endif

    // 设置扫描顺序
    #if defined(CONFIG_WS2812_MATRIX_SCAN_PROGRESSIVE)
        matrix->config.matrixType |= MATRIX_SCAN_PROGRESSIVE;
    #elif defined(CONFIG_WS2812_MATRIX_SCAN_ZIGZAG)
        matrix->config.matrixType |= MATRIX_SCAN_ZIGZAG;
    #endif

    // 初始化RMT配置
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)CONFIG_WS2812_TX_GPIO, RMT_TX_CHANNEL);
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    // 初始化LED条配置
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(
        CONFIG_WS2812_MATRIX_WIDTH * CONFIG_WS2812_MATRIX_HEIGHT,
        (led_strip_dev_t)config.channel
    );
    matrix->strip = led_strip_new_rmt_ws2812(&strip_config);

    if (!matrix->strip) {
        ESP_LOGE(TAG, "Failed to create WS2812 strip");
        free(matrix);
        return NULL;
    }

    ESP_ERROR_CHECK(matrix->strip->clear(matrix->strip, 100));
    return matrix;
}


void led_matrix_set_pixel(ws2812_matrix_t *matrix, int x, int y, led_color_t color) {
    if (!matrix || !matrix->strip) {
        ESP_LOGE(TAG, "Matrix or strip handle is NULL");
        return;
    }

    // 检查坐标是否在矩阵范围内
    if (x < 0 || x >= matrix->config.matrixWidth || y < 0 || y >= matrix->config.matrixHeight) {
        ESP_LOGE(TAG, "Coordinates out of bounds");
        return;
    }

    int index;
    
    // 判断布局是行优先还是列优先
    if (matrix->config.matrixType & MATRIX_LAYOUT_VERTICAL) {
        // 列优先布局
        if (matrix->config.matrixType & MATRIX_SCAN_ZIGZAG) {
            // 之字形扫描
            index = (y % 2 == 0) ? (y * matrix->config.matrixWidth + x) 
                                  : (y * matrix->config.matrixWidth + (matrix->config.matrixWidth - 1 - x));
        } else {
            // 连续扫描
            index = y * matrix->config.matrixWidth + x;
        }
    } else {
        // 行优先布局
        if (matrix->config.matrixType & MATRIX_SCAN_ZIGZAG) {
            // 之字形扫描
            index = (x % 2 == 0) ? (x * matrix->config.matrixHeight + y)
                                  : (x * matrix->config.matrixHeight + (matrix->config.matrixHeight - 1 - y));
        } else {
            // 连续扫描
            index = x * matrix->config.matrixHeight + y;
        }
    }

    // 调整索引以处理起始角
    switch (matrix->config.matrixType & MATRIX_START_CORNER) {
        case MATRIX_START_TOP_RIGHT:
            index = (matrix->config.matrixWidth * matrix->config.matrixHeight - 1) - index;
            break;
        case MATRIX_START_BOTTOM_LEFT:
            index = (matrix->config.matrixHeight - 1 - y) * matrix->config.matrixWidth + x;
            break;
        case MATRIX_START_BOTTOM_RIGHT:
            index = (matrix->config.matrixWidth * matrix->config.matrixHeight - 1) - index;
            break;
        case MATRIX_START_TOP_LEFT:
        default:
            // 无需调整
            break;
    }

    // 设置指定 LED 的颜色
    matrix->strip->set_pixel(matrix->strip, index, color.red, color.green, color.blue);
}

// 刷新显示以更新 LED
void led_matrix_show(ws2812_matrix_t *matrix) {
    if (matrix == NULL || matrix->strip == NULL) {
        ESP_LOGE(TAG, "Matrix or strip handle is NULL");
        return;
    }
    matrix->strip->refresh(matrix->strip, 100);
}

void led_matrix_clear(ws2812_matrix_t *matrix){

    if (matrix == NULL || matrix->strip == NULL) {
        ESP_LOGE(TAG, "Matrix or strip handle is NULL");
        return;
    }
    matrix->strip->clear(matrix->strip,100); // 清空所有 LED
}

#endif
