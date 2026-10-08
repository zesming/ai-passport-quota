#include "quota_ui.h"

#include "bsp_pins.h"
#include "lvgl.h"
#include "quota_brand_assets.h"
#include "quota_portable.h"

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
    lv_obj_t *footer;
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
    lv_obj_t *setting_rows[6];
    lv_obj_t *setting_markers[6];
    lv_obj_t *setting_labels[6];
    lv_obj_t *setting_values[6];
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
    lv_obj_t *network_title;
    lv_obj_t *network_info;
    lv_obj_t *network_hint;
    lv_obj_t *network_saved[QUOTA_PORTABLE_NETWORKS];
    lv_obj_t *device_rows[2];
    lv_obj_t *device_markers[2];
    lv_obj_t *qr;
    lv_obj_t *qr_title;
    lv_obj_t *qr_hint;
    lv_obj_t *qr_countdown;
    lv_obj_t *manual_address;
    lv_obj_t *manual_caption;
    lv_obj_t *manual_secret;
    char qr_data[256];
    lv_obj_t *setup_countdown;
    lv_obj_t *setup_hint;
} quota_ui_objects_t;

static lv_obj_t *s_root;
static lv_obj_t *s_page;
static quota_screen_t s_screen = (quota_screen_t)-1;
static quota_ui_objects_t s_ui;

static const char *portable_error_text(const char *code);
static const char *portable_storage_title(const char *code);

static lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
}

static lv_obj_t *create_rect(lv_obj_t *parent, int x, int y, int width, int height, uint32_t hex,
                             int radius)
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
                              const lv_font_t *font, uint32_t text_color, lv_text_align_t align,
                              const char *text)
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

static lv_obj_t *create_logo(lv_obj_t *parent, const lv_image_dsc_t *image, int x, int y, int width,
                             int height)
{
    lv_obj_t *logo = lv_image_create(parent);
    lv_image_set_src(logo, image);
    lv_obj_set_pos(logo, x, y);
    lv_obj_set_size(logo, width, height);
    if (width < 36)
        lv_image_set_scale(logo, (uint16_t)(width * 256 / 36));
    return logo;
}

static void create_header(const char *title, const char *info)
{
    create_label(s_page, 12, 9, 70, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_LEFT, title);
    s_ui.header_info =
        create_label(s_page, 86, 11, 28, 18, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, info);
    s_ui.clock = create_label(s_page, 122, 11, 40, 18, &lv_font_montserrat_12, UI_MUTED,
                              LV_TEXT_ALIGN_CENTER, "--:--");
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
    s_ui.footer =
        create_label(s_page, 12, 288, 216, 21, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, text);
}

static void create_home_page(void)
{
    create_header("AI 额度", "0/0");
    s_ui.home_logo = create_logo(s_page, &quota_openai_logo, 12, 46, 36, 36);
    s_ui.home_provider = create_label(s_page, 56, 43, 160, 24, &lv_font_montserrat_20, UI_INK,
                                      LV_TEXT_ALIGN_LEFT, "ChatGPT");
    s_ui.home_plan = create_label(s_page, 56, 67, 160, 17, &quota_font_12, UI_MUTED,
                                  LV_TEXT_ALIGN_LEFT, "Codex");
    s_ui.home_email =
        create_label(s_page, 56, 84, 160, 17, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    create_rect(s_page, 12, 103, 216, 1, UI_LINE, 0);

    static const char *const names[] = {"5h 剩余", "7 天剩余"};
    for (size_t i = 0; i < 2; i++) {
        int y = i == 0 ? 107 : 183;
        s_ui.metric_name[i] = create_label(s_page, 12, y, 86, 18, &quota_font_12, UI_INK,
                                           LV_TEXT_ALIGN_LEFT, names[i]);
        s_ui.metric_reset[i] = create_label(s_page, 96, y, 132, 18, &quota_font_12, UI_MUTED,
                                            LV_TEXT_ALIGN_RIGHT, "重置时间未知");
        s_ui.metric_value[i] = create_label(s_page, 12, y + 17, 116, 30, &lv_font_montserrat_20,
                                            UI_MINT, LV_TEXT_ALIGN_LEFT, "--");
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
        s_ui.balance_heading[i] =
            create_label(s_page, 12, y, 216, 18, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
        s_ui.balance_total[i] = create_label(s_page, 12, y + 17, 216, 26, &lv_font_montserrat_20,
                                             UI_INK, LV_TEXT_ALIGN_LEFT, "");
    }
    for (size_t i = 0; i < 2; i++) {
        s_ui.extra_name[i] = create_label(s_page, 12, 228 + (int)i * 16, 72, 16, &quota_font_12,
                                          UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
        s_ui.extra_value[i] = create_label(s_page, 84, 228 + (int)i * 16, 144, 16, &quota_font_12,
                                           UI_INK, LV_TEXT_ALIGN_RIGHT, "");
    }
    s_ui.home_empty = create_label(s_page, 12, 119, 216, 22, &quota_font_16, UI_MUTED,
                                   LV_TEXT_ALIGN_LEFT, "等待额度数据");
    s_ui.home_status = create_label(s_page, 12, 262, 216, 18, &quota_font_12, UI_MUTED,
                                    LV_TEXT_ALIGN_LEFT, "等待额度数据");
    create_footer("UP/DOWN 切换  OK 刷新  长按设置");
}

static void create_focus_row(lv_obj_t **background, lv_obj_t **marker, int y, int height)
{
    *background = create_rect(s_page, 12, y, 216, height, UI_BG, 4);
    *marker = create_rect(s_page, 12, y + 4, 3, height - 8, UI_MINT, 2);
    lv_obj_add_flag(*marker, LV_OBJ_FLAG_HIDDEN);
}

static void create_settings_page(void)
{
    create_header("设置", "");
    static const char *const labels[] = {"账户管理", "刷新间隔", "立即刷新",
                                         "自动息屏", "网络信息", "设备设置"};
    for (size_t i = 0; i < 6; i++) {
        int y = 43 + (int)i * 35;
        create_focus_row(&s_ui.setting_rows[i], &s_ui.setting_markers[i], y, 34);
        s_ui.setting_labels[i] = create_label(s_page, 22, y + 5, 118, 24, &quota_font_16, UI_INK,
                                              LV_TEXT_ALIGN_LEFT, labels[i]);
        s_ui.setting_values[i] = create_label(s_page, 140, y + 7, 82, 20, &quota_font_12, UI_MUTED,
                                              LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 254, 212, 20, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "离线时保留最近数据");
    create_footer("UP/DOWN 选择  OK 确认  长按返回");
}

static void create_sleep_page(void)
{
    create_header("自动息屏", "");
    static const char *const labels[] = {
        "从不", "30 秒", "1 分钟", "2 分钟", "5 分钟", "10 分钟",
    };
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        int y = 43 + (int)i * 35;
        create_focus_row(&s_ui.sleep_rows[i], &s_ui.sleep_markers[i], y, 32);
        create_label(s_page, 22, y + 4, 146, 24, &quota_font_16, UI_INK, LV_TEXT_ALIGN_LEFT,
                     labels[i]);
        s_ui.sleep_values[i] = create_label(s_page, 170, y + 6, 48, 19, &quota_font_12, UI_MUTED,
                                            LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 253, 212, 23, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "任意键亮屏  长按 DOWN 息屏");
    create_footer("UP/DOWN 选择  OK 保存  长按返回");
}

static void create_accounts_page(void)
{
    create_header("账户管理", "0/0");
    for (size_t i = 0; i < QUOTA_MAX_ACCOUNTS + 1; i++) {
        int y = 41 + (int)i * 26;
        create_focus_row(&s_ui.account_rows[i], &s_ui.account_markers[i], y, 25);
        s_ui.account_primary[i] = create_label(s_page, 22, y + 1, 198, 13, &quota_font_12, UI_INK,
                                               LV_TEXT_ALIGN_LEFT, "");
        s_ui.account_secondary[i] = create_label(s_page, 22, y + 13, 198, 12, &quota_font_12,
                                                 UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    }
    create_footer("OK 选择或添加账户  长按返回");
}

static void create_interval_page(void)
{
    create_header("刷新间隔", "");
    static const char *const labels[] = {
        "自动刷新", "1 分钟", "5 分钟", "15 分钟", "30 分钟",
    };
    for (size_t i = 0; i < 5; i++) {
        int y = 49 + (int)i * 43;
        create_focus_row(&s_ui.interval_rows[i], &s_ui.interval_markers[i], y, 36);
        s_ui.interval_labels[i] = create_label(s_page, 22, y + 6, 146, 24, &quota_font_16, UI_INK,
                                               LV_TEXT_ALIGN_LEFT, labels[i]);
        s_ui.interval_values[i] = create_label(s_page, 170, y + 8, 48, 19, &quota_font_12, UI_MUTED,
                                               LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 267, 212, 14, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "全部账户使用此刷新间隔");
    create_footer("UP/DOWN 选择  OK 保存  长按返回");
}

static void create_setup_page(void)
{
    create_header("USB 设置", "");
    create_label(s_page, 20, 51, 200, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "电脑管理账户与设置");
    create_label(s_page, 20, 74, 200, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "账户保存在 Passport");
    create_logo(s_page, &quota_openai_logo, 54, 109, 36, 36);
    create_logo(s_page, &quota_deepseek_logo, 150, 109, 36, 36);
    create_label(s_page, 36, 149, 72, 22, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER, "Codex");
    create_label(s_page, 132, 149, 72, 22, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "DeepSeek");
    s_ui.setup_countdown = create_label(s_page, 15, 190, 210, 25, &quota_font_16, UI_MINT,
                                        LV_TEXT_ALIGN_CENTER, "配对窗口 02:00");
    const char *connect_hint = "请用 USB 线连接电脑\n在设置页连接设备";
    s_ui.setup_hint = create_label(s_page, 16, 220, 208, 44, &quota_font_12, UI_MUTED,
                                   LV_TEXT_ALIGN_CENTER, connect_hint);
    create_footer("长按 OK 返回");
}

static void create_network_page(void)
{
    create_header("网络信息", "");
    s_ui.network_title = create_label(s_page, 12, 49, 216, 24, &quota_font_16, UI_INK,
                                      LV_TEXT_ALIGN_LEFT, "尚未配置网络");
    s_ui.network_info =
        create_label(s_page, 12, 80, 216, 44, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    create_label(s_page, 12, 124, 216, 18, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "已保存 Wi-Fi");
    for (size_t i = 0; i < QUOTA_PORTABLE_NETWORKS; i++) {
        s_ui.network_saved[i] = create_label(s_page, 12, 146 + (int)i * 25, 216, 22, &quota_font_16,
                                             UI_INK, LV_TEXT_ALIGN_LEFT, "");
    }
    s_ui.network_hint =
        create_label(s_page, 12, 255, 216, 24, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "");
    create_footer("长按 OK 返回");
}

static void create_device_settings_page(void)
{
    create_header("设备设置", "");
    static const char *const labels[] = {"热点", "USB"};
    static const char *const hints[] = {
        "手机和电脑管理账户",
        "电脑管理账户与设置",
    };
    for (size_t i = 0; i < 2; i++) {
        int y = 70 + (int)i * 76;
        create_focus_row(&s_ui.device_rows[i], &s_ui.device_markers[i], y, 60);
        create_label(s_page, 22, y + 5, 196, 24, &quota_font_16, UI_INK, LV_TEXT_ALIGN_LEFT,
                     labels[i]);
        create_label(s_page, 22, y + 32, 196, 20, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                     hints[i]);
    }
    create_footer("UP/DOWN 选择  OK 确认  长按返回");
}

static void create_qr_page(bool auth)
{
    create_header(auth ? "账户授权" : "设备设置", auth ? "" : "1/3");
    s_ui.qr_title =
        create_label(s_page, 12, 44, 216, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_CENTER, "");
    s_ui.qr = lv_qrcode_create(s_page);
    lv_obj_set_pos(s_ui.qr, 36, 70);
    lv_qrcode_set_size(s_ui.qr, 168);
    lv_qrcode_set_dark_color(s_ui.qr, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_ui.qr, true);
    lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
    s_ui.qr_hint = create_label(s_page, 12, 242, 216, 22, auth ? &quota_font_16 : &quota_font_12,
                                UI_INK, LV_TEXT_ALIGN_CENTER, "");
    s_ui.qr_countdown =
        create_label(s_page, 12, 265, 216, 14, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER, "");
    if (!auth) {
        s_ui.manual_address = create_label(s_page, 12, 78, 216, 22, &lv_font_montserrat_12, UI_INK,
                                           LV_TEXT_ALIGN_CENTER, "http://192.168.4.1");
        const char *caption = "设置密钥 · 完整输入";
        s_ui.manual_caption = create_label(s_page, 12, 108, 216, 22, &quota_font_12, UI_MUTED,
                                           LV_TEXT_ALIGN_CENTER, caption);
        s_ui.manual_secret = create_label(s_page, 12, 136, 216, 88, &lv_font_montserrat_14, UI_INK,
                                          LV_TEXT_ALIGN_CENTER, "");
        lv_obj_set_style_text_line_space(s_ui.manual_secret, 5, 0);
        lv_label_set_long_mode(s_ui.manual_secret, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_flag(s_ui.manual_address, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.manual_caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.manual_secret, LV_OBJ_FLAG_HIDDEN);
    }
    create_footer(auth ? "长按OK取消 · 长按DOWN暂停" : "UP/DOWN 切换  长按 OK 返回");
}

static void create_page(quota_screen_t screen)
{
    if (s_page != NULL)
        lv_obj_delete(s_page);
    memset(&s_ui, 0, sizeof(s_ui));
    s_page = create_rect(s_root, 0, 0, BSP_LCD_W, BSP_LCD_H, UI_BG, 0);
    s_screen = screen;
    switch (screen) {
    case QUOTA_SCREEN_HOME:
        create_home_page();
        break;
    case QUOTA_SCREEN_SETTINGS:
        create_settings_page();
        break;
    case QUOTA_SCREEN_ACCOUNTS:
        create_accounts_page();
        break;
    case QUOTA_SCREEN_INTERVAL:
        create_interval_page();
        break;
    case QUOTA_SCREEN_SLEEP:
        create_sleep_page();
        break;
    case QUOTA_SCREEN_SETUP:
        create_setup_page();
        break;
    case QUOTA_SCREEN_NETWORK:
        create_network_page();
        break;
    case QUOTA_SCREEN_DEVICE_SETTINGS:
        create_device_settings_page();
        break;
    case QUOTA_SCREEN_PHONE:
        create_qr_page(false);
        break;
    case QUOTA_SCREEN_AUTH:
        create_qr_page(true);
        break;
    default:
        create_home_page();
        break;
    }
}

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (label != NULL && text != NULL && strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static void set_row_focus(lv_obj_t *row, lv_obj_t *marker, bool focused)
{
    if (row == NULL || marker == NULL)
        return;
    lv_color_t tone = color(focused ? UI_PANEL : UI_BG);
    if (!lv_color_eq(lv_obj_get_style_bg_color(row, 0), tone))
        lv_obj_set_style_bg_color(row, tone, 0);
    bool hidden = lv_obj_has_flag(marker, LV_OBJ_FLAG_HIDDEN);
    if (focused && hidden)
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
    else if (!focused && !hidden)
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
}

static void set_account_row_visible(size_t index, bool visible)
{
    lv_obj_t *objects[] = {
        s_ui.account_rows[index],
        s_ui.account_markers[index],
        s_ui.account_primary[index],
        s_ui.account_secondary[index],
    };
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        if (objects[i] == NULL)
            continue;
        if (visible && objects[i] == s_ui.account_markers[index])
            continue;
        if (visible)
            lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
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

static void render_metric(size_t index, const quota_window_t *window, uint64_t now, bool stale,
                          bool clock_synchronized, int y, bool single)
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
    uint32_t tone = stale                             ? UI_DIM
                    : window->remaining_percent <= 5  ? UI_RED
                    : window->remaining_percent <= 20 ? UI_AMBER
                                                      : UI_MINT;
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

static void render_home(const quota_navigation_t *navigation, const quota_service_view_t *service)
{
    char count[16];
    snprintf(count, sizeof(count), "%u/%u",
             service->snapshot.account_count == 0 ? 0
                                                  : (unsigned)(navigation->selected_account + 1),
             (unsigned)service->snapshot.account_count);
    set_label_text(s_ui.header_info, count);
    bool deepseek = service->snapshot.account_count > 0 &&
                    service->snapshot
                            .accounts[navigation->selected_account < service->snapshot.account_count
                                          ? navigation->selected_account
                                          : 0]
                            .provider == QUOTA_PROVIDER_DEEPSEEK;
    for (size_t i = 0; i < QUOTA_BALANCE_CURRENCIES; i++) {
        lv_obj_t *balance_objects[] = {s_ui.balance_heading[i], s_ui.balance_total[i]};
        for (size_t j = 0; j < 2; j++)
            lv_obj_add_flag(balance_objects[j], LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *quota_objects[] = {s_ui.metric_name[i], s_ui.metric_reset[i],
                                     s_ui.metric_value[i], s_ui.metric_bar[i]};
        for (size_t j = 0; j < 4; j++)
            lv_obj_add_flag(quota_objects[j], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.extra_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.extra_value[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_ui.home_empty, LV_OBJ_FLAG_HIDDEN);
    if (service->snapshot.account_count == 0) {
        lv_obj_add_flag(s_ui.home_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_ui.home_provider, &quota_font_16, 0);
        bool storage_error = service->portable.storage_error[0] != '\0';
        set_label_text(s_ui.home_provider,
                       storage_error ? portable_storage_title(service->portable.storage_error)
                                     : "暂无账户");
        set_label_text(s_ui.home_plan, storage_error ? (strcmp(service->portable.storage_error,
                                                               "storage_write_unknown") == 0
                                                            ? "设备正在确认保存"
                                                            : "请保留数据并检查设备")
                                                     : "请打开设备设置添加账户");
        set_label_text(s_ui.home_email, "");
        set_label_text(s_ui.home_status, storage_error
                                             ? portable_error_text(service->portable.storage_error)
                                             : "长按 OK 打开设置");
        lv_obj_set_style_text_color(s_ui.home_status, color(storage_error ? UI_AMBER : UI_MUTED),
                                    0);
        return;
    }

    size_t selected = navigation->selected_account;
    if (selected >= service->snapshot.account_count)
        selected = 0;
    const quota_account_t *account = &service->snapshot.accounts[selected];
    lv_obj_set_style_text_font(s_ui.home_provider, &lv_font_montserrat_20, 0);
    lv_image_set_src(s_ui.home_logo, deepseek ? &quota_deepseek_logo : &quota_openai_logo);
    lv_obj_clear_flag(s_ui.home_logo, LV_OBJ_FLAG_HIDDEN);
    set_label_text(s_ui.home_provider, deepseek ? "DeepSeek" : "ChatGPT");
    char plan[QUOTA_PLAN_MAX_BYTES + 1];
    quota_copy_display_plan(account->plan, plan, sizeof(plan));
    char source_and_plan[QUOTA_PLAN_MAX_BYTES + 20];
    snprintf(source_and_plan, sizeof(source_and_plan), "%s · %s", "Codex",
             plan[0] == '\0' ? "" : plan);
    set_label_text(s_ui.home_plan, source_and_plan);
    if (deepseek)
        set_label_text(s_ui.home_plan, "开放平台 · API");
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
            lv_obj_set_style_text_font(s_ui.balance_total[i],
                                       strlen(entry->total_balance) > 16   ? &lv_font_montserrat_12
                                       : strlen(entry->total_balance) > 12 ? &lv_font_montserrat_14
                                                                           : &lv_font_montserrat_20,
                                       0);
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
            if (!windows[i]->present)
                continue;
            render_metric(i, windows[i], service->now_epoch, stale, service->clock_synchronized,
                          109 + (int)row++ * 60, window_count == 1);
        }
        const quota_codex_extras_t *extras = &service->snapshot.codex_extras[selected];
        int extra_y = window_count == 2 ? 228 : window_count == 1 ? 199 : 151;
        unsigned extra_row = 0;
        if (extras->has_banked_reset && extras->available_resets > 0) {
            char value[64];
            if (extras->has_next_reset_expiry && !service->clock_synchronized) {
                snprintf(value, sizeof(value), "%llu次 · 时间待同步",
                         (unsigned long long)extras->available_resets);
            } else if (extras->has_next_reset_expiry &&
                       extras->next_reset_expires_at > service->now_epoch) {
                char expiry[20];
                quota_format_duration(extras->next_reset_expires_at - service->now_epoch, expiry,
                                      sizeof(expiry));
                snprintf(value, sizeof(value), "%llu次 · %s 到期",
                         (unsigned long long)extras->available_resets, expiry);
            } else if (extras->has_next_reset_expiry) {
                snprintf(value, sizeof(value), "%llu次 · 等待更新",
                         (unsigned long long)extras->available_resets);
            } else {
                snprintf(value, sizeof(value), "%llu 次",
                         (unsigned long long)extras->available_resets);
            }
            render_extra(extra_row++, "可用重置", value, extra_y, stale);
        }
        if (extras->has_credits) {
            char value[QUOTA_CREDITS_BALANCE_BYTES + 1];
            quota_copy_display_ascii(extras->credits_balance, value, sizeof(value));
            render_extra(extra_row, "剩余额度",
                         extras->unlimited_credits ? "不限量"
                         : value[0] != '\0'        ? value
                                                   : "可用",
                         extra_y + (int)extra_row * 16, stale);
        }
        if (window_count == 0) {
            const char *empty =
                account->status == QUOTA_STATUS_OK ? "未提供额度窗口" : "等待额度数据";
            set_label_text(s_ui.home_empty, empty);
            lv_obj_clear_flag(s_ui.home_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }

    char status[96];
    const char *account_error = service->portable.account_errors[selected];
    bool connected = service->portable.network_state == QUOTA_PORTABLE_NETWORK_READY ||
                     service->portable.network_state == QUOTA_PORTABLE_NETWORK_CONNECTED;
    if (service->portable.storage_error[0]) {
        snprintf(status, sizeof(status), "%s",
                 portable_error_text(service->portable.storage_error));
    } else if (!connected) {
        snprintf(status, sizeof(status), "离线 · 最近数据");
    } else if (account_error[0]) {
        uint64_t retry = service->portable.account_retry_at[selected];
        if (strcmp(account_error, "rate_limited") == 0 && service->clock_synchronized &&
            retry > service->now_epoch) {
            snprintf(status, sizeof(status), "请求过多 · %llu 秒后重试",
                     (unsigned long long)(retry - service->now_epoch));
        } else
            snprintf(status, sizeof(status), "%s", portable_error_text(account_error));
    } else if (service->refreshing) {
        snprintf(status, sizeof(status), "正在刷新");
    } else if (!service->clock_synchronized) {
        snprintf(status, sizeof(status), "时间待同步 · 最近数据");
    } else if (account->status == QUOTA_STATUS_EXPIRED) {
        snprintf(status, sizeof(status), "登录过期 · 最近数据");
    } else if (account->status == QUOTA_STATUS_WAITING) {
        snprintf(status, sizeof(status), "待验证 · 结束设备设置");
    } else if (account->status == QUOTA_STATUS_ERROR) {
        snprintf(status, sizeof(status), "数据源错误 · 保留缓存");
    } else if (account->status == QUOTA_STATUS_UNSUPPORTED) {
        snprintf(status, sizeof(status), "数据源暂不支持");
    } else if (deepseek && balance->present && !balance->is_available) {
        snprintf(status, sizeof(status), "余额不可用");
    } else if (account->has_observed_at) {
        char clock_text[16];
        format_clock(account->observed_at, clock_text, sizeof(clock_text));
        if (stale)
            snprintf(status, sizeof(status), "缓存 · 更新于 %s", clock_text);
        else
            snprintf(status, sizeof(status), "更新于 %s", clock_text);
    } else {
        snprintf(status, sizeof(status), "尚未采集数据");
    }
    set_label_text(s_ui.home_status, status);
    lv_obj_set_style_text_color(
        s_ui.home_status,
        color((service->portable.storage_error[0] || !connected || stale) ? UI_AMBER : UI_MUTED),
        0);
}

static void render_settings(const quota_navigation_t *navigation,
                            const quota_service_view_t *service)
{
    static const char *const labels[] = {"账户管理", "刷新间隔", "立即刷新",
                                         "自动息屏", "网络信息", "设备设置"};
    char values[6][24];
    snprintf(values[0], sizeof(values[0]), "%u 个", (unsigned)service->snapshot.account_count);
    if (!service->auto_refresh)
        snprintf(values[1], sizeof(values[1]), "手动");
    else
        snprintf(values[1], sizeof(values[1]), "%u 分钟",
                 (unsigned)(service->refresh_seconds / 60));
    snprintf(values[2], sizeof(values[2]), "%s", service->refreshing ? "同步中" : "");
    if (service->screen_timeout_seconds == 0)
        snprintf(values[3], sizeof(values[3]), "从不");
    else if (service->screen_timeout_seconds < 60) {
        snprintf(values[3], sizeof(values[3]), "%u 秒", (unsigned)service->screen_timeout_seconds);
    } else {
        snprintf(values[3], sizeof(values[3]), "%u 分钟",
                 (unsigned)(service->screen_timeout_seconds / 60));
    }
    snprintf(values[4], sizeof(values[4]), "%u 个",
             (unsigned)service->portable.saved_network_count);
    snprintf(values[5], sizeof(values[5]), "%s",
             service->usb_window_preparing                                  ? "准备中"
             : service->usb_window_active || service->portable.setup_active ? "已打开"
                                                                            : "");
    for (size_t i = 0; i < 6; i++) {
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
                            ? navigation->account_focus + 1
                            : service->snapshot.account_count + 1),
             (unsigned)(service->snapshot.account_count + 1));
    set_label_text(s_ui.header_info, count);
    size_t visible_rows = (size_t)service->snapshot.account_count + 1;
    if (visible_rows > QUOTA_MAX_ACCOUNTS + 1)
        visible_rows = QUOTA_MAX_ACCOUNTS + 1;
    for (size_t i = 0; i < visible_rows; i++) {
        set_account_row_visible(i, true);
        bool selected = i == navigation->account_focus;
        set_row_focus(s_ui.account_rows[i], s_ui.account_markers[i], selected);
        if (i == service->snapshot.account_count) {
            set_label_text(s_ui.account_primary[i], "添加账户");
            set_label_text(s_ui.account_secondary[i], "手机或电脑打开设备设置");
        } else {
            const quota_account_t *account = &service->snapshot.accounts[i];
            char title[QUOTA_PLAN_MAX_BYTES + 20];
            char plan[QUOTA_PLAN_MAX_BYTES + 1];
            quota_copy_display_plan(account->plan, plan, sizeof(plan));
            bool deepseek = account->provider == QUOTA_PROVIDER_DEEPSEEK;
            snprintf(title, sizeof(title), "%s · %s", deepseek ? "DeepSeek" : "ChatGPT", plan);
            char email[QUOTA_EMAIL_MAX_BYTES + 1];
            quota_copy_display_ascii(account->email, email, sizeof(email));
            if (deepseek)
                quota_copy_display_ascii(service->snapshot.balances[i].label, email, sizeof(email));
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
    static const char *const labels[] = {
        "自动刷新", "1 分钟", "5 分钟", "15 分钟", "30 分钟",
    };
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
            set_label_text(s_ui.interval_values[i],
                           seconds[i] == service->refresh_seconds ? "选中" : "");
        }
    }
}

static void render_setup(const quota_service_view_t *service)
{
    if (service->portable.storage_error[0]) {
        set_label_text(s_ui.setup_countdown,
                       portable_storage_title(service->portable.storage_error));
        lv_obj_set_style_text_color(s_ui.setup_countdown, color(UI_AMBER), 0);
        set_label_text(s_ui.setup_hint, portable_error_text(service->portable.storage_error));
        return;
    }
    char countdown[32];
    if (service->usb_window_preparing) {
        snprintf(countdown, sizeof(countdown), "正在准备 USB 设置");
        lv_obj_set_style_text_color(s_ui.setup_countdown, color(UI_AMBER), 0);
        bool saving = strcmp(service->portable.login_error, "storage_failed") == 0;
        const char *waiting = saving ? "等待保存账户\n准备完成后开始 2 分钟配对"
                                     : "请稍候\n准备完成后开始 2 分钟配对";
        set_label_text(s_ui.setup_hint, waiting);
    } else if (service->usb_window_active) {
        uint32_t seconds = service->usb_window_seconds_left;
        snprintf(countdown, sizeof(countdown), "配对窗口 %02u:%02u", (unsigned)(seconds / 60),
                 (unsigned)(seconds % 60));
        lv_obj_set_style_text_color(s_ui.setup_countdown, color(UI_MINT), 0);
        set_label_text(s_ui.setup_hint, "请用 USB 线连接电脑\n在设置页连接设备");
    } else {
        snprintf(countdown, sizeof(countdown), "配对窗口已关闭");
        lv_obj_set_style_text_color(s_ui.setup_countdown, color(UI_AMBER), 0);
        const char *reopen = "长按 OK 返回并重新打开\n配置有效期为 2 分钟";
        set_label_text(s_ui.setup_hint, reopen);
    }
    set_label_text(s_ui.setup_countdown, countdown);
}

static void render_sleep(const quota_navigation_t *navigation, const quota_service_view_t *service)
{
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        set_row_focus(s_ui.sleep_rows[i], s_ui.sleep_markers[i], i == navigation->sleep_focus);
        set_label_text(s_ui.sleep_values[i],
                       quota_screen_timeouts[i] == service->screen_timeout_seconds ? "选中" : "");
    }
}

static const char *portable_storage_title(const char *code)
{
    if (strcmp(code, "storage_invalid") == 0)
        return "存储记录损坏";
    if (strcmp(code, "recovery_conflict") == 0)
        return "存储冲突";
    if (strcmp(code, "storage_write_unknown") == 0)
        return "保存待确认";
    if (strcmp(code, "storage_io_error") == 0)
        return "存储暂不可读";
    if (strcmp(code, "storage_missing") == 0)
        return "存储记录不存在";
    if (strcmp(code, "storage_busy") == 0)
        return "存储处理中";
    if (strcmp(code, "no_memory") == 0)
        return "设备资源不足";
    if (strcmp(code, "storage_failed") == 0)
        return "保存失败";
    return "存储暂不可用";
}

static const char *portable_error_text(const char *code)
{
    if (strcmp(code, "wifi_auth_failed") == 0)
        return "密码错误 · 请重新配网";
    if (strcmp(code, "wifi_not_found") == 0)
        return "找不到网络 · 请检查热点";
    if (strcmp(code, "network_unavailable") == 0)
        return "服务连接失败或超时";
    if (strcmp(code, "rate_limited") == 0)
        return "请求过多 · 稍后重试";
    if (strcmp(code, "login_disabled") == 0)
        return "请开启设备码授权";
    if (strcmp(code, "codex_expired") == 0 || strcmp(code, "auth_required") == 0)
        return "需要重新授权";
    if (strcmp(code, "deepseek_invalid_key") == 0)
        return "密钥无效 · 请更换密钥";
    if (strcmp(code, "time_required") == 0)
        return "时间待同步";
    if (strcmp(code, "provider_response_invalid") == 0)
        return "服务暂不兼容 · 稍后重试";
    if (strcmp(code, "no_memory") == 0 || strcmp(code, "resource_error") == 0)
        return "设备资源不足 · 稍后重试";
    if (strcmp(code, "tls_error") == 0)
        return "服务安全连接失败";
    if (strcmp(code, "response_too_large") == 0)
        return "服务响应过大 · 请稍后重试";
    if (strcmp(code, "configuration_changed") == 0)
        return "配置已变化 · 请重新设置";
    if (strcmp(code, "recovery_conflict") == 0)
        return "存储冲突 · 请检查设备";
    if (strcmp(code, "storage_invalid") == 0)
        return "存储记录损坏 · 请检查";
    if (strcmp(code, "storage_io_error") == 0)
        return "存储暂不可读 · 请重试";
    if (strcmp(code, "storage_missing") == 0)
        return "存储记录不存在 · 请检查";
    if (strcmp(code, "canceled") == 0)
        return "操作已取消";
    if (strcmp(code, "unsupported") == 0)
        return "此操作暂不支持";
    if (strcmp(code, "busy") == 0)
        return "正在处理 · 请稍候";
    if (strcmp(code, "storage_failed") == 0)
        return "保存失败 · 请重试";
    if (strcmp(code, "storage_write_unknown") == 0)
        return "保存待确认 · 请稍候";
    if (strcmp(code, "storage_busy") == 0)
        return "存储处理中 · 请稍候";
    return "操作未完成 · 请重试";
}

static void render_network(const quota_portable_view_t *portable)
{
    for (size_t i = 0; i < QUOTA_PORTABLE_NETWORKS; i++) {
        char line[QUOTA_SSID_MAX_BYTES + 24] = "";
        char saved[QUOTA_SSID_MAX_BYTES + 1];
        if (!portable->storage_error[0] && i < portable->saved_network_count) {
            quota_copy_display_ascii(portable->saved_network_ssids[i], saved, sizeof(saved));
            snprintf(line, sizeof(line), "%s%s",
                     i == portable->selected_saved_network ? "* " : "  ", saved);
        }
        set_label_text(s_ui.network_saved[i], line);
    }
    char ssid[QUOTA_SSID_MAX_BYTES + 1];
    quota_copy_display_ascii(portable->network_ssid, ssid, sizeof(ssid));
    if (portable->storage_error[0]) {
        set_label_text(s_ui.network_title, portable_storage_title(portable->storage_error));
        set_label_text(s_ui.network_info, portable_error_text(portable->storage_error));
        set_label_text(s_ui.network_hint,
                       strcmp(portable->storage_error, "storage_write_unknown") == 0
                           ? "设备正在确认保存"
                           : "请保留数据并检查设备");
        return;
    }
    set_label_text(s_ui.network_hint, "修改网络请打开设备设置");
    set_label_text(s_ui.network_title, ssid[0] ? ssid : "尚未配置网络");
    char info[80];
    const char *status = "网络未连接";
    switch (portable->network_state) {
    case QUOTA_PORTABLE_NETWORK_READY:
        status = "网络已连接";
        break;
    case QUOTA_PORTABLE_NETWORK_CONNECTING:
        status = "正在连接网络";
        break;
    case QUOTA_PORTABLE_NETWORK_AP:
        status = "设备设置已打开";
        break;
    case QUOTA_PORTABLE_NETWORK_TIME_REQUIRED:
        status = "时间待同步";
        break;
    case QUOTA_PORTABLE_NETWORK_CONNECTED:
        status = "网络已连接 · 时间待同步";
        break;
    case QUOTA_PORTABLE_NETWORK_ERROR:
        status = portable_error_text(portable->network_error);
        break;
    default:
        break;
    }
    snprintf(info, sizeof(info), "%s\n%s", status, portable->network_ip);
    set_label_text(s_ui.network_info, info);
}

static void render_device_settings(const quota_navigation_t *navigation)
{
    for (size_t i = 0; i < 2; i++)
        set_row_focus(s_ui.device_rows[i], s_ui.device_markers[i],
                      navigation->device_settings_focus == i);
}

static void render_qr_data(const char *data)
{
    if (data == NULL || data[0] == '\0') {
        s_ui.qr_data[0] = '\0';
        lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (strcmp(s_ui.qr_data, data) != 0) {
        if (strlen(data) >= sizeof(s_ui.qr_data) ||
            lv_qrcode_update(s_ui.qr, data, (uint32_t)strlen(data)) != LV_RESULT_OK) {
            lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
            set_label_text(s_ui.qr_title, "二维码生成失败");
            return;
        }
        snprintf(s_ui.qr_data, sizeof(s_ui.qr_data), "%s", data);
    }
    lv_obj_clear_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
}

static void render_phone(const quota_navigation_t *navigation,
                         const quota_portable_view_t *portable)
{
    bool second = navigation->phone_step == 1;
    bool manual = navigation->phone_step == 2;
    bool ready = portable->setup_active && portable->setup_ready && !portable->storage_error[0];
    set_label_text(s_ui.header_info, manual ? "3/3" : second ? "2/3" : "1/3");
    lv_obj_set_y(s_ui.qr, second ? 70 : 66);
    lv_obj_set_y(s_ui.qr_hint, manual ? 229 : second ? 242 : 235);
    lv_obj_set_height(s_ui.qr_hint, manual ? 34 : second ? 22 : 28);
    lv_obj_t *manual_labels[] = {s_ui.manual_address, s_ui.manual_caption, s_ui.manual_secret};
    for (size_t i = 0; i < 3; i++) {
        if (manual && ready)
            lv_obj_clear_flag(manual_labels[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(manual_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    char countdown[40];
    snprintf(countdown, sizeof(countdown), "OK 下一步 · 窗口 %02u:%02u",
             (unsigned)(portable->setup_seconds_left / 60),
             (unsigned)(portable->setup_seconds_left % 60));
    set_label_text(s_ui.qr_countdown, countdown);
    if (portable->storage_error[0]) {
        render_qr_data(NULL);
        set_label_text(s_ui.manual_secret, "");
        set_label_text(s_ui.qr_title, portable_storage_title(portable->storage_error));
        set_label_text(s_ui.qr_hint, portable_error_text(portable->storage_error));
        set_label_text(s_ui.qr_countdown,
                       strcmp(portable->storage_error, "storage_write_unknown") == 0
                           ? "等待保存确认"
                           : "请保留数据并检查设备");
        return;
    }
    if (!ready) {
        render_qr_data(NULL);
        set_label_text(s_ui.manual_secret, "");
        if (portable->setup_active)
            set_label_text(s_ui.qr_title, "正在打开热点");
        else
            set_label_text(s_ui.qr_title, "设置窗口已关闭");
        set_label_text(s_ui.qr_hint, portable->setup_active ? "请稍候" : "按 OK 重新打开");
        return;
    }
    if (manual) {
        /* XXXX-XXXX-XXXX-XXXX on two lines of 9 characters. */
        char grouped[24];
        snprintf(grouped, sizeof(grouped), "%.9s\n%.9s", portable->setup_secret,
                 portable->setup_secret + 10);
        set_label_text(s_ui.manual_secret, grouped);
        set_label_text(s_ui.qr_title, "手动打开设置");
        const char *manual_hint = "手机或电脑连接设备热点\n在网页输入完整密钥";
        set_label_text(s_ui.qr_hint, manual_hint);
        render_qr_data(NULL);
        return;
    }
    set_label_text(s_ui.manual_secret, "");
    set_label_text(s_ui.qr_title, second ? "打开设置网页" : "连接设备热点");
    char data[256];
    if (second) {
        snprintf(data, sizeof(data), "%s", portable->setup_page_url);
        set_label_text(s_ui.qr_hint, "手机或电脑 · 扫码打开");
    } else {
        /* Firmware-generated AP name/password use QR-safe ASCII only. */
        snprintf(data, sizeof(data), "WIFI:T:WPA;S:%s;P:%s;;", portable->setup_ssid,
                 portable->setup_password);
        char fallback[112];
        snprintf(fallback, sizeof(fallback), "%s\n密码: %s", portable->setup_ssid,
                 portable->setup_password);
        set_label_text(s_ui.qr_hint, fallback);
    }
    render_qr_data(data);
}

static void render_auth(const quota_portable_view_t *portable)
{
    const char *title = "正在准备授权";
    const char *hint = "请打开手机热点或可用网络";
    bool show_qr = portable->login_state == QUOTA_PORTABLE_LOGIN_WAITING;
    bool terminal = portable->login_state == QUOTA_PORTABLE_LOGIN_SUCCESS ||
                    portable->login_state == QUOTA_PORTABLE_LOGIN_EXPIRED ||
                    portable->login_state == QUOTA_PORTABLE_LOGIN_CANCELED ||
                    portable->login_state == QUOTA_PORTABLE_LOGIN_ERROR;
    bool saving = portable->login_state == QUOTA_PORTABLE_LOGIN_EXCHANGING &&
                  strcmp(portable->login_error, "storage_failed") == 0;
    set_label_text(s_ui.footer, saving     ? "正在保存 · 请稍候"
                                : terminal ? "长按 OK 返回"
                                           : "OK USB 设置 · 长按OK取消");
    switch (portable->login_state) {
    case QUOTA_PORTABLE_LOGIN_CONNECTING:
        title = "正在连接网络";
        break;
    case QUOTA_PORTABLE_LOGIN_REQUESTING_CODE:
        title = "正在获取验证码";
        break;
    case QUOTA_PORTABLE_LOGIN_WAITING:
        title = "扫码打开官方验证页";
        hint = portable->login_user_code;
        break;
    case QUOTA_PORTABLE_LOGIN_EXCHANGING:
        title = portable->login_error[0] && strcmp(portable->login_error, "storage_failed") == 0
                    ? "正在保存账户"
                    : "正在完成授权";
        hint = portable->login_error[0] && strcmp(portable->login_error, "storage_failed") == 0
                   ? "请稍候 · 保存后再退出"
                   : "请稍候";
        break;
    case QUOTA_PORTABLE_LOGIN_SUCCESS:
        title = "账户已连接";
        hint = "正在查询额度";
        break;
    case QUOTA_PORTABLE_LOGIN_EXPIRED:
        title = "授权已过期";
        hint = "返回并重新打开设备设置";
        break;
    case QUOTA_PORTABLE_LOGIN_CANCELED:
        title = "授权已取消";
        hint = "长按 OK 返回";
        break;
    case QUOTA_PORTABLE_LOGIN_ERROR:
        title = "授权未完成";
        hint = portable_error_text(portable->login_error);
        break;
    default:
        break;
    }
    if (portable->storage_error[0]) {
        title = portable_storage_title(portable->storage_error);
        hint = portable_error_text(portable->storage_error);
        show_qr = false;
        set_label_text(s_ui.footer, strcmp(portable->storage_error, "storage_write_unknown") == 0
                                        ? "保存待确认 · 请稍候"
                                        : "请保留数据 · 检查设备");
    }
    set_label_text(s_ui.qr_title, title);
    lv_obj_set_style_text_font(s_ui.qr_hint, show_qr ? &quota_font_16 : &quota_font_12, 0);
    bool long_code = show_qr && strlen(hint) > 32;
    unsigned qr_size = long_code ? 144 : 168;
    if ((unsigned)lv_obj_get_width(s_ui.qr) != qr_size) {
        lv_qrcode_set_size(s_ui.qr, qr_size);
        s_ui.qr_data[0] = '\0';
    }
    lv_obj_set_x(s_ui.qr, long_code ? 48 : 36);
    lv_obj_set_y(s_ui.qr_countdown, long_code ? 273 : 265);
    char code[QUOTA_PORTABLE_USER_CODE_BYTES + 4];
    if (long_code) {
        size_t length = strlen(hint), at = 0;
        for (size_t i = 0; i < length; i++) {
            if (i && i % 16 == 0)
                code[at++] = '\n';
            code[at++] = hint[i];
        }
        code[at] = '\0';
        lv_obj_set_style_text_font(s_ui.qr_hint, &quota_font_12, 0);
        lv_obj_set_y(s_ui.qr, 66);
        lv_obj_set_y(s_ui.qr_hint, 211);
        lv_obj_set_height(s_ui.qr_hint, 56);
        set_label_text(s_ui.qr_hint, code);
    } else if (show_qr && strlen(hint) > 20) {
        snprintf(code, sizeof(code), "%.16s\n%s", hint, hint + 16);
        lv_obj_set_style_text_font(s_ui.qr_hint, &quota_font_12, 0);
        lv_obj_set_y(s_ui.qr, 66);
        lv_obj_set_y(s_ui.qr_hint, 235);
        lv_obj_set_height(s_ui.qr_hint, 28);
        set_label_text(s_ui.qr_hint, code);
    } else {
        lv_obj_set_y(s_ui.qr, 70);
        lv_obj_set_y(s_ui.qr_hint, 242);
        lv_obj_set_height(s_ui.qr_hint, 22);
        set_label_text(s_ui.qr_hint, hint);
    }
    render_qr_data(show_qr ? portable->login_url : NULL);
    char countdown[40];
    snprintf(countdown, sizeof(countdown), "有效时间 %02u:%02u",
             (unsigned)(portable->login_seconds_left / 60),
             (unsigned)(portable->login_seconds_left % 60));
    set_label_text(s_ui.qr_countdown,
                   portable->storage_error[0]
                       ? (strcmp(portable->storage_error, "storage_write_unknown") == 0
                              ? "等待保存确认"
                              : "请检查设备存储")
                       : countdown);
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

void quota_ui_render(const quota_navigation_t *navigation, const quota_service_view_t *service,
                     int battery_percent)
{
    if (navigation == NULL || service == NULL || s_root == NULL)
        return;
    if (s_screen != navigation->screen || s_page == NULL) {
        create_page(navigation->screen);
        lv_screen_load(s_root);
    }

    bool battery_known = battery_percent >= 0 && battery_percent <= 100;
    int fill_width = battery_known ? (22 * battery_percent + 50) / 100 : 0;
    lv_obj_set_width(s_ui.battery_fill, fill_width);
    if (fill_width == 0)
        lv_obj_add_flag(s_ui.battery_fill, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(s_ui.battery_fill, LV_OBJ_FLAG_HIDDEN);
    if (battery_known)
        lv_obj_add_flag(s_ui.battery_unknown, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(s_ui.battery_unknown, LV_OBJ_FLAG_HIDDEN);
    char clock_text[16];
    format_clock(service->clock_synchronized ? service->now_epoch : 0, clock_text,
                 sizeof(clock_text));
    set_label_text(s_ui.clock, clock_text);
    for (size_t i = 0; i < 2; i++) {
        lv_color_t tone = color(service->connected ? UI_INK : UI_DIM);
        if (!lv_color_eq(lv_obj_get_style_line_color(s_ui.wifi_lines[i], 0), tone))
            lv_obj_set_style_line_color(s_ui.wifi_lines[i], tone, 0);
    }
    lv_color_t wifi_tone = color(service->connected ? UI_INK : UI_DIM);
    if (!lv_color_eq(lv_obj_get_style_bg_color(s_ui.wifi_dot, 0), wifi_tone))
        lv_obj_set_style_bg_color(s_ui.wifi_dot, wifi_tone, 0);
    if (service->connected)
        lv_obj_add_flag(s_ui.wifi_slash, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(s_ui.wifi_slash, LV_OBJ_FLAG_HIDDEN);

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
    case QUOTA_SCREEN_NETWORK:
        render_network(&service->portable);
        break;
    case QUOTA_SCREEN_DEVICE_SETTINGS:
        render_device_settings(navigation);
        break;
    case QUOTA_SCREEN_PHONE:
        render_phone(navigation, &service->portable);
        break;
    case QUOTA_SCREEN_AUTH:
        render_auth(&service->portable);
        break;
    default:
        break;
    }
}
