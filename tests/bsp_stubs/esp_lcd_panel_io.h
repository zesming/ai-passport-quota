#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t esp_lcd_spi_bus_handle_t;
typedef struct {
    int cs_gpio_num;
    int dc_gpio_num;
    int pclk_hz;
    int spi_mode;
    int lcd_cmd_bits;
    int lcd_param_bits;
    int trans_queue_depth;
} esp_lcd_panel_io_spi_config_t;

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param,
                                    size_t param_size);
