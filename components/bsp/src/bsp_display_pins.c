// components/bsp/src/bsp_display_pins.c
// 息屏浅睡眠的引脚状态：PM_SLP_DISABLE_GPIO 让睡眠中的引脚浮空，背光会漏光、LCD CS 会失去
// 片选高电平。单独成文件是为了让主机测试能直接验证这些电平。
#include "bsp_display_pins.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "bsp_pins";

esp_err_t bsp_display_sleep_pins_set(bool backlight_pwm)
{
    esp_err_t first_error = ESP_OK;
    if (BSP_LCD_BL >= 0) {
        esp_err_t e = ESP_OK;
        if (backlight_pwm)
            e = ledc_stop(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, 0);
        if (e == ESP_OK)
            e = gpio_sleep_set_pull_mode(BSP_LCD_BL, GPIO_PULLDOWN_ONLY);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "背光睡眠电平配置失败: %s", esp_err_to_name(e));
            first_error = e;
        }
    }
    if (BSP_LCD_CS >= 0) {
        esp_err_t e = gpio_sleep_set_pull_mode(BSP_LCD_CS, GPIO_PULLUP_ONLY);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "LCD CS 睡眠电平配置失败: %s", esp_err_to_name(e));
            if (first_error == ESP_OK)
                first_error = e;
        }
    }
    return first_error;
}

esp_err_t bsp_display_sleep_pins_restore(void)
{
    esp_err_t first_error = ESP_OK;
    const gpio_num_t pins[] = {BSP_LCD_BL, BSP_LCD_CS};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if ((int)pins[i] < 0)
            continue;
        esp_err_t e = gpio_sleep_set_pull_mode(pins[i], GPIO_FLOATING);
        if (e != ESP_OK && first_error == ESP_OK)
            first_error = e;
    }
    return first_error;
}
