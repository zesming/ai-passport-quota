#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"

typedef enum { LCD_RGB_ELEMENT_ORDER_RGB = 0 } lcd_rgb_element_order_t;

typedef struct {
    int reset_gpio_num;
    lcd_rgb_element_order_t rgb_ele_order;
    unsigned bits_per_pixel;
} esp_lcd_panel_dev_config_t;

esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);
