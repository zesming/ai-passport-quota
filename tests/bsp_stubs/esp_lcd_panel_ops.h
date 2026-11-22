#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t, bool, bool);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t, int, int);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t);
