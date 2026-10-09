// Sleep pin levels for the screen-off light sleep: backlight pulled down, LCD CS pulled up.
#include <assert.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_display_pins.c"

#define PIN_COUNT 64
static int pull[PIN_COUNT];
static int stop_calls, stop_idle = -1, fail_stop, fail_pull_pin = -1;

esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t channel, uint32_t idle_level)
{
    assert(mode == BSP_BL_LEDC_MODE && channel == BSP_BL_LEDC_CHANNEL);
    ++stop_calls;
    stop_idle = (int)idle_level;
    return fail_stop ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_sleep_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t mode)
{
    assert(pin >= 0 && pin < PIN_COUNT);
    if (pin == fail_pull_pin)
        return ESP_FAIL;
    pull[pin] = (int)mode;
    return ESP_OK;
}

int main(void)
{
    for (int i = 0; i < PIN_COUNT; ++i)
        pull[i] = -1;
    /* Backlight: PWM stopped at idle low and pulled down; CS: pulled up so the ST7789 stays idle.
     */
    assert(bsp_display_sleep_pins_set(true) == ESP_OK);
    assert(stop_calls == 1 && stop_idle == 0);
    assert(pull[BSP_LCD_BL] == GPIO_PULLDOWN_ONLY);
    assert(pull[BSP_LCD_CS] == GPIO_PULLUP_ONLY);
    /* Without a PWM channel (backlight init failed) only the pulls are applied. */
    assert(bsp_display_sleep_pins_set(false) == ESP_OK && stop_calls == 1);
    /* No other pin is touched. */
    for (int i = 0; i < PIN_COUNT; ++i)
        assert(i == BSP_LCD_BL || i == BSP_LCD_CS || pull[i] == -1);
    /* Restore returns both pins to the default floating isolation. */
    assert(bsp_display_sleep_pins_restore() == ESP_OK);
    assert(pull[BSP_LCD_BL] == GPIO_FLOATING && pull[BSP_LCD_CS] == GPIO_FLOATING);
    /* Errors are reported but the other pin is still configured. */
    fail_stop = 1;
    pull[BSP_LCD_CS] = -1;
    assert(bsp_display_sleep_pins_set(true) == ESP_FAIL);
    assert(pull[BSP_LCD_CS] == GPIO_PULLUP_ONLY);
    fail_stop = 0;
    fail_pull_pin = BSP_LCD_CS;
    assert(bsp_display_sleep_pins_set(true) == ESP_FAIL && pull[BSP_LCD_BL] == GPIO_PULLDOWN_ONLY);
    fail_pull_pin = BSP_LCD_BL;
    assert(bsp_display_sleep_pins_restore() == ESP_FAIL && pull[BSP_LCD_CS] == GPIO_FLOATING);
    puts("BSP display sleep pin tests: PASS");
}
