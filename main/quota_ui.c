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

typedef struct {
    lv_obj_t *title;
    lv_obj_t *header_info;
    lv_obj_t *battery;
    lv_obj_t *home_logo;
    lv_obj_t *home_provider;
    lv_obj_t *home_plan;
    lv_obj_t *home_email;
    lv_obj_t *metric_name[2];
    lv_obj_t *metric_reset[2];
    lv_obj_t *metric_value[2];
    lv_obj_t *metric_bar[2];
    lv_obj_t *home_status;
    lv_obj_t *setting_rows[4];
    lv_obj_t *setting_markers[4];
    lv_obj_t *setting_labels[4];
    lv_obj_t *setting_values[4];
    lv_obj_t *account_rows[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *account_markers[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *account_primary[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *account_secondary[QUOTA_MAX_ACCOUNTS + 1];
    lv_obj_t *interval_rows[5];
    lv_obj_t *interval_markers[5];
    lv_obj_t *interval_labels[5];
    lv_obj_t *interval_values[5];
    lv_obj_t *setup_openai_logo;
    lv_obj_t *setup_claude_logo;
    lv_obj_t *setup_openai_name;
    lv_obj_t *setup_claude_name;
    lv_obj_t *setup_countdown;
    lv_obj_t *setup_hint;
    lv_obj_t *footer;
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
    s_ui.title = create_label(s_page, 12, 9, 130, 22, &quota_font_16,
                              UI_INK, LV_TEXT_ALIGN_LEFT, title);
    s_ui.header_info = create_label(s_page, 142, 11, 48, 18, &quota_font_12,
                                    UI_MUTED, LV_TEXT_ALIGN_RIGHT, info);
    s_ui.battery = create_label(s_page, 194, 11, 34, 18, &quota_font_12,
                                UI_MINT, LV_TEXT_ALIGN_RIGHT, "");
    create_rect(s_page, 12, 35, 216, 1, UI_LINE, 0);
}

static void create_footer(const char *text)
{
    create_rect(s_page, 12, 280, 216, 1, UI_LINE, 0);
    s_ui.footer = create_label(s_page, 12, 288, 216, 21, &quota_font_12,
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
    s_ui.home_status = create_label(s_page, 12, 254, 216, 18, &quota_font_12,
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
    create_header("设置", "未连接");
    static const char *const labels[] = {"账户管理", "刷新间隔", "立即刷新", "电脑配对"};
    for (size_t i = 0; i < 4; i++) {
        int y = 48 + (int)i * 48;
        create_focus_row(&s_ui.setting_rows[i], &s_ui.setting_markers[i], y, 40);
        s_ui.setting_labels[i] = create_label(s_page, 22, y + 8, 118, 24,
                                             &quota_font_16, UI_INK,
                                             LV_TEXT_ALIGN_LEFT, labels[i]);
        s_ui.setting_values[i] = create_label(s_page, 140, y + 10, 82, 20,
                                             &quota_font_12, UI_MUTED,
                                             LV_TEXT_ALIGN_RIGHT, "");
    }
    create_label(s_page, 16, 244, 212, 30, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_LEFT, "离线时保留最近数据\n账户登录由电脑端管理");
    create_footer("UP/DOWN 选择  OK 确认  长按返回");
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
                 LV_TEXT_ALIGN_CENTER, "选择平台并完成登录");
    s_ui.setup_openai_logo = create_logo(s_page, &quota_openai_logo, 58, 109, 36, 36);
    s_ui.setup_claude_logo = create_logo(s_page, &quota_claude_logo, 146, 109, 36, 36);
    s_ui.setup_openai_name = create_label(s_page, 26, 149, 100, 22, &quota_font_12,
                                         UI_INK, LV_TEXT_ALIGN_CENTER, "ChatGPT / Codex");
    s_ui.setup_claude_name = create_label(s_page, 124, 149, 90, 22, &quota_font_12,
                                         UI_INK, LV_TEXT_ALIGN_CENTER, "Claude Code");
    create_rect(s_page, 119, 108, 1, 65, UI_LINE, 0);
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

static void battery_text(int battery_percent, char output[8])
{
    if (battery_percent < 0 || battery_percent > 100) {
        output[0] = '\0';
        return;
    }
    snprintf(output, 8, "%d%%", battery_percent);
}

static void format_clock(uint64_t epoch, bool include_date, char *output, size_t capacity)
{
    time_t value = (time_t)epoch;
    struct tm local = {0};
    if (localtime_r(&value, &local) == NULL ||
        strftime(output, capacity, include_date ? "%m/%d %H:%M" : "%H:%M", &local) == 0) {
        snprintf(output, capacity, "--:--");
    }
}

static void metric_reset_text(const quota_window_t *window, uint64_t now,
                              bool include_date, char *output, size_t capacity)
{
    if (window == NULL || !window->present || !window->has_resets_at) {
        snprintf(output, capacity, "重置时间未知");
    } else if (now >= window->resets_at) {
        snprintf(output, capacity, "等待新数据");
    } else {
        char clock_text[20];
        format_clock(window->resets_at, include_date, clock_text, sizeof(clock_text));
        snprintf(output, capacity, "%s 重置", clock_text);
    }
}

static void render_metric(size_t index, const quota_window_t *window, uint64_t now,
                          bool stale, bool include_date)
{
    char reset_text[32];
    metric_reset_text(window, now, include_date, reset_text, sizeof(reset_text));
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

static void render_home(const quota_navigation_t *navigation,
                        const quota_service_view_t *service)
{
    char count[16];
    snprintf(count, sizeof(count), "%u/%u", (unsigned)(navigation->selected_account + 1),
             (unsigned)service->snapshot.account_count);
    set_label_text(s_ui.header_info, count);
    if (service->snapshot.account_count == 0) {
        lv_obj_add_flag(s_ui.home_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_ui.home_provider, &quota_font_16, 0);
        set_label_text(s_ui.home_provider, "暂无账户");
        set_label_text(s_ui.home_plan, service->configured ? "请在电脑端添加账户" : "等待电脑配对");
        set_label_text(s_ui.home_email, "");
        for (size_t i = 0; i < 2; i++) {
            set_label_text(s_ui.metric_value[i], "未提供");
            lv_obj_set_style_text_font(s_ui.metric_value[i], &quota_font_12, 0);
            lv_obj_set_style_text_color(s_ui.metric_value[i], color(UI_DIM), 0);
            lv_obj_add_flag(s_ui.metric_bar[i], LV_OBJ_FLAG_HIDDEN);
            set_label_text(s_ui.metric_reset[i], "重置时间未知");
        }
        set_label_text(s_ui.home_status, service->configured ? "等待额度快照" : "请先完成 USB 配置");
        return;
    }

    size_t selected = navigation->selected_account;
    if (selected >= service->snapshot.account_count) selected = 0;
    const quota_account_t *account = &service->snapshot.accounts[selected];
    bool is_claude = account->provider == QUOTA_PROVIDER_CLAUDE;
    lv_obj_set_style_text_font(s_ui.home_provider, &lv_font_montserrat_20, 0);
    lv_image_set_src(s_ui.home_logo, is_claude ? &quota_claude_logo : &quota_openai_logo);
    lv_obj_clear_flag(s_ui.home_logo, LV_OBJ_FLAG_HIDDEN);
    set_label_text(s_ui.home_provider, is_claude ? "Claude" : "ChatGPT");
    char plan[QUOTA_PLAN_MAX_BYTES + 1];
    quota_copy_display_ascii(account->plan, plan, sizeof(plan));
    char source_and_plan[QUOTA_PLAN_MAX_BYTES + 20];
    snprintf(source_and_plan, sizeof(source_and_plan), "%s · %s",
             is_claude ? "Claude Code" : "Codex", plan[0] == '\0' ? "" : plan);
    set_label_text(s_ui.home_plan, source_and_plan);
    char email[QUOTA_EMAIL_MAX_BYTES + 1];
    quota_copy_display_ascii(account->email, email, sizeof(email));
    set_label_text(s_ui.home_email, email);

    bool stale = quota_data_is_stale(service->now_epoch, account->has_observed_at,
                                     account->observed_at, service->refresh_seconds) ||
                 account->status != QUOTA_STATUS_OK;
    render_metric(0, &account->five_hour, service->now_epoch, stale, false);
    render_metric(1, &account->seven_day, service->now_epoch, stale, true);

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
    } else if (account->has_observed_at) {
        char clock_text[16];
        format_clock(account->observed_at, false, clock_text, sizeof(clock_text));
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
    static const char *const labels[] = {"账户管理", "刷新间隔", "立即刷新", "电脑配对"};
    char values[4][24];
    snprintf(values[0], sizeof(values[0]), "%u 个", (unsigned)service->snapshot.account_count);
    if (!service->auto_refresh) snprintf(values[1], sizeof(values[1]), "手动");
    else snprintf(values[1], sizeof(values[1]), "%u 分钟", (unsigned)(service->refresh_seconds / 60));
    snprintf(values[2], sizeof(values[2]), "%s", service->refreshing ? "同步中" : "");
    snprintf(values[3], sizeof(values[3]), "%s", service->pairing_active ? "已打开" : "");
    for (size_t i = 0; i < 4; i++) {
        set_row_focus(s_ui.setting_rows[i], s_ui.setting_markers[i],
                      i == navigation->settings_focus);
        set_label_text(s_ui.setting_labels[i], labels[i]);
        set_label_text(s_ui.setting_values[i], values[i]);
    }
    set_label_text(s_ui.header_info, service->connected ? "已连接" : "离线");
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
            quota_copy_display_ascii(account->plan, plan, sizeof(plan));
            snprintf(title, sizeof(title), "%s · %s", claude ? "Claude" : "ChatGPT", plan);
            char email[QUOTA_EMAIL_MAX_BYTES + 1];
            quota_copy_display_ascii(account->email, email, sizeof(email));
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

    char battery[8];
    battery_text(battery_percent, battery);
    set_label_text(s_ui.battery, battery);

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
        case QUOTA_SCREEN_SETUP:
            render_setup(service);
            break;
        default:
            break;
    }
}
