// components/bsp/src/bsp_display_lvgl.c
// LVGL 接入单独成文件:不用 LVGL 的开发者删掉本文件 + idf_component.yml 里的两条依赖即可。
#include "bsp_display.h"
#include "bsp_display_rounding.h"
#include "bsp_pins.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bsp_lvgl";

#define BSP_LVGL_DRAW_BUFFER_LINES 20

static lv_display_t *s_disp;
static bool s_port_initialized;
static bool s_port_init_failed;

static void rounded_flush_event(lv_event_t *event)
{
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data ||
        lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        return;
    }

    const int32_t width = lv_area_get_width(area);
    if (draw_buf->header.stride < (uint32_t)width * sizeof(uint16_t))
        return;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t *row = (uint16_t *)(draw_buf->data + (y - area->y1) * draw_buf->header.stride);
        int32_t visible_x1;
        int32_t visible_x2;
        if (!bsp_display_rounded_row_span(y, BSP_LCD_W, BSP_LCD_H, BSP_LVGL_SCREEN_RADIUS,
                                          &visible_x1, &visible_x2)) {
            memset(row, 0, (size_t)width * sizeof(uint16_t));
            continue;
        }
        // Only clear pixels outside the visible span. The port swaps RGB565
        // bytes after this event; black is 0 in either byte order.
        const int32_t clear_left_end = visible_x1 > area->x2 ? area->x2 : visible_x1 - 1;
        const int32_t clear_right_start = visible_x2 < area->x1 ? area->x1 : visible_x2 + 1;
        for (int32_t x = area->x1; x <= clear_left_end; ++x) {
            row[x - area->x1] = 0;
        }
        for (int32_t x = clear_right_start; x <= area->x2; ++x) {
            row[x - area->x1] = 0;
        }
    }
}

lv_display_t *bsp_lvgl_init(void)
{
    if (s_disp)
        return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "请先成功调用 bsp_display_init()");
        return NULL;
    }

    if (!s_port_initialized) {
        // Port 2.9.0 has no public completion handshake for asynchronous deinit.
        // Never overwrite a possibly live context after a partial port failure.
        if (s_port_init_failed) {
            ESP_LOGE(TAG, "LVGL port 初始化未完成，需重启后重试");
            return NULL;
        }
        lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
        pc.task_max_sleep_ms = 60000;
        if (lvgl_port_init(&pc) != ESP_OK) {
            s_port_init_failed = true;
            ESP_LOGE(TAG, "lvgl_port_init 失败，需重启后重试");
            return NULL;
        }
        s_port_initialized = true;
    }

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle = bsp_display_io(),
        // ⚠ C3 无 PSRAM,DMA 只能用内部 RAM。20 行单缓冲约 9.6KB，
        // 可减少窗口命令和队列提交次数；仍保留单缓冲，避免双缓冲挤压音频/Wi-Fi。
        .buffer_size = (uint32_t)BSP_LCD_W * BSP_LVGL_DRAW_BUFFER_LINES,
        .double_buffer = false,
        .hres = BSP_LCD_W,
        .vres = BSP_LCD_H,
        // 旋转/镜像必须在这里配:esp_lvgl_port 注册显示时会重新下发 MADCTL,
        // 覆盖 bsp_display.c 里 esp_lcd_panel_mirror() 的设置。
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        // swap_bytes:LVGL 输出小端 RGB565,ST7789 走 SPI 要大端 → 需交换高低字节。
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    // The port mutex is recursive. Keep registration and the mask callback in
    // one critical section, before the new display can produce its first flush.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "LVGL 初始化加锁失败");
        return NULL;
    }
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    bool mask_registered = false;
    if (disp) {
        // LVGL 9.5 returns void here; check the list while still holding the lock.
        const uint32_t count = lv_display_get_event_count(disp);
        lv_display_add_event_cb(disp, rounded_flush_event, LV_EVENT_FLUSH_START, NULL);
        mask_registered = lv_display_get_event_count(disp) == count + 1;
    }
    if (!mask_registered) {
        ESP_LOGE(TAG, "LVGL display 或圆角回调注册失败");
        if (disp)
            lvgl_port_remove_disp(disp);
        lvgl_port_unlock();
        // Retain the initialized port for retry. Deinit is asynchronous and can
        // race the next init (or even run before the task sets running=true).
        return NULL;
    }

    // Mask the final RGB565 flush instead of using root-screen clip_corner.
    // Full-screen rounded clipping creates an ARGB layer that does not fit the
    // 24 KB LVGL pool reliably on this no-PSRAM target.
    s_disp = disp;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL 就绪，全局圆角=%d，外部填充=黑色", BSP_LVGL_SCREEN_RADIUS);
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms)
{
    if (!s_disp)
        return false;
    return lvgl_port_lock(timeout_ms);
}
void bsp_lvgl_unlock(void)
{
    if (s_disp)
        lvgl_port_unlock();
}

bool bsp_lvgl_set_sleeping(bool sleeping)
{
    if (!bsp_lvgl_lock(500))
        return false;
    lv_timer_t *refresh = lv_display_get_refr_timer(s_disp);
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!refresh || !panel) {
        bsp_lvgl_unlock();
        return false;
    }
    esp_err_t first, second;
    if (sleeping) {
        lv_timer_pause(refresh);
        first = esp_lcd_panel_disp_on_off(panel, false);
        second = esp_lcd_panel_disp_sleep(panel, true);
    } else {
        first = esp_lcd_panel_disp_sleep(panel, false); /* Driver waits for SLPOUT. */
        second = first == ESP_OK ? esp_lcd_panel_disp_on_off(panel, true) : first;
        if (first == ESP_OK && second == ESP_OK) {
            lv_timer_resume(refresh);
            lv_obj_invalidate(lv_display_get_screen_active(s_disp));
        }
    }
    /* Keep the port task/tick alive here: lvgl_port_stop alone disables the timer handler,
       whose LVGL 9.5 disabled path returns 1 and makes this port poll every 2ms.
       bsp_lvgl_suspend() stops the tick safely by pausing every timer first. */
    bsp_lvgl_unlock();
    if (!sleeping && first == ESP_OK && second == ESP_OK) {
        lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, s_disp);
    }
    return first == ESP_OK && second == ESP_OK;
}

// Screen-off quiet: LVGL timers (animations, UI timers) would otherwise wake the port task, and the
// 2 ms tick timer would keep the chip out of light sleep. Pause what is running, stop the tick,
// then re-enable the timer handler: with every timer paused it returns LV_NO_TIMER_READY and the
// port task blocks for task_max_sleep_ms instead of polling (see bsp_lvgl_set_sleeping).
#define BSP_LVGL_SUSPEND_MAX_TIMERS 16

static lv_timer_t *s_suspended_timers[BSP_LVGL_SUSPEND_MAX_TIMERS];
static uint8_t s_suspended_count;
static bool s_suspended;

// Caller holds the LVGL lock. A recorded timer deleted in the meantime is skipped.
static void resume_recorded_timers(void)
{
    for (uint8_t i = 0; i < s_suspended_count; ++i) {
        for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) {
            if (timer == s_suspended_timers[i]) {
                lv_timer_resume(timer);
                break;
            }
        }
    }
    s_suspended_count = 0;
}

bool bsp_lvgl_suspend(void)
{
    if (!bsp_lvgl_lock(500))
        return false;
    if (s_suspended) {
        bsp_lvgl_unlock();
        return true;
    }
    s_suspended_count = 0;
    bool overflow = false;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) {
        if (lv_timer_get_paused(timer))
            continue;
        if (s_suspended_count == BSP_LVGL_SUSPEND_MAX_TIMERS) {
            overflow = true; // Left running: the port task wakes more often but stays correct.
            continue;
        }
        lv_timer_pause(timer);
        s_suspended_timers[s_suspended_count++] = timer;
    }
    if (overflow)
        ESP_LOGW(TAG, "运行中的 LVGL 定时器超过 %d 个，多出的未暂停", BSP_LVGL_SUSPEND_MAX_TIMERS);
    if (lvgl_port_stop() != ESP_OK) {
        resume_recorded_timers();
        bsp_lvgl_unlock();
        return false;
    }
    lv_timer_enable(true);
    s_suspended = true;
    bsp_lvgl_unlock();
    return true;
}

bool bsp_lvgl_resume(void)
{
    if (!bsp_lvgl_lock(500))
        return false;
    if (!s_suspended) {
        bsp_lvgl_unlock();
        return true;
    }
    if (lvgl_port_resume() != ESP_OK) {
        bsp_lvgl_unlock();
        return false;
    }
    resume_recorded_timers();
    s_suspended = false;
    bsp_lvgl_unlock();
    // The port task may be blocked for task_max_sleep_ms; rerun the handler now.
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, s_disp);
    return true;
}

bool bsp_lvgl_refresh(void)
{
    if (!bsp_lvgl_lock(500))
        return false;
    lv_refr_now(s_disp);
    /* SPI parameter commands drain queued color transfers before returning. */
    esp_err_t err = esp_lcd_panel_disp_on_off(bsp_display_panel(), true);
    bsp_lvgl_unlock();
    return err == ESP_OK;
}
