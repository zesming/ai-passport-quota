// Exercise ST7789 display init, rollback, backlight scaling, and light-sleep gates (bsp_display.c).
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../components/bsp/src/bsp_display.c"

static int io_token;
static int panel_token;
static esp_err_t fail_spi_init, fail_io_new, fail_panel_new, fail_panel_reset, fail_panel_init;
static esp_err_t fail_tx_param, fail_disp_on, fail_ledc_timer, fail_ledc_channel;
static esp_err_t fail_panel_del, fail_sleep_set, fail_sleep_restore;
static unsigned spi_inits, spi_frees, io_dels, panel_dels, tx_calls, ledc_resets;
static unsigned sleep_set_calls, sleep_restore_calls, duty_updates;
static uint32_t last_duty;
static bool last_sleep_bl_ready;

static void reset_fake(void)
{
    s_panel = NULL;
    s_io = NULL;
    s_bus_ready = false;
    s_bl_ready = false;
    s_ready = false;
    fail_spi_init = fail_io_new = fail_panel_new = fail_panel_reset = fail_panel_init = ESP_OK;
    fail_tx_param = fail_disp_on = fail_ledc_timer = fail_ledc_channel = ESP_OK;
    fail_panel_del = fail_sleep_set = fail_sleep_restore = ESP_OK;
    spi_inits = spi_frees = io_dels = panel_dels = tx_calls = ledc_resets = 0;
    sleep_set_calls = sleep_restore_calls = duty_updates = 0;
    last_duty = UINT32_MAX;
    last_sleep_bl_ready = false;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *bus_cfg,
                             spi_dma_chan_t dma_chan)
{
    (void)bus_cfg;
    (void)dma_chan;
    assert(host == BSP_LCD_SPI_HOST);
    spi_inits++;
    return fail_spi_init;
}

esp_err_t spi_bus_free(spi_host_device_t host)
{
    assert(host == BSP_LCD_SPI_HOST);
    spi_frees++;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io)
{
    (void)bus;
    (void)io_config;
    if (fail_io_new != ESP_OK)
        return fail_io_new;
    *ret_io = &io_token;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    assert(io == &io_token);
    io_dels++;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param,
                                    size_t param_size)
{
    (void)lcd_cmd;
    (void)param;
    (void)param_size;
    assert(io == &io_token);
    tx_calls++;
    return fail_tx_param;
}

esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    (void)panel_dev_config;
    assert(io == &io_token);
    if (fail_panel_new != ESP_OK)
        return fail_panel_new;
    *ret_panel = &panel_token;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel)
{
    assert(panel == &panel_token);
    return fail_panel_reset;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel)
{
    assert(panel == &panel_token);
    return fail_panel_init;
}

esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert)
{
    assert(panel == &panel_token && invert == BSP_LCD_INVERT_COLOR);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool x, bool y)
{
    assert(panel == &panel_token && !x && !y);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t panel, int x, int y)
{
    assert(panel == &panel_token && x == 0 && y == 0);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on)
{
    assert(panel == &panel_token && on);
    return fail_disp_on;
}

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel)
{
    assert(panel == &panel_token);
    panel_dels++;
    return fail_panel_del;
}

esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg)
{
    assert(cfg->speed_mode == BSP_BL_LEDC_MODE && cfg->timer_num == BSP_BL_LEDC_TIMER);
    return fail_ledc_timer;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg)
{
    assert(cfg->gpio_num == BSP_LCD_BL && cfg->channel == BSP_BL_LEDC_CHANNEL);
    return fail_ledc_channel;
}

esp_err_t ledc_timer_rst(ledc_mode_t mode, ledc_timer_t timer)
{
    assert(mode == BSP_BL_LEDC_MODE && timer == BSP_BL_LEDC_TIMER);
    ledc_resets++;
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty)
{
    assert(mode == BSP_BL_LEDC_MODE && channel == BSP_BL_LEDC_CHANNEL);
    last_duty = duty;
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel)
{
    assert(mode == BSP_BL_LEDC_MODE && channel == BSP_BL_LEDC_CHANNEL);
    duty_updates++;
    return ESP_OK;
}

esp_err_t bsp_display_sleep_pins_set(bool bl_ready)
{
    sleep_set_calls++;
    last_sleep_bl_ready = bl_ready;
    return fail_sleep_set;
}

esp_err_t bsp_display_sleep_pins_restore(void)
{
    sleep_restore_calls++;
    return fail_sleep_restore;
}

int main(void)
{
    /* Before init: handles are NULL, light sleep refuses, backlight is a no-op. */
    reset_fake();
    assert(bsp_display_panel() == NULL && bsp_display_io() == NULL);
    assert(bsp_display_enter_light_sleep() == ESP_ERR_INVALID_STATE);
    assert(bsp_display_exit_light_sleep() == ESP_ERR_INVALID_STATE);
    bsp_display_backlight(50);
    assert(duty_updates == 0);

    /* Failure during LEDC channel config resets timer and rolls back panel, IO, and SPI bus. */
    fail_ledc_channel = ESP_ERR_NO_MEM;
    assert(bsp_display_init() == ESP_ERR_NO_MEM);
    assert(ledc_resets == 1 && panel_dels == 1 && io_dels == 1 && spi_frees == 1);
    assert(bsp_display_panel() == NULL && bsp_display_io() == NULL && !s_bus_ready && !s_bl_ready);

    /* Retry after fixing the failure succeeds, sends vendor commands, and is idempotent. */
    fail_ledc_channel = ESP_OK;
    assert(bsp_display_init() == ESP_OK);
    assert(bsp_display_panel() == &panel_token && bsp_display_io() == &io_token);
    assert(tx_calls == sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]) * 2);
    unsigned inits_before = spi_inits;
    assert(bsp_display_init() == ESP_OK && spi_inits == inits_before);

    /* Backlight scales 0..100 across 10-bit LEDC duty and clamps values >100. */
    bsp_display_backlight(0);
    assert(last_duty == 0);
    bsp_display_backlight(100);
    assert(last_duty == 1023);
    bsp_display_backlight(250);
    assert(last_duty == 1023);

    /* Light-sleep enter/exit zero backlight and delegate to pin configuration. */
    assert(bsp_display_enter_light_sleep() == ESP_OK);
    assert(sleep_set_calls == 1 && last_sleep_bl_ready && last_duty == 0);
    bsp_display_backlight(80);
    assert(bsp_display_exit_light_sleep() == ESP_OK);
    assert(sleep_restore_calls == 1 && last_duty == 0);

    /* Partial rollback (panel_del fails) refuses to overwrite surviving SPI resources on retry. */
    reset_fake();
    fail_tx_param = ESP_FAIL;
    fail_panel_del = ESP_FAIL;
    assert(bsp_display_init() == ESP_FAIL);
    assert(s_panel == &panel_token && io_dels == 0 && spi_frees == 0);
    assert(bsp_display_init() == ESP_ERR_INVALID_STATE);

    puts("BSP display driver tests: PASS");
    return 0;
}
