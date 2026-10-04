#include "quota_ui.h"

#include "bsp_pins.h"
#include "lvgl.h"
#include "quota_brand_assets.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(quota_font_12);
LV_FONT_DECLARE(quota_font_16);

#define UI_BG 0x11181F
#define UI_PANEL 0x18232B
#define UI_LINE 0x33414C
#define UI_INK 0xEDF1F4
#define UI_MUTED 0xAFBBC6
#define UI_DIM 0x82919D
#define UI_MINT 0x66D6AA
#define UI_AMBER 0xE1B974
#define UI_RED 0xF18C82
#define UI_TRACK 0x33414D
#define UI_BATTERY 0x34C759

typedef struct {
    lv_obj_t *header_info;
    lv_obj_t *battery_unknown;
    lv_obj_t *battery_fill;
    lv_obj_t *clock;
    lv_obj_t *wifi_lines[2];
    lv_obj_t *wifi_dot;
    lv_obj_t *wifi_slash;
    lv_obj_t *home_logo;
    lv_obj_t *home_provider;
    lv_obj_t *home_plan;
    lv_obj_t *home_email;
    lv_obj_t *metric_name[2];
    lv_obj_t *metric_reset[2];
    lv_obj_t *metric_value[2];
    lv_obj_t *metric_bar[2];
    lv_obj_t *extra_name[2];
    lv_obj_t *extra_value[2];
    lv_obj_t *home_empty;
    lv_obj_t *balance_heading[QUOTA_BALANCE_CURRENCIES];
    lv_obj_t *balance_total[QUOTA_BALANCE_CURRENCIES];
    lv_obj_t *home_status;
    lv_obj_t *setting_rows[5];
    lv_obj_t *setting_markers[5];
    lv_obj_t *setting_labels[5];
    lv_obj_t *setting_values[5];
    lv_obj_t *account_rows[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *account_markers[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *account_primary[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *account_secondary[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *interval_rows[5];
    lv_obj_t *interval_markers[5];
    lv_obj_t *interval_labels[5];
    lv_obj_t *interval_values[5];
    lv_obj_t *sleep_rows[QUOTA_SCREEN_TIMEOUT_COUNT];
    lv_obj_t *sleep_markers[QUOTA_SCREEN_TIMEOUT_COUNT];
    lv_obj_t *sleep_values[QUOTA_SCREEN_TIMEOUT_COUNT];
    lv_obj_t *setup_countdown;
    lv_obj_t *setup_hint;
} quota_ui_objects_t;

static lv_obj_t *s_root;
static lv_obj_t *s_page;
static quota_screen_t s_screen = (quota_screen_t)-1;
static quota_ui_objects_t s_ui;

static lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
}

static lv_obj_t *create_rect(lv_obj_t *parent, int x, int y, int width, int height,
                             uint32_t hex, int radius)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color(hex), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *create_label(lv_obj_t *parent, int x, int y, int width, int height,
                              const lv_font_t *font, uint32_t text_color,
                              lv_text_align_t align, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(text_color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *create_logo(lv_obj_t *parent, const lv_image_dsc_t *image,
                             int x, int y, int width, int height)
{
    lv_obj_t *logo = lv_image_create(parent);
    lv_image_set_src(logo, image);
    lv_obj_set_pos(logo, x, y);
    lv_obj_set_size(logo, width, height);
    if (width < 36) lv_image_set_scale(logo, (uint16_t)(width * 256 / 36));
    return logo;
}

static void create_header(const char *title, const char *info)
{
    create_label(s_page, 12, 9, 70, 22, &quota_font_16,
                              UI_INK, LV_TEXT_ALIGN_LEFT, title);
    s_ui.header_info = create_label(s_page, 86, 11, 28, 18, &quota_font_12,
                                    UI_MUTED, LV_TEXT_ALIGN_RIGHT, info);
    s_ui.clock = create_label(s_page, 122, 11, 40, 18, &lv_font_montserrat_12,
                              UI_MUTED, LV_TEXT_ALIGN_CENTER, "--:--");
    static const lv_point_precise_t wifi_points[][7] = {
        {{0, 5}, {2, 3}, {5, 1}, {8, 0}, {11, 1}, {14, 3}, {16, 5}},
        {{4, 9}, {6, 7}, {8, 6}, {10, 7}, {12, 9}},
    };
    for (size_t i = 0; i < 2; i++) {
        s_ui.wifi_lines[i] = lv_line_create(s_page);
        lv_obj_remove_style_all(s_ui.wifi_lines[i]);
        lv_obj_set_pos(s_ui.wifi_lines[i], 170, 12);
        lv_line_set_points(s_ui.wifi_lines[i], wifi_points[i], i == 0 ? 7 : 5);
        lv_obj_set_style_line_width(s_ui.wifi_lines[i], 2, 0);
        lv_obj_set_style_line_rounded(s_ui.wifi_lines[i], true, 0);
    }
    s_ui.wifi_dot = create_rect(s_page, 177, 24, 3, 3, UI_MUTED, 2);
    static const lv_point_precise_t slash[] = {{0, 0}, {16, 16}};
    s_ui.wifi_slash = lv_line_create(s_page);
    lv_obj_remove_style_all(s_ui.wifi_slash);
    lv_obj_set_pos(s_ui.wifi_slash, 170, 10);
    lv_line_set_points(s_ui.wifi_slash, slash, 2);
    lv_obj_set_style_line_width(s_ui.wifi_slash, 2, 0);
    lv_obj_set_style_line_color(s_ui.wifi_slash, color(UI_MUTED), 0);
    lv_obj_t *shell = create_rect(s_page, 197, 14, 26, 12, UI_BG, 2);
    lv_obj_set_style_border_color(shell, color(UI_MUTED), 0);
    lv_obj_set_style_border_width(shell, 1, 0);
    create_rect(s_page, 223, 17, 3, 6, UI_MUTED, 1);
    s_ui.battery_fill = create_rect(s_page, 199, 16, 22, 8, UI_BATTERY, 1);
    static const lv_point_precise_t battery_slash[] = {{0, 7}, {7, 0}};
    s_ui.battery_unknown = lv_line_create(s_page);
    lv_obj_remove_style_all(s_ui.battery_unknown);
    lv_obj_set_pos(s_ui.battery_unknown, 206, 16);
    lv_line_set_points(s_ui.battery_unknown, battery_slash, 2);
    lv_obj_set_style_line_width(s_ui.battery_unknown, 1, 0);
    lv_obj_set_style_line_color(s_ui.battery_unknown, color(UI_MUTED), 0);
    create_rect(s_page, 12, 35, 216, 1, UI_LINE, 0);
}

static void create_footer(const char *text)
{
    create_rect(s_page, 12, 280, 216, 1, UI_LINE, 0);
    create_label(s_page, 12, 288, 216, 21, &quota_font_12,
                                UI_MUTED, LV_TEXT_ALIGN_LEFT, text);
}

static void create_home_page(void)
{
    create_header("AI 额度", "0/0");
    s_ui.home_logo = create_logo(s_page, &quota_openai_logo, 12, 46, 36, 36);
    s_ui.home_provider = create_label(s_page, 56, 43, 160, 24, &lv_font_montserrat_20,
                                      UI_INK, LV_TEXT_ALIGN_LEFT, "ChatGPT");
    s_ui.home_plan = create_label(s_page, 56, 67, 160, 17, &quota_font_12,
                                  UI_MUTED, LV_TEXT_ALIGN_LEFT, "Codex");
    s_ui.home_email = create_label(s_page, 56, 84, 160, 17, &quota_font_12,
                                   UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    create_rect(s_page, 12, 103, 216, 1, UI_LINE, 0);

    static const char *const names[] = {"5h 剩余", "7 天剩余"};
    for (size_t i = 0; i < 2; i++) {
        int y = i == 0 ? 107 : 183;
        s_ui.metric_name[i] = create_label(s_page, 12, y, 86, 18, &quota_font_12,
                                           UI_INK, LV_TEXT_ALIGN_LEFT, names[i]);
        s_ui.metric_reset[i] = create_label(s_page, 96, y, 132, 18, &quota_font_12,
                                            UI_MUTED, LV_TEXT_ALIGN_RIGHT, "重置时间未知");
        s_ui.metric_value[i] = create_label(s_page, 12, y + 17, 116, 30,
                                            &lv_font_montserrat_20, UI_MINT,
                                            LV_TEXT_ALIGN_LEFT, "--");
        s_ui.metric_bar[i] = lv_bar_create(s_page);
        lv_obj_remove_style_all(s_ui.metric_bar[i]);
        lv_obj_set_pos(s_ui.metric_bar[i], 12, y + 52);
        lv_obj_set_size(s_ui.metric_bar[i], 216, 8);
        lv_bar_set_range(s_ui.metric_bar[i], 0, 100);
        lv_bar_set_value(s_ui.metric_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_ui.metric_bar[i], color(UI_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_ui.metric_bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_ui.metric_bar[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_ui.metric_bar[i], color(UI_MINT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_ui.metric_bar[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_ui.metric_bar[i], 4, LV_PART_INDICATOR);
    }
    for (size_t i = 0; i < QUOTA_BALANCE_CURRENCIES; i++) {
        int y = i == 0 ? 107 : 183;
        s_ui.balance_heading[i] = create_label(s_page, 12, y, 216, 18,
                                               &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
        s_ui.balance_total[i] = create_label(s_page, 12, y + 17, 216, 26,
                                             &lv_font_montserrat_20, UI_INK, LV_TEXT_ALIGN_LEFT, "");
    }
    for (size_t i = 0; i < 2; i++) {
        s_ui.extra_name[i] = create_label(s_page, 12, 228 + (int)i * 16, 72, 16,
                                          &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
        s_ui.extra_value[i] = create_label(s_page, 84, 228 + (int)i * 16, 144, 16,
                                           &quota_font_12, UI_INK, LV_TEXT_ALIGN_RIGHT, "");
    }
    s_ui.home_empty = create_label(s_page, 12, 119, 216, 22, &quota_font_16,
                                    UI_MUTED, LV_TEXT_ALIGN_LEFT, "等待额度数据");
    s_ui.home_status = create_label(s_page, 12, 262, 216, 18, &quota_font_12,
                                    UI_MUTED, LV_TEXT_ALIGN_LEFT, "等待电脑数据");
    create_footer("UP/DOWN 切换  OK 刷新  长按设置");
}

static void create_focus_row(lv_obj_t **background, lv_obj_t **marker,
                             int y, int height)
{
    *background = create_rect(s_page, 12, y, 216, height, UI_BG, 4);
    *marker = create_rect(s_page, 12, y + 4, 3, height - 8, UI_MINT, 2);
    lv_obj_add_flag(*marker, LV_OBJ_FLAG_HIDDEN);
}

static void create_settings_page(void)
{
    create_header("设置", "");
    static const char *const labels[] = {"账户管理", "刷新间隔", "立即刷新", "自动息屏", "电脑配对"};
    for (size_t i = 0; i < 5; i++) {
        int y = 44 + (int)i * 39;
        create_focus_row(&s_ui.setting_rows[i], &s_ui.setting_markers[i], y, 34);
        s_ui.setting_labels[i] = create_label(s_page, 22, y + 5, 118, 24,
                                             &quota_font_16, UI_INK,
                                             LV_TEXT_ALIGN_LEFT, labels[i]);
        s_ui.setting_values[i] = create_label(s_page, 140, y + 7, 82, 20,
                                             &quota_font_12, UI_MUTED,
                                             LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 244, 212, 30, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_LEFT, "离线时保留最近数据\n账户登录由电脑端管理");
    create_footer("UP/DOWN 选择  OK 确认  长按返回");
}

static void create_sleep_page(void)
{
    create_header("自动息屏", "");
    static const char *const labels[] = {"从不", "30 秒", "1 分钟", "2 分钟", "5 分钟", "10 分钟"};
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        int y = 43 + (int)i * 35;
        create_focus_row(&s_ui.sleep_rows[i], &s_ui.sleep_markers[i], y, 32);
        create_label(s_page, 22, y + 4, 146, 24, &quota_font_16, UI_INK,
                     LV_TEXT_ALIGN_LEFT, labels[i]);
        s_ui.sleep_values[i] = create_label(s_page, 170, y + 6, 48, 19,
                                            &quota_font_12, UI_MUTED,
                                            LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 253, 212, 23, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_LEFT, "任意键亮屏  长按 DOWN 息屏");
    create_footer("UP/DOWN 选择  OK 保存  长按返回");
}

static void create_accounts_page(void)
{
    create_header("账户管理", "0/0");
    for (size_t i = 0; i < QUOTA_MAX_ACCOUNTS + 1; i++) {
        int y = 41 + (int)i * 26;
        create_focus_row(&s_ui.account_rows[i], &s_ui.account_markers[i], y, 25);
        s_ui.account_primary[i] = create_label(s_page, 22, y + 1, 198, 13,
                                               &quota_font_12, UI_INK,
                                               LV_TEXT_ALIGN_LEFT, "");
        s_ui.account_secondary[i] = create_label(s_page, 22, y + 13, 198, 12,
                                                 &quota_font_12, UI_MUTED,
                                                 LV_TEXT_ALIGN_LEFT, "");
    }
    create_footer("OK 选择账户或电脑配对  长按返回");
}

static void create_interval_page(void)
{
    create_header("刷新间隔", "");
    static const char *const labels[] = {"自动刷新", "1 分钟", "5 分钟", "15 分钟", "30 分钟"};
    for (size_t i = 0; i < 5; i++) {
        int y = 49 + (int)i * 43;
        create_focus_row(&s_ui.interval_rows[i], &s_ui.interval_markers[i], y, 36);
        s_ui.interval_labels[i] = create_label(s_page, 22, y + 6, 146, 24,
                                               &quota_font_16, UI_INK,
                                               LV_TEXT_ALIGN_LEFT, labels[i]);
        s_ui.interval_values[i] = create_label(s_page, 170, y + 8, 48, 19,
                                               &quota_font_12, UI_MUTED,
                                               LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 267, 212, 14, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_LEFT, "自动刷新也受电脑端设置影响");
    create_footer("UP/DOWN 选择  OK 保存  长按返回");
}

static void create_setup_page(void)
{
    create_header("电脑配对", "USB");
    create_label(s_page, 20, 51, 200, 22, &quota_font_16, UI_INK,
                 LV_TEXT_ALIGN_CENTER, "请在电脑设置页");
    create_label(s_page, 20, 74, 200, 22, &quota_font_16, UI_INK,
                 LV_TEXT_ALIGN_CENTER, "添加账户并完成连接");
    create_logo(s_page, &quota_openai_logo, 30, 109, 36, 36);
    create_logo(s_page, &quota_claude_logo, 102, 109, 36, 36);
    create_logo(s_page, &quota_deepseek_logo, 174, 109, 36, 36);
    create_label(s_page, 12, 149, 72, 22, &quota_font_12,
                                         UI_INK, LV_TEXT_ALIGN_CENTER, "Codex");
    create_label(s_page, 84, 149, 72, 22, &quota_font_12,
                                         UI_INK, LV_TEXT_ALIGN_CENTER, "Claude");
    create_label(s_page, 156, 149, 72, 22, &quota_font_12,
                  UI_INK, LV_TEXT_ALIGN_CENTER, "DeepSeek");
    s_ui.setup_countdown = create_label(s_page, 15, 190, 210, 25, &quota_font_16,
                                        UI_MINT, LV_TEXT_ALIGN_CENTER, "配对窗口 02:00");
    s_ui.setup_hint = create_label(s_page, 16, 220, 208, 44, &quota_font_12,
                                   UI_MUTED, LV_TEXT_ALIGN_CENTER,
                                   "请通过 USB 提交配置\n设备只接收额度快照");
    create_footer("长按 OK 返回");
}

static void create_page(quota_screen_t screen)
{
    if (s_page != NULL) lv_obj_delete(s_page);
    memset(&s_ui, 0, sizeof(s_ui));
    s_page = create_rect(s_root, 0, 0, BSP_LCD_W, BSP_LCD_H, UI_BG, 0);
    s_screen = screen;
    switch (screen) {
        case QUOTA_SCREEN_HOME: create_home_page(); break;
        case QUOTA_SCREEN_SETTINGS: create_settings_page(); break;
        case QUOTA_SCREEN_ACCOUNTS: create_accounts_page(); break;
        case QUOTA_SCREEN_INTERVAL: create_interval_page(); break;
        case QUOTA_SCREEN_SLEEP: create_sleep_page(); break;
        case QUOTA_SCREEN_SETUP: create_setup_page(); break;
        default: create_home_page(); break;
    }
}

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (label != NULL && text != NULL) lv_label_set_text(label, text);
}

static void set_row_focus(lv_obj_t *row, lv_obj_t *marker, bool focused)
{
    if (row == NULL || marker == NULL) return;
    lv_obj_set_style_bg_color(row, color(focused ? UI_PANEL : UI_BG), 0);
    if (focused) lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
}

static void set_account_row_visible(size_t index, bool visible)
{
    lv_obj_t *objects[] = {
        s_ui.account_rows[index], s_ui.account_markers[index],
        s_ui.account_primary[index], s_ui.account_secondary[index],
    };
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        if (objects[i] == NULL) continue;
        if (visible) lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void format_clock(uint64_t epoch, char *output, size_t capacity)
{
    time_t value = (time_t)epoch;
    struct tm local = {0};
    if (epoch < 1577836800ULL || localtime_r(&value, &local) == NULL ||
        strftime(output, capacity, "%H:%M", &local) == 0) {
        snprintf(output, capacity, "--:--");
    }
}

static void render_metric(size_t index, const quota_window_t *window, uint64_t now,
                          bool stale, bool clock_synchronized, int y, bool single)
{
    lv_obj_set_y(s_ui.metric_name[index], y);
    lv_obj_set_y(s_ui.metric_reset[index], y);
    lv_obj_set_y(s_ui.metric_value[index], y + 18);
    lv_obj_set_y(s_ui.metric_bar[index], y + (single ? 62 : 46));
    lv_obj_clear_flag(s_ui.metric_name[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.metric_reset[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.metric_value[index], LV_OBJ_FLAG_HIDDEN);
    char reset_text[48];
    quota_format_reset_time(window, now, clock_synchronized, reset_text, sizeof(reset_text));
    set_label_text(s_ui.metric_reset[index], reset_text);
    quota_metric_state_t state = quota_metric_state(window, now);
    if (state == QUOTA_METRIC_UNAVAILABLE) {
        set_label_text(s_ui.metric_value[index], "未提供");
        lv_obj_set_style_text_font(s_ui.metric_value[index], &quota_font_12, 0);
        lv_obj_set_style_text_color(s_ui.metric_value[index], color(UI_DIM), 0);
        lv_obj_add_flag(s_ui.metric_bar[index], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (state == QUOTA_METRIC_WAITING_FOR_SOURCE) {
        set_label_text(s_ui.metric_value[index], "等待新数据");
        lv_obj_set_style_text_font(s_ui.metric_value[index], &quota_font_12, 0);
        lv_obj_set_style_text_color(s_ui.metric_value[index], color(UI_MUTED), 0);
        lv_obj_add_flag(s_ui.metric_bar[index], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char value[8];
    snprintf(value, sizeof(value), "%u%%", (unsigned)window->remaining_percent);
    set_label_text(s_ui.metric_value[index], value);
    lv_obj_set_style_text_font(s_ui.metric_value[index], &lv_font_montserrat_20, 0);
    uint32_t tone = stale ? UI_DIM : window->remaining_percent <= 5 ? UI_RED :
                    window->remaining_percent <= 20 ? UI_AMBER : UI_MINT;
    lv_obj_set_style_text_color(s_ui.metric_value[index], color(tone), 0);
    lv_obj_set_style_bg_color(s_ui.metric_bar[index], color(stale ? UI_DIM : tone),
                              LV_PART_INDICATOR);
    lv_bar_set_value(s_ui.metric_bar[index], window->remaining_percent, LV_ANIM_OFF);
    lv_obj_clear_flag(s_ui.metric_bar[index], LV_OBJ_FLAG_HIDDEN);
}

static void render_extra(size_t index, const char *name, const char *value, int y, bool stale)
{
    set_label_text(s_ui.extra_name[index], name);
    set_label_text(s_ui.extra_value[index], value);
    lv_obj_set_y(s_ui.extra_name[index], y);
    lv_obj_set_y(s_ui.extra_value[index], y);
    lv_obj_set_style_text_color(s_ui.extra_value[index], color(stale ? UI_DIM : UI_INK), 0);
    lv_obj_clear_flag(s_ui.extra_name[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.extra_value[index], LV_OBJ_FLAG_HIDDEN);
}

static void render_home(const quota_navigation_t *navigation,
                        const quota_service_view_t *service)
{
    char count[16];
    snprintf(count, sizeof(count), "%u/%u", service->snapshot.account_count == 0 ? 0 : (unsigned)(navigation->selected_account + 1),
             (unsigned)service->snapshot.account_count);
    set_label_text(s_ui.header_info, count);
    bool deepseek = service->snapshot.account_count > 0 &&
        service->snapshot.accounts[navigation->selected_account < service->snapshot.account_count
                                    ? navigation->selected_account : 0].provider == QUOTA_PROVIDER_DEEPSEEK;
    for (size_t i = 0; i < QUOTA_BALANCE_CURRENCIES; i++) {
        lv_obj_t *balance_objects[] = {s_ui.balance_heading[i], s_ui.balance_total[i]};
        for (size_t j = 0; j < 2; j++) lv_obj_add_flag(balance_objects[j], LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *quota_objects[] = {s_ui.metric_name[i], s_ui.metric_reset[i], s_ui.metric_value[i], s_ui.metric_bar[i]};
        for (size_t j = 0; j < 4; j++) lv_obj_add_flag(quota_objects[j], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.extra_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.extra_value[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_ui.home_empty, LV_OBJ_FLAG_HIDDEN);
    if (service->snapshot.account_count == 0) {
        lv_obj_add_flag(s_ui.home_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_ui.home_provider, &quota_font_16, 0);
        set_label_text(s_ui.home_provider, "暂无账户");
        set_label_text(s_ui.home_plan, service->configured ? "请在电脑端添加账户" : "等待电脑配对");
        set_label_text(s_ui.home_email, "");
        set_label_text(s_ui.home_status, service->configured ? "等待额度快照" : "请先完成 USB 配置");
        return;
    }

    size_t selected = navigation->selected_account;
    if (selected >= service->snapshot.account_count) selected = 0;
    const quota_account_t *account = &service->snapshot.accounts[selected];
    bool is_claude = account->provider == QUOTA_PROVIDER_CLAUDE;
    lv_obj_set_style_text_font(s_ui.home_provider, &lv_font_montserrat_20, 0);
    lv_image_set_src(s_ui.home_logo, deepseek ? &quota_deepseek_logo
                                            : is_claude ? &quota_claude_logo : &quota_openai_logo);
    lv_obj_clear_flag(s_ui.home_logo, LV_OBJ_FLAG_HIDDEN);
    set_label_text(s_ui.home_provider, deepseek ? "DeepSeek" : is_claude ? "Claude" : "ChatGPT");
    char plan[QUOTA_PLAN_MAX_BYTES + 1];
    quota_copy_display_plan(account->plan, plan, sizeof(plan));
    char source_and_plan[QUOTA_PLAN_MAX_BYTES + 20];
    snprintf(source_and_plan, sizeof(source_and_plan), "%s · %s",
             is_claude ? "Claude Code" : "Codex", plan[0] == '\0' ? "" : plan);
    set_label_text(s_ui.home_plan, source_and_plan);
    if (deepseek) set_label_text(s_ui.home_plan, "开放平台 · API");
    char email[QUOTA_EMAIL_MAX_BYTES + 1];
    quota_copy_display_ascii(account->email, email, sizeof(email));
    set_label_text(s_ui.home_email, email);
    if (deepseek) {
        quota_copy_display_ascii(service->snapshot.balances[selected].label, email, sizeof(email));
        set_label_text(s_ui.home_email, email[0] == '\0' ? "DeepSeek API" : email);
    }

    bool stale = quota_data_is_stale(service->now_epoch, account->has_observed_at,
                                     account->observed_at, service->refresh_seconds) ||
                 account->status != QUOTA_STATUS_OK;
    const quota_balance_t *balance = &service->snapshot.balances[selected];
    if (deepseek) {
        const quota_currency_balance_t *entry = quota_balance_cny(balance);
        if (entry != NULL) {
            size_t i = 0;
            char heading[32];
            snprintf(heading, sizeof(heading), "人民币可用余额");
            set_label_text(s_ui.balance_heading[i], heading);
            set_label_text(s_ui.balance_total[i], entry->total_balance);
            lv_obj_set_style_text_font(s_ui.balance_total[i], strlen(entry->total_balance) > 16
                ? &lv_font_montserrat_12 : strlen(entry->total_balance) > 12
                ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(s_ui.balance_total[i], color(stale ? UI_DIM : UI_INK), 0);
            lv_obj_clear_flag(s_ui.balance_heading[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_ui.balance_total[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (entry == NULL) {
            set_label_text(s_ui.balance_heading[0], "人民币可用余额");
            set_label_text(s_ui.balance_total[0], "未提供");
            lv_obj_set_style_text_font(s_ui.balance_total[0], &quota_font_16, 0);
            lv_obj_clear_flag(s_ui.balance_heading[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_ui.balance_total[0], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        const quota_window_t *windows[] = {&account->five_hour, &account->seven_day};
        unsigned window_count = account->five_hour.present + account->seven_day.present;
        unsigned row = 0;
        for (size_t i = 0; i < 2; i++) {
            if (!windows[i]->present) continue;
            render_metric(i, windows[i], service->now_epoch, stale, service->clock_synchronized,
                          109 + (int)row++ * 60, window_count == 1);
        }
        const quota_codex_extras_t *extras = &service->snapshot.codex_extras[selected];
        int extra_y = window_count == 2 ? 228 : window_count == 1 ? 199 : 151;
        unsigned extra_row = 0;
        if (!is_claude && extras->has_banked_reset && extras->available_resets > 0) {
            char value[64];
            if (extras->has_next_reset_expiry && !service->clock_synchronized) {
                snprintf(value, sizeof(value), "%llu次 · 时间待同步",
                         (unsigned long long)extras->available_resets);
            } else if (extras->has_next_reset_expiry && extras->next_reset_expires_at > service->now_epoch) {
                char expiry[20];
                quota_format_duration(extras->next_reset_expires_at - service->now_epoch,
                                      expiry, sizeof(expiry));
                snprintf(value, sizeof(value), "%llu次 · %s 到期",
                         (unsigned long long)extras->available_resets, expiry);
            } else if (extras->has_next_reset_expiry) {
                snprintf(value, sizeof(value), "%llu次 · 等待更新",
                         (unsigned long long)extras->available_resets);
            } else {
                snprintf(value, sizeof(value), "%llu 次", (unsigned long long)extras->available_resets);
            }
            render_extra(extra_row++, "可用重置", value, extra_y, stale);
        }
        if (!is_claude && extras->has_credits) {
            char value[QUOTA_CREDITS_BALANCE_BYTES + 1];
            quota_copy_display_ascii(extras->credits_balance, value, sizeof(value));
            render_extra(extra_row, "剩余额度", extras->unlimited_credits ? "不限量" :
                         value[0] != '\0' ? value : "可用", extra_y + (int)extra_row * 16, stale);
        }
        if (window_count == 0) {
            set_label_text(s_ui.home_empty, account->status == QUOTA_STATUS_OK ? "未提供额度窗口" : "等待额度数据");
            lv_obj_clear_flag(s_ui.home_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }

    char status[48];
    if (!service->connected) {
        snprintf(status, sizeof(status), "离线 · 最近数据");
    } else if (service->refreshing) {
        snprintf(status, sizeof(status), "正在刷新");
    } else if (service->request_failed) {
        snprintf(status, sizeof(status), "更新失败 · 保留缓存");
    } else if (account->status == QUOTA_STATUS_EXPIRED) {
        snprintf(status, sizeof(status), "登录过期 · 最近数据");
    } else if (account->status == QUOTA_STATUS_WAITING) {
        snprintf(status, sizeof(status), "等待电脑采集");
    } else if (account->status == QUOTA_STATUS_ERROR) {
        snprintf(status, sizeof(status), "数据源错误 · 保留缓存");
    } else if (account->status == QUOTA_STATUS_UNSUPPORTED) {
        snprintf(status, sizeof(status), "数据源暂不支持");
    } else if (deepseek && balance->present && !balance->is_available) {
        snprintf(status, sizeof(status), "余额不可用");
    } else if (account->has_observed_at) {
        char clock_text[16];
        format_clock(account->observed_at, clock_text, sizeof(clock_text));
        snprintf(status, sizeof(status), stale ? "缓存 · 更新于 %s" : "更新于 %s", clock_text);
    } else {
        snprintf(status, sizeof(status), "尚未采集数据");
    }
    set_label_text(s_ui.home_status, status);
    lv_obj_set_style_text_color(s_ui.home_status,
        color((!service->connected || stale || service->request_failed) ? UI_AMBER : UI_MUTED), 0);
}

static void render_settings(const quota_navigation_t *navigation,
                           const quota_service_view_t *service)
{
    static const char *const labels[] = {"账户管理", "刷新间隔", "立即刷新", "自动息屏", "电脑配对"};
    char values[5][24];
    snprintf(values[0], sizeof(values[0]), "%u 个", (unsigned)service->snapshot.account_count);
    if (!service->auto_refresh) snprintf(values[1], sizeof(values[1]), "手动");
    else snprintf(values[1], sizeof(values[1]), "%u 分钟", (unsigned)(service->refresh_seconds / 60));
    snprintf(values[2], sizeof(values[2]), "%s", service->refreshing ? "同步中" : "");
    if (service->screen_timeout_seconds == 0) snprintf(values[3], sizeof(values[3]), "从不");
    else if (service->screen_timeout_seconds < 60) {
        snprintf(values[3], sizeof(values[3]), "%u 秒", (unsigned)service->screen_timeout_seconds);
    } else {
        snprintf(values[3], sizeof(values[3]), "%u 分钟", (unsigned)(service->screen_timeout_seconds / 60));
    }
    snprintf(values[4], sizeof(values[4]), "%s", service->pairing_active ? "已打开" : "");
    for (size_t i = 0; i < 5; i++) {
        set_row_focus(s_ui.setting_rows[i], s_ui.setting_markers[i],
                      i == navigation->settings_focus);
        set_label_text(s_ui.setting_labels[i], labels[i]);
        set_label_text(s_ui.setting_values[i], values[i]);
    }
    set_label_text(s_ui.header_info, "");
}

static void render_accounts(const quota_navigation_t *navigation,
                            const quota_service_view_t *service)
{
    char count[16];
    snprintf(count, sizeof(count), "%u/%u",
             (unsigned)(navigation->account_focus < service->snapshot.account_count
                        ? navigation->account_focus + 1 : service->snapshot.account_count + 1),
             (unsigned)(service->snapshot.account_count + 1));
    set_label_text(s_ui.header_info, count);
    size_t visible_rows = (size_t)service->snapshot.account_count + 1;
    if (visible_rows > QUOTA_MAX_ACCOUNTS + 1) visible_rows = QUOTA_MAX_ACCOUNTS + 1;
    for (size_t i = 0; i < visible_rows; i++) {
        set_account_row_visible(i, true);
        bool selected = i == navigation->account_focus;
        set_row_focus(s_ui.account_rows[i], s_ui.account_markers[i], selected);
        if (i == service->snapshot.account_count) {
            set_label_text(s_ui.account_primary[i], "电脑端添加账户");
            set_label_text(s_ui.account_secondary[i], "打开 USB 配对设置");
        } else {
            const quota_account_t *account = &service->snapshot.accounts[i];
            bool claude = account->provider == QUOTA_PROVIDER_CLAUDE;
            char title[QUOTA_PLAN_MAX_BYTES + 20];
            char plan[QUOTA_PLAN_MAX_BYTES + 1];
            quota_copy_display_plan(account->plan, plan, sizeof(plan));
            bool deepseek = account->provider == QUOTA_PROVIDER_DEEPSEEK;
            snprintf(title, sizeof(title), "%s · %s", deepseek ? "DeepSeek" : claude ? "Claude" : "ChatGPT", plan);
            char email[QUOTA_EMAIL_MAX_BYTES + 1];
            quota_copy_display_ascii(account->email, email, sizeof(email));
            if (deepseek) quota_copy_display_ascii(service->snapshot.balances[i].label, email, sizeof(email));
            set_label_text(s_ui.account_primary[i], title);
            set_label_text(s_ui.account_secondary[i], email);
        }
    }
    for (size_t i = visible_rows; i < QUOTA_MAX_ACCOUNTS + 1; i++) {
        set_account_row_visible(i, false);
    }
}

static void render_interval(const quota_navigation_t *navigation,
                            const quota_service_view_t *service)
{
    static const char *const labels[] = {"自动刷新", "1 分钟", "5 分钟", "15 分钟", "30 分钟"};
    static const uint16_t seconds[] = {0, 60, 300, 900, 1800};
    for (size_t i = 0; i < 5; i++) {
        set_row_focus(s_ui.interval_rows[i], s_ui.interval_markers[i],
                      i == navigation->interval_focus);
        set_label_text(s_ui.interval_labels[i], labels[i]);
        if (i == 0) {
            set_label_text(s_ui.interval_values[i], service->auto_refresh ? "开" : "关");
            lv_obj_set_style_text_color(s_ui.interval_values[i],
                color(service->auto_refresh ? UI_MINT : UI_MUTED), 0);
        } else {
            set_label_text(s_ui.interval_values[i], seconds[i] == service->refresh_seconds ? "选中" : "");
        }
    }
}

static void render_setup(const quota_service_view_t *service)
{
    char countdown[32];
    if (service->pairing_active) {
        uint32_t seconds = service->pairing_seconds_left;
        snprintf(countdown, sizeof(countdown), "配对窗口 %02u:%02u",
                 (unsigned)(seconds / 60), (unsigned)(seconds % 60));
        lv_obj_set_style_text_color(s_ui.setup_countdown, color(UI_MINT), 0);
        set_label_text(s_ui.setup_hint, "请通过 USB 提交配置\n设备只接收额度快照");
    } else {
        snprintf(countdown, sizeof(countdown), "配对窗口已关闭");
        lv_obj_set_style_text_color(s_ui.setup_countdown, color(UI_AMBER), 0);
        set_label_text(s_ui.setup_hint, "长按 OK 返回并重新打开\n配置有效期为 2 分钟");
    }
    set_label_text(s_ui.setup_countdown, countdown);
}

static void render_sleep(const quota_navigation_t *navigation,
                          const quota_service_view_t *service)
{
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        set_row_focus(s_ui.sleep_rows[i], s_ui.sleep_markers[i], i == navigation->sleep_focus);
        set_label_text(s_ui.sleep_values[i], quota_screen_timeouts[i] == service->screen_timeout_seconds
                                             ? "选中" : "");
    }
}

void quota_ui_init(void)
{
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, color(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    s_page = NULL;
    s_screen = (quota_screen_t)-1;
}

void quota_ui_render(const quota_navigation_t *navigation,
                     const quota_service_view_t *service,
                     int battery_percent)
{
    if (navigation == NULL || service == NULL || s_root == NULL) return;
    if (s_screen != navigation->screen || s_page == NULL) {
        create_page(navigation->screen);
        lv_screen_load(s_root);
    }

    bool battery_known = battery_percent >= 0 && battery_percent <= 100;
    int fill_width = battery_known ? (22 * battery_percent + 50) / 100 : 0;
    lv_obj_set_width(s_ui.battery_fill, fill_width);
    if (fill_width == 0) lv_obj_add_flag(s_ui.battery_fill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_ui.battery_fill, LV_OBJ_FLAG_HIDDEN);
    if (battery_known) lv_obj_add_flag(s_ui.battery_unknown, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_ui.battery_unknown, LV_OBJ_FLAG_HIDDEN);
    char clock_text[16];
    format_clock(service->clock_synchronized ? service->now_epoch : 0, clock_text, sizeof(clock_text));
    set_label_text(s_ui.clock, clock_text);
    for (size_t i = 0; i < 2; i++) {
        lv_obj_set_style_line_color(s_ui.wifi_lines[i], color(service->connected ? UI_INK : UI_DIM), 0);
    }
    lv_obj_set_style_bg_color(s_ui.wifi_dot, color(service->connected ? UI_INK : UI_DIM), 0);
    if (service->connected) lv_obj_add_flag(s_ui.wifi_slash, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_ui.wifi_slash, LV_OBJ_FLAG_HIDDEN);

    switch (navigation->screen) {
        case QUOTA_SCREEN_HOME:
            render_home(navigation, service);
            break;
        case QUOTA_SCREEN_SETTINGS:
            render_settings(navigation, service);
            break;
        case QUOTA_SCREEN_ACCOUNTS:
            render_accounts(navigation, service);
            break;
        case QUOTA_SCREEN_INTERVAL:
            render_interval(navigation, service);
            break;
        case QUOTA_SCREEN_SLEEP:
            render_sleep(navigation, service);
            break;
        case QUOTA_SCREEN_SETUP:
            render_setup(service);
            break;
        default:
            break;
    }
}
