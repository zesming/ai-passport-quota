idf_component_register(
    SRCS "src/bsp_i2c.c"
         "src/bsp_display.c"
         "src/bsp_display_lvgl.c"
         "src/bsp_display_pins.c"
         "src/bsp_display_rounding.c"
         "src/bsp_button.c"
         "src/bsp_battery.c"
         "src/bsp_power.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_lcd esp_lvgl_port button esp_adc
    PRIV_REQUIRES esp_timer esp_pm
)
