#include "quota_ui.h"

#include "bsp_pins.h"
#include "lvgl.h"
#include "quota_brand_assets.h"
#include "quota_portable.h"
#include "quota_wifi_icons.h"

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
/* Status bar battery: outline and nub, and the fill by state. */
#define UI_BATT_LINE 0x7C8A96
#define UI_BATT_NORMAL UI_INK
#define UI_BATT_LOW 0xFF3B30
#define UI_BATT_USB 0x34C759

#define MARGIN 12
#define CONTENT_W 216
#define LIST_ROWS 6
#define LIST_TOP 52
#define LIST_ROW_H 36
#define QR_SIZE 150
#define QR_X 45
#define QR_Y 50
/* Status bar battery: a 22 x 11 body with a 1 px outline, a 2 x 5 nub, and a fill inset by 1 px. */
#define BATT_X 204
#define BATT_Y 8
#define BATT_BODY_W 22
#define BATT_BODY_H 11
#define BATT_FILL_MAX 18
/* Status bar Wi-Fi glyph (17 x 13), left of the battery with 8 px between. */
#define WIFI_X 179
#define WIFI_Y 7
/* The account title: from the logo to the "1/3" counter. */
#define TITLE_W 140
#define STATUS_Y 276
#define FOOTER_Y 300
#define FOOTER_LINE_Y 294
#define QR_DATA_BYTES 192

/* Every screen draws with the same objects. Nothing is created or deleted after quota_ui_init():
 * a screen change moves, restyles, retexts and hides or shows what is already there. The label
 * slots are shared; the first group has a fixed job on every screen. */
typedef enum {
    SLOT_CLOCK = 0,
    SLOT_STATUS,
    SLOT_FOOTER,
    SLOT_TEXT, /* general text, laid out by each screen */
    SLOT_TEXT_COUNT = 14,
    SLOT_COUNT = SLOT_TEXT + SLOT_TEXT_COUNT,
} slot_t;

/* General text slots by screen. Names only say what the home card uses them for. */
enum {
    T_TITLE = 0,
    T_RIGHT,
    T_SUB,
    T_NAME0,
    T_REMAIN0,
    T_VALUE0,
    T_RESET0,
    T_NAME1,
    T_REMAIN1,
    T_VALUE1,
    T_RESET1,
    T_EXTRA,
    T_EXTRA2, /* the second line of the credits and resets text, when one line is too short */
};
/* A list screen has a title and counter, then a label and a value for each row. */
#define T_ROW_LABEL(row) (2 + (int)(row))
#define T_ROW_VALUE(row) (8 + (int)(row))

typedef struct {
    lv_obj_t *obj;
    int16_t x, y, w, h;
    const lv_font_t *font;
    uint32_t color;
    lv_text_align_t align;
    uint32_t text_hash;   /* hash and length of the text last set: a label that shortens its own */
    uint32_t text_length; /* text with dots no longer holds it, so it cannot be compared */
    bool used;
} label_slot_t;

typedef struct {
    label_slot_t slot[SLOT_COUNT];
    lv_obj_t *batt_body, *batt_nub, *batt_fill, *batt_slash;
    int batt_fill_width;
    quota_wifi_icon_t wifi_icon; /* last drawn: the signal level changes with hysteresis */
    lv_obj_t *wifi_dim, *wifi_lit, *wifi_slash;
    lv_obj_t *logo;
    lv_obj_t *bar[2];
    lv_obj_t *footer_line;
    lv_obj_t *row_bg[LIST_ROWS];
    lv_obj_t *qr;
    char qr_data[QR_DATA_BYTES];
    bool qr_valid;
    /* What the frame being drawn wants on screen; applied once it is complete. */
    bool want_logo, want_qr, want_bar[2], want_row[LIST_ROWS], want_marker[LIST_ROWS];
} quota_ui_objects_t;

static lv_obj_t *s_root;
static quota_ui_objects_t s_ui;
static lv_style_t s_text_style;
static const lv_image_dsc_t *s_logo_src;
static bool s_sleep_notice;

static const char *portable_error_text(const char *code);
static const char *portable_storage_title(const char *code);

static lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
}

/* A hidden flag or a style is written only when it changes, so an unchanged frame repaints
 * nothing. */
static void show(lv_obj_t *object, bool visible)
{
    if (object == NULL || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) != visible)
        return;
    if (visible)
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static uint32_t text_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    for (; *text != '\0'; text++)
        hash = (hash ^ (uint8_t)*text) * 16777619u;
    return hash;
}

static void create_slot(lv_obj_t *parent, slot_t index)
{
    label_slot_t *slot = &s_ui.slot[index];
    slot->obj = lv_label_create(parent);
    lv_obj_remove_style_all(slot->obj);
    lv_obj_add_style(slot->obj, &s_text_style, 0);
    lv_label_set_long_mode(slot->obj, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text_static(slot->obj, "");
    slot->font = NULL;
    slot->color = UINT32_MAX;
    slot->align = (lv_text_align_t)-1;
    slot->x = slot->y = slot->w = slot->h = -1;
    slot->text_hash = text_hash("");
    slot->text_length = 0;
    lv_obj_add_flag(slot->obj, LV_OBJ_FLAG_HIDDEN);
}

static void slot_set_text(label_slot_t *slot, const char *text)
{
    /* The empty string is static; anything else is the label's own copy. */
    uint32_t hash = text_hash(text), length = (uint32_t)strlen(text);
    if (hash != slot->text_hash || length != slot->text_length) {
        lv_label_set_text(slot->obj, text);
        slot->text_hash = hash;
        slot->text_length = length;
    }
}

/* Place, style and fill one label, and mark it in use for this frame. */
static void put(slot_t index, int x, int y, int width, int height, const lv_font_t *font,
                uint32_t text_color, lv_text_align_t align, const char *text)
{
    label_slot_t *slot = &s_ui.slot[index];
    if (slot->x != x || slot->y != y) {
        lv_obj_set_pos(slot->obj, x, y);
        slot->x = (int16_t)x;
        slot->y = (int16_t)y;
    }
    if (slot->w != width || slot->h != height) {
        lv_obj_set_size(slot->obj, width, height);
        slot->w = (int16_t)width;
        slot->h = (int16_t)height;
    }
    if (slot->font != font) {
        lv_obj_set_style_text_font(slot->obj, font, 0);
        slot->font = font;
    }
    if (slot->color != text_color) {
        lv_obj_set_style_text_color(slot->obj, color(text_color), 0);
        slot->color = text_color;
    }
    if (slot->align != align) {
        lv_obj_set_style_text_align(slot->obj, align, 0);
        slot->align = align;
    }
    slot_set_text(slot, text);
    show(slot->obj, true);
    slot->used = true;
}

static void put_text(int index, int x, int y, int width, int height, const lv_font_t *font,
                     uint32_t text_color, lv_text_align_t align, const char *text)
{
    put((slot_t)(SLOT_TEXT + index), x, y, width, height, font, text_color, align, text);
}

/* Hide the labels a frame did not use, then arm the next frame. */
static void finish_slots(void)
{
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        label_slot_t *slot = &s_ui.slot[i];
        if (!slot->used)
            show(slot->obj, false);
        slot->used = false;
    }
}

static lv_obj_t *create_wifi_image(const lv_image_dsc_t *source)
{
    lv_obj_t *image = lv_image_create(s_root);
    lv_image_set_src(image, source);
    lv_obj_set_pos(image, WIFI_X, WIFI_Y);
    /* The A8 images are only a shape: the style color tints them. */
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    return image;
}

static void create_wifi(void)
{
    s_ui.wifi_dim = create_wifi_image(&quota_wifi_dim[0]);
    s_ui.wifi_lit = create_wifi_image(&quota_wifi_lit[0]);
    s_ui.wifi_slash = create_wifi_image(&quota_wifi_slash);
}

static void create_battery(void)
{
    static const lv_point_precise_t slash_points[] = {{3, 9}, {18, 1}};
    s_ui.batt_body = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_ui.batt_body);
    lv_obj_set_pos(s_ui.batt_body, BATT_X, BATT_Y);
    lv_obj_set_size(s_ui.batt_body, BATT_BODY_W, BATT_BODY_H);
    lv_obj_set_style_bg_opa(s_ui.batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.batt_body, 1, 0);
    lv_obj_set_style_border_color(s_ui.batt_body, color(UI_BATT_LINE), 0);
    lv_obj_set_style_border_opa(s_ui.batt_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ui.batt_body, 3, 0);
    lv_obj_remove_flag(s_ui.batt_body, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.batt_fill = create_rect(s_root, BATT_X + 2, BATT_Y + 2, BATT_FILL_MAX, BATT_BODY_H - 4,
                                 UI_BATT_NORMAL, 1);
    s_ui.batt_fill_width = BATT_FILL_MAX;
    s_ui.batt_nub = create_rect(s_root, BATT_X + BATT_BODY_W, BATT_Y + 3, 2, 5, UI_BATT_LINE, 1);
    s_ui.batt_slash = lv_line_create(s_root);
    lv_obj_remove_style_all(s_ui.batt_slash);
    lv_obj_set_pos(s_ui.batt_slash, BATT_X, BATT_Y);
    lv_line_set_points(s_ui.batt_slash, slash_points, 2);
    lv_obj_set_style_line_width(s_ui.batt_slash, 1, 0);
    lv_obj_set_style_line_color(s_ui.batt_slash, color(UI_DIM), 0);
    lv_obj_add_flag(s_ui.batt_slash, LV_OBJ_FLAG_HIDDEN);
}

static void create_pool(void)
{
    lv_style_init(&s_text_style);
    lv_style_set_pad_all(&s_text_style, 0);
    lv_style_set_text_line_space(&s_text_style, 2);

    s_ui.footer_line = create_rect(s_root, MARGIN, FOOTER_LINE_Y, CONTENT_W, 1, UI_LINE, 0);
    create_battery();
    create_wifi();
    s_ui.logo = lv_image_create(s_root);
    lv_image_set_src(s_ui.logo, &quota_openai_logo);
    s_logo_src = &quota_openai_logo;
    lv_obj_set_pos(s_ui.logo, MARGIN, 30);
    lv_obj_set_size(s_ui.logo, 36, 36);
    for (size_t i = 0; i < 2; i++) {
        lv_obj_t *bar = lv_bar_create(s_root);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, MARGIN, 0);
        lv_obj_set_size(bar, CONTENT_W, 8);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, color(UI_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, color(UI_MINT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        s_ui.bar[i] = bar;
    }
    for (size_t i = 0; i < LIST_ROWS; i++) {
        int y = LIST_TOP + (int)i * LIST_ROW_H;
        s_ui.row_bg[i] = create_rect(s_root, MARGIN, y, CONTENT_W, LIST_ROW_H - 2, UI_BG, 4);
        /* The focus marker is the left border of the row; it is only switched on or off. */
        lv_obj_set_style_border_side(s_ui.row_bg[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(s_ui.row_bg[i], 3, 0);
        lv_obj_set_style_border_color(s_ui.row_bg[i], color(UI_MINT), 0);
        lv_obj_set_style_border_opa(s_ui.row_bg[i], LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_ui.row_bg[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_ui.qr = lv_qrcode_create(s_root);
    lv_obj_set_pos(s_ui.qr, QR_X, QR_Y);
    lv_qrcode_set_size(s_ui.qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_ui.qr, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_ui.qr, true);
    lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < SLOT_COUNT; i++)
        create_slot(s_root, (slot_t)i);
}

/* ---- shared pieces -------------------------------------------------------------------------- */

static bool font_has_glyph(uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph;
    return lv_font_get_glyph_dsc(&quota_font_12, &glyph, codepoint, 0);
}

/* A name or note the font cannot draw becomes "<kind> N" (N is its place in the list) instead of a
 * row of boxes. Everyday Chinese is in the font; rarer characters and emoji are not. */
static void display_name(const char *name, const char *kind, unsigned number, char *out,
                         size_t capacity)
{
    if (name[0] != '\0' && !quota_text_is_displayable(name, font_has_glyph))
        snprintf(out, capacity, "%s %u", kind, number);
    else
        snprintf(out, capacity, "%s", name);
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

static void format_countdown(uint32_t seconds, char *output, size_t capacity)
{
    snprintf(output, capacity, "剩余 %u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
}

/* One list row: label left, value right. A focused row gets the panel tone and the marker. */
static void list_row(size_t row, bool focused, bool selectable, const char *label,
                     uint32_t label_color, const char *value, const lv_font_t *value_font,
                     uint32_t value_color, int value_x)
{
    int y = LIST_TOP + (int)row * LIST_ROW_H;
    lv_color_t tone = color(focused ? UI_PANEL : UI_BG);
    if (!lv_color_eq(lv_obj_get_style_bg_color(s_ui.row_bg[row], 0), tone))
        lv_obj_set_style_bg_color(s_ui.row_bg[row], tone, 0);
    s_ui.want_row[row] = true;
    s_ui.want_marker[row] = focused && selectable;
    put_text(T_ROW_LABEL(row), 22, y + 8, value_x - 22, 22, &quota_font_16, label_color,
             LV_TEXT_ALIGN_LEFT, label);
    if (value != NULL && value[0] != '\0') {
        /* Small text sits lower in the row so its baseline matches the 16 px label. */
        int offset = value_font == &quota_font_16 ? 8 : 11;
        put_text(T_ROW_VALUE(row), value_x, y + offset, 220 - value_x, 22, value_font, value_color,
                 LV_TEXT_ALIGN_RIGHT, value);
    }
}

static void set_footer(const char *text)
{
    /* The refused sleep key is explained on whichever screen it was pressed. */
    put(SLOT_FOOTER, MARGIN, FOOTER_Y, CONTENT_W, 18, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
        s_sleep_notice ? "设置中不能息屏" : text);
}

static void set_status(const char *text, uint32_t text_color)
{
    put(SLOT_STATUS, MARGIN, STATUS_Y, CONTENT_W, 18, &quota_font_12, text_color,
        LV_TEXT_ALIGN_LEFT, text);
}

static void set_logo(const lv_image_dsc_t *image)
{
    if (s_logo_src != image) {
        lv_image_set_src(s_ui.logo, image);
        s_logo_src = image;
    }
    s_ui.want_logo = true;
}

static void render_qr_data(const char *data)
{
    if (data == NULL || data[0] == '\0')
        return;
    if (!s_ui.qr_valid || strcmp(s_ui.qr_data, data) != 0) {
        s_ui.qr_valid = false;
        if (strlen(data) >= sizeof(s_ui.qr_data) ||
            lv_qrcode_update(s_ui.qr, data, (uint32_t)strlen(data)) != LV_RESULT_OK) {
            return;
        }
        snprintf(s_ui.qr_data, sizeof(s_ui.qr_data), "%s", data);
        s_ui.qr_valid = true;
    }
    s_ui.want_qr = true;
}

static void set_bar(size_t index, int y, int value, uint32_t tone)
{
    lv_obj_t *bar = s_ui.bar[index];
    if (lv_obj_get_y(bar) != y)
        lv_obj_set_y(bar, y);
    if (lv_bar_get_value(bar) != value)
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
    if (!lv_color_eq(lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR), color(tone)))
        lv_obj_set_style_bg_color(bar, color(tone), LV_PART_INDICATOR);
    s_ui.want_bar[index] = true;
}

static bool login_active(const quota_portable_view_t *portable)
{
    return portable->login_state >= QUOTA_PORTABLE_LOGIN_CONNECTING &&
           portable->login_state <= QUOTA_PORTABLE_LOGIN_EXCHANGING;
}

static void set_bg_color(lv_obj_t *object, uint32_t hex)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(object, 0), color(hex)))
        lv_obj_set_style_bg_color(object, color(hex), 0);
}

static void set_image(lv_obj_t *image, const lv_image_dsc_t *source, uint32_t tint)
{
    if (lv_image_get_src(image) != source)
        lv_image_set_src(image, source);
    if (!lv_color_eq(lv_obj_get_style_image_recolor(image, 0), color(tint)))
        lv_obj_set_style_image_recolor(image, color(tint), 0);
    show(image, true);
}

/* A reading outside 0 to 100 (no gauge) draws the outline with a slash instead of a fill. */
static void render_battery(int battery_percent, bool usb_powered)
{
    quota_battery_icon_t icon = quota_battery_icon(battery_percent, usb_powered, BATT_FILL_MAX);
    uint32_t tone = icon.tone == QUOTA_BATTERY_USB   ? UI_BATT_USB
                    : icon.tone == QUOTA_BATTERY_LOW ? UI_BATT_LOW
                                                     : UI_BATT_NORMAL;
    bool available = icon.tone != QUOTA_BATTERY_UNAVAILABLE;
    set_bg_color(s_ui.batt_fill, tone);
    if (icon.fill > 0 && s_ui.batt_fill_width != icon.fill) {
        lv_obj_set_width(s_ui.batt_fill, icon.fill);
        s_ui.batt_fill_width = icon.fill;
    }
    show(s_ui.batt_fill, icon.fill > 0);
    show(s_ui.batt_slash, !available);
}

static void render_wifi(const quota_service_view_t *service)
{
    const quota_portable_view_t *portable = &service->portable;
    quota_wifi_icon_t icon =
        quota_wifi_icon(portable->saved_network_count > 0, service->connected,
                        portable->network_state == QUOTA_PORTABLE_NETWORK_ERROR, service->wifi_rssi,
                        s_ui.wifi_icon);
    s_ui.wifi_icon = icon;
    bool signal = icon >= QUOTA_WIFI_ICON_SIGNAL_1;
    int lit = signal ? icon - QUOTA_WIFI_ICON_SIGNAL_1 : -1;
    if (icon == QUOTA_WIFI_ICON_HIDDEN || (signal && lit == 2)) {
        show(s_ui.wifi_dim, false);
    } else {
        /* What is not lit stays visible, dim: all of it while offline. */
        set_image(s_ui.wifi_dim, &quota_wifi_dim[signal ? lit + 1 : 0], UI_DIM);
    }
    if (signal)
        set_image(s_ui.wifi_lit, &quota_wifi_lit[lit], UI_INK);
    else
        show(s_ui.wifi_lit, false);
    if (icon == QUOTA_WIFI_ICON_FAILED)
        set_image(s_ui.wifi_slash, &quota_wifi_slash, UI_AMBER);
    else
        show(s_ui.wifi_slash, false);
}

static void render_status_bar(const quota_service_view_t *service, int battery_percent,
                              bool usb_powered)
{
    char text[16];
    format_clock(service->clock_synchronized ? service->now_epoch : 0, text, sizeof(text));
    put(SLOT_CLOCK, MARGIN, 6, 60, 16, &lv_font_montserrat_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, text);
    render_battery(battery_percent, usb_powered);
    render_wifi(service);
}

static void title(const char *left, const char *right)
{
    bool has_right = right != NULL && right[0] != '\0';
    put_text(T_TITLE, MARGIN, 30, has_right ? 150 : CONTENT_W, 22, &quota_font_16, UI_INK,
             LV_TEXT_ALIGN_LEFT, left);
    if (has_right) {
        put_text(T_RIGHT, 150, 34, 78, 16, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, right);
    }
}

/* ---- home ----------------------------------------------------------------------------------- */

static void render_metric(size_t block, const char *name, const quota_window_t *window,
                          uint64_t now, bool stale, bool clock_synchronized)
{
    int y = block == 0 ? 80 : 150;
    int base = block == 0 ? T_NAME0 : T_NAME1;
    put_text(base, MARGIN, y, 120, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_LEFT, name);
    char reset[48];
    quota_format_reset_time(window, now, clock_synchronized, reset, sizeof(reset));
    put_text(base + 3, MARGIN, y + 42, CONTENT_W, 16, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
             reset);
    quota_metric_state_t state = quota_metric_state(window, now);
    if (state != QUOTA_METRIC_VALUE) {
        put_text(base + 2, 120, y + 2, 108, 18, &quota_font_12, UI_DIM, LV_TEXT_ALIGN_RIGHT,
                 state == QUOTA_METRIC_UNAVAILABLE ? "未提供" : "等待新数据");
        return;
    }
    char value[8];
    snprintf(value, sizeof(value), "%u%%", (unsigned)window->remaining_percent);
    uint32_t tone = stale                             ? UI_DIM
                    : window->remaining_percent <= 5  ? UI_RED
                    : window->remaining_percent <= 20 ? UI_AMBER
                                                      : UI_MINT;
    put_text(base + 1, 120, y, 48, 22, &quota_font_16, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "剩余");
    put_text(base + 2, 168, y - 2, 60, 26, &lv_font_montserrat_20, tone, LV_TEXT_ALIGN_RIGHT,
             value);
    set_bar(block, y + 28, window->remaining_percent, stale ? UI_DIM : tone);
}

static void build_status_line(const quota_service_view_t *service, size_t selected,
                              const quota_account_t *account, bool stale, char *text,
                              size_t capacity, uint32_t *tone, bool *authorizing)
{
    const quota_portable_view_t *portable = &service->portable;
    const char *error = portable->account_errors[selected];
    quota_status_input_t input = {
        .storage_error = portable->storage_error[0] != '\0',
        .authorizing = login_active(portable),
        .reauth_needed = account->status == QUOTA_STATUS_EXPIRED ||
                         strcmp(error, "codex_expired") == 0 || strcmp(error, "auth_required") == 0,
        .unverified_items = (uint8_t)(portable->pending_items + portable->failed_items),
        .wifi_failed = portable->network_state == QUOTA_PORTABLE_NETWORK_ERROR,
        .rate_limited = strcmp(error, "rate_limited") == 0,
        .update_failed = error[0] != '\0' || account->status == QUOTA_STATUS_ERROR ||
                         account->status == QUOTA_STATUS_UNSUPPORTED,
        .refreshing = service->refreshing,
        .has_observed_at = account->has_observed_at,
    };
    char clock_text[16] = "";
    if (account->has_observed_at)
        format_clock(account->observed_at, clock_text, sizeof(clock_text));
    quota_status_line_t kind = quota_status_line_select(&input);
    *authorizing = kind == QUOTA_STATUS_LINE_AUTHORIZING;
    *tone = stale ? UI_AMBER : UI_MUTED;
    switch (kind) {
    case QUOTA_STATUS_LINE_STORAGE_ERROR:
        snprintf(text, capacity, "%s", portable_error_text(portable->storage_error));
        *tone = UI_AMBER;
        break;
    case QUOTA_STATUS_LINE_AUTHORIZING:
        snprintf(text, capacity, "授权中 · OK 查看");
        break;
    case QUOTA_STATUS_LINE_REAUTH:
        snprintf(text, capacity, "需要重新授权 · 请打开设置页");
        *tone = UI_AMBER;
        break;
    case QUOTA_STATUS_LINE_UNVERIFIED:
        if (portable->failed_items > 0)
            snprintf(text, capacity, "验证失败 · 请打开设置页修改");
        else
            snprintf(text, capacity, "待验证 · 在设置页点完成设置");
        *tone = UI_AMBER;
        break;
    case QUOTA_STATUS_LINE_WIFI_FAILED:
        if (account->has_observed_at)
            snprintf(text, capacity, "Wi-Fi 连接失败 · 显示 %s 的数据", clock_text);
        else
            snprintf(text, capacity, "Wi-Fi 连接失败");
        *tone = UI_AMBER;
        break;
    case QUOTA_STATUS_LINE_RATE_LIMITED: {
        uint64_t retry = portable->account_retry_at[selected];
        if (service->clock_synchronized && retry > service->now_epoch) {
            snprintf(text, capacity, "请求过多 · %llu 秒后重试",
                     (unsigned long long)(retry - service->now_epoch));
        } else {
            snprintf(text, capacity, "请求过多 · 稍后重试");
        }
        *tone = UI_AMBER;
        break;
    }
    case QUOTA_STATUS_LINE_UPDATE_FAILED:
        if (account->has_observed_at)
            snprintf(text, capacity, "更新失败 · 显示 %s 的数据", clock_text);
        else
            snprintf(text, capacity, "更新失败");
        *tone = UI_AMBER;
        break;
    case QUOTA_STATUS_LINE_REFRESHING:
        snprintf(text, capacity, "正在刷新");
        break;
    case QUOTA_STATUS_LINE_UPDATED:
        snprintf(text, capacity, "更新于 %s", clock_text);
        break;
    default:
        snprintf(text, capacity, "尚无数据");
        break;
    }
}

static void render_welcome(const quota_service_view_t *service)
{
    bool storage_error = service->portable.storage_error[0] != '\0';
    put_text(T_TITLE, 0, 56, 240, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_CENTER, "AI 额度");
    if (storage_error) {
        put_text(T_SUB, MARGIN, 90, CONTENT_W, 22, &quota_font_16, UI_AMBER, LV_TEXT_ALIGN_CENTER,
                 portable_storage_title(service->portable.storage_error));
        put_text(T_NAME0, MARGIN, 124, CONTENT_W, 40, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_CENTER, portable_error_text(service->portable.storage_error));
        set_footer("OK 热点设置       长按OK 菜单");
        return;
    }
    put_text(T_SUB, MARGIN, 90, CONTENT_W, 22, &quota_font_16, UI_MUTED, LV_TEXT_ALIGN_CENTER,
             "Passport 还没有账户");
    put_text(T_NAME0, MARGIN, 128, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_LEFT,
             "手机：按 OK 开启热点设置");
    put_text(T_NAME1, MARGIN, 148, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_LEFT,
             "USB：用 USB 线连接，打开");
    put_text(T_VALUE0, 24, 166, 204, 32, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
             "zesming.github.io/\nai-passport-quota");
    put_text(T_VALUE1, MARGIN, 206, CONTENT_W, 34, &quota_font_12, UI_INK, LV_TEXT_ALIGN_LEFT,
             "点“连接 Passport”后，长按 OK，\n按下键，再按 OK 打开 USB 设置");
    set_footer("OK 热点设置       长按OK 菜单");
}

static int text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The amount in the big font, with its currency in a smaller one on the same baseline; the pair
 * is centered. A long amount drops to a smaller size. */
static void render_balance(const quota_currency_balance_t *entry, uint32_t tone)
{
    quota_money_t money;
    quota_money_parts(entry->currency, entry->total_balance, &money);
    size_t length = strlen(money.number);
    const lv_font_t *big = length > 16   ? &lv_font_montserrat_12
                           : length > 12 ? &lv_font_montserrat_14
                                         : &lv_font_montserrat_20;
    /* The symbol is a step smaller than the digits, so a long amount scales both. */
    const lv_font_t *small = big == &lv_font_montserrat_20 ? &quota_font_16 : &quota_font_12;
    const int top = 116, gap = 2;
    int prefix_w = money.prefix[0] ? text_width(money.prefix, small) + gap : 0;
    int suffix_w = money.suffix[0] ? text_width(money.suffix, small) : 0;
    int number_w = text_width(money.number, big);
    int x = MARGIN + (CONTENT_W - (prefix_w + number_w + suffix_w)) / 2;
    if (x < MARGIN)
        x = MARGIN;
    int baseline = top + big->line_height - big->base_line;
    int small_top = baseline - (small->line_height - small->base_line);
    put_text(T_VALUE0, x + prefix_w, top, number_w + 2, 28, big, tone, LV_TEXT_ALIGN_LEFT,
             money.number);
    if (money.prefix[0]) {
        put_text(T_VALUE1, x, small_top, prefix_w, small->line_height, small, tone,
                 LV_TEXT_ALIGN_LEFT, money.prefix);
    }
    if (money.suffix[0]) {
        put_text(T_RESET0, x + prefix_w + number_w, small_top, suffix_w + 2, small->line_height,
                 small, tone, LV_TEXT_ALIGN_LEFT, money.suffix);
    }
}

static void render_home(const quota_navigation_t *navigation, const quota_service_view_t *service)
{
    if (service->snapshot.account_count == 0) {
        render_welcome(service);
        return;
    }
    size_t selected = navigation->selected_account;
    if (selected >= service->snapshot.account_count)
        selected = 0;
    const quota_account_t *account = &service->snapshot.accounts[selected];
    bool deepseek = account->provider == QUOTA_PROVIDER_DEEPSEEK;
    char text[QUOTA_EMAIL_MAX_BYTES + 1], heading[QUOTA_PLAN_MAX_BYTES + 12];
    if (deepseek) {
        snprintf(heading, sizeof(heading), "DeepSeek");
        display_name(service->snapshot.balances[selected].label, "DeepSeek", (unsigned)selected + 1,
                     text, sizeof(text));
    } else {
        /* The plan joins the title so the whole email, unmasked, has the line below to itself. */
        char plan[QUOTA_PLAN_MAX_BYTES + 1];
        quota_copy_display_plan(account->plan, plan, sizeof(plan));
        snprintf(heading, sizeof(heading), "ChatGPT%s%s", plan[0] ? " " : "", plan);
        /* A plan name too long to sit after "ChatGPT" is shown alone. */
        if (plan[0] && text_width(heading, &quota_font_16) > TITLE_W)
            snprintf(heading, sizeof(heading), "%s", plan);
        quota_copy_display_ascii(account->email, text, sizeof(text));
    }
    put_text(T_TITLE, 56, 30, TITLE_W, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_LEFT, heading);
    char counter[16];
    snprintf(counter, sizeof(counter), "%u/%u", (unsigned)(selected + 1),
             (unsigned)service->snapshot.account_count);
    put_text(T_RIGHT, 196, 34, 32, 16, &lv_font_montserrat_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT,
             counter);
    set_logo(deepseek ? &quota_deepseek_logo : &quota_openai_logo);
    put_text(T_SUB, 56, 52, 172, 16, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, text);

    bool stale = quota_data_is_stale(service->now_epoch, account->has_observed_at,
                                     account->observed_at, service->refresh_seconds) ||
                 account->status != QUOTA_STATUS_OK;
    if (deepseek) {
        const quota_currency_balance_t *entry =
            quota_balance_primary(&service->snapshot.balances[selected]);
        put_text(T_NAME0, MARGIN, 84, CONTENT_W, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_LEFT,
                 "可用余额");
        if (entry != NULL)
            render_balance(entry, stale ? UI_DIM : UI_INK);
        else
            put_text(T_VALUE0, MARGIN, 116, CONTENT_W, 22, &quota_font_16, UI_DIM,
                     LV_TEXT_ALIGN_CENTER, "未提供");
    } else {
        const quota_window_t *windows[] = {&account->five_hour, &account->seven_day};
        static const char *const names[] = {"5 小时额度", "7 天额度"};
        size_t block = 0;
        for (size_t i = 0; i < 2; i++) {
            if (windows[i]->present) {
                render_metric(block++, names[i], windows[i], service->now_epoch, stale,
                              service->clock_synchronized);
            }
        }
        if (block == 0) {
            put_text(T_NAME0, MARGIN, 84, CONTENT_W, 22, &quota_font_16, UI_MUTED,
                     LV_TEXT_ALIGN_LEFT,
                     account->status == QUOTA_STATUS_OK ? "未提供额度窗口" : "等待额度数据");
        }
        const quota_codex_extras_t *extras = &service->snapshot.codex_extras[selected];
        char credits[QUOTA_CREDITS_BALANCE_BYTES + 32] = "", resets[80];
        if (extras->has_credits) {
            /* Whole credits only: the stored decimal string is untouched. */
            char amount[QUOTA_CREDITS_BALANCE_BYTES + 1];
            quota_format_credits(extras->credits_balance, amount, sizeof(amount));
            snprintf(credits, sizeof(credits), "剩余额度 %s",
                     extras->unlimited_credits ? "不限量"
                     : amount[0]               ? amount
                                               : "可用");
        }
        quota_format_banked_resets(extras, service->now_epoch, service->clock_synchronized, resets,
                                   sizeof(resets));
        /* One line when both fit, otherwise the resets note takes a second line. */
        char joined[sizeof(credits) + sizeof(resets) + 4];
        snprintf(joined, sizeof(joined), "%s%s%s", credits, credits[0] && resets[0] ? "   " : "",
                 resets);
        uint32_t extra_tone = stale ? UI_DIM : UI_INK;
        if (credits[0] && resets[0] && text_width(joined, &quota_font_12) > CONTENT_W) {
            put_text(T_EXTRA, MARGIN, 214, CONTENT_W, 16, &quota_font_12, extra_tone,
                     LV_TEXT_ALIGN_LEFT, credits);
            put_text(T_EXTRA2, MARGIN, 232, CONTENT_W, 16, &quota_font_12, extra_tone,
                     LV_TEXT_ALIGN_LEFT, resets);
        } else if (joined[0] != '\0') {
            put_text(T_EXTRA, MARGIN, 222, CONTENT_W, 16, &quota_font_12, extra_tone,
                     LV_TEXT_ALIGN_LEFT, joined);
        }
    }
    char status[96];
    uint32_t tone;
    bool authorizing;
    build_status_line(service, selected, account, stale, status, sizeof(status), &tone,
                      &authorizing);
    set_status(status, tone);
    if (authorizing)
        set_footer("上下 切换  OK 查看  长按OK 菜单");
    else
        set_footer("上下 切换  OK 刷新  长按OK 菜单");
}

/* ---- menu and option lists ------------------------------------------------------------------ */

static void format_refresh(const quota_service_view_t *service, char *output, size_t capacity)
{
    if (!service->auto_refresh)
        snprintf(output, capacity, "手动");
    else
        snprintf(output, capacity, "%u 分钟", (unsigned)(service->refresh_seconds / 60));
}

static void format_timeout(uint16_t seconds, char *output, size_t capacity)
{
    if (seconds == 0)
        snprintf(output, capacity, "从不");
    else if (seconds < 60)
        snprintf(output, capacity, "%u 秒", (unsigned)seconds);
    else
        snprintf(output, capacity, "%u 分钟", (unsigned)(seconds / 60));
}

static void render_menu(const quota_navigation_t *navigation, const quota_service_view_t *service)
{
    static const char *const labels[QUOTA_MENU_ITEMS] = {"热点设置", "USB 设置", "刷新频率",
                                                         "自动息屏", "设备信息"};
    char values[QUOTA_MENU_ITEMS][16] = {"", "", "", "", ""};
    if (service->portable.setup_active)
        snprintf(values[0], sizeof(values[0]), "已打开");
    if (service->usb_window_active || service->usb_window_preparing)
        snprintf(values[1], sizeof(values[1]), "已打开");
    format_refresh(service, values[2], sizeof(values[2]));
    format_timeout(service->screen_timeout_seconds, values[3], sizeof(values[3]));
    title("菜单", "");
    for (size_t i = 0; i < QUOTA_MENU_ITEMS; i++) {
        list_row(i, i == navigation->menu_focus, true, labels[i], UI_INK, values[i], &quota_font_12,
                 UI_MUTED, 120);
    }
    set_footer("上下 选择  OK 进入  长按OK 返回");
}

static void render_options(const quota_navigation_t *navigation,
                           const quota_service_view_t *service)
{
    bool refresh = navigation->screen == QUOTA_SCREEN_REFRESH;
    static const char *const refresh_labels[QUOTA_REFRESH_OPTIONS] = {"手动", "1 分钟", "5 分钟",
                                                                      "15 分钟", "30 分钟"};
    static const uint16_t refresh_seconds[QUOTA_REFRESH_OPTIONS] = {0, 60, 300, 900, 1800};
    static const char *const sleep_labels[QUOTA_SCREEN_TIMEOUT_COUNT] = {
        "从不", "30 秒", "1 分钟", "2 分钟", "5 分钟", "10 分钟"};
    size_t count = refresh ? QUOTA_REFRESH_OPTIONS : QUOTA_SCREEN_TIMEOUT_COUNT;
    title(refresh ? "刷新频率" : "自动息屏", "");
    for (size_t i = 0; i < count; i++) {
        bool current = refresh ? (i == 0 ? !service->auto_refresh
                                         : service->auto_refresh &&
                                               refresh_seconds[i] == service->refresh_seconds)
                               : quota_screen_timeouts[i] == service->screen_timeout_seconds;
        list_row(i, i == navigation->option_focus, true,
                 refresh ? refresh_labels[i] : sleep_labels[i], UI_INK, current ? "●" : "",
                 &quota_font_16, UI_MINT, 170);
    }
    set_status(refresh ? "所有账户使用同一频率" : "长按下键可随时息屏", UI_MUTED);
    set_footer("上下 选择  OK 保存  长按OK 返回");
}

/* ---- device information and confirmation ---------------------------------------------------- */

static void render_info(const quota_service_view_t *service, int battery_percent)
{
    const quota_portable_view_t *portable = &service->portable;
    char wifi[64], address[24], level[12], ssid[QUOTA_SSID_MAX_BYTES + 1];
    display_name(portable->network_ssid, "Wi-Fi", (unsigned)portable->selected_saved_network + 1,
                 ssid, sizeof(ssid));
    switch (portable->network_state) {
    case QUOTA_PORTABLE_NETWORK_READY:
    case QUOTA_PORTABLE_NETWORK_CONNECTED:
    case QUOTA_PORTABLE_NETWORK_TIME_REQUIRED:
        snprintf(wifi, sizeof(wifi), "已连接 %.40s", ssid);
        break;
    case QUOTA_PORTABLE_NETWORK_CONNECTING:
        snprintf(wifi, sizeof(wifi), "连接中");
        break;
    case QUOTA_PORTABLE_NETWORK_ERROR:
        snprintf(wifi, sizeof(wifi), "连接失败");
        break;
    default:
        snprintf(wifi, sizeof(wifi), portable->saved_network_count ? "未连接" : "未设置");
        break;
    }
    snprintf(address, sizeof(address), "%s", service->connected ? portable->network_ip : "--");
    if (battery_percent >= 0 && battery_percent <= 100)
        snprintf(level, sizeof(level), "%d%%", battery_percent);
    else
        snprintf(level, sizeof(level), "--");
    char firmware[QUOTA_FIRMWARE_VERSION_BYTES + 1];
    quota_copy_display_ascii(portable->firmware, firmware, sizeof(firmware));
    const char *values[] = {wifi, address, service->clock_synchronized ? "已校时" : "未校时", level,
                            firmware[0] ? firmware : "--"};
    static const char *const labels[] = {"Wi-Fi", "IP", "时间", "电量", "固件"};
    title("设备信息", "");
    for (size_t i = 0; i < 5; i++)
        list_row(i, false, false, labels[i], UI_INK, values[i], &quota_font_12, UI_MUTED, 76);
    set_footer("OK 恢复出厂设置   长按OK 返回");
}

static void render_confirm(const quota_navigation_t *navigation,
                           const quota_service_view_t *service)
{
    (void)service;
    bool reset = navigation->confirm_kind == QUOTA_CONFIRM_FACTORY_RESET;
    if (navigation->factory_resetting) { /* every key is ignored until the device restarts */
        title("正在恢复出厂设置", "");
        put_text(T_SUB, MARGIN, 62, CONTENT_W, 40, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "正在擦除，请勿断电。\n完成后 Passport 会重启。");
        set_footer("擦除中，按键无效");
        return;
    }
    if (navigation->factory_failed) {
        title("恢复出厂设置失败", "");
        put_text(T_SUB, MARGIN, 62, CONTENT_W, 40, &quota_font_12, UI_AMBER, LV_TEXT_ALIGN_LEFT,
                 "擦除失败，账户和设置保持不变。\n请重试，或检查设备存储。");
        set_footer("长按OK 返回");
        return;
    }
    title(reset ? "恢复出厂设置？" : "取消 ChatGPT 授权？", "");
    put_text(T_SUB, MARGIN, 62, CONTENT_W, 56, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT,
             reset ? "将删除全部账户、Wi-Fi 和设置，\nPassport 会重启。"
                   : "授权会停止，已扫码的授权页\n也会失效。");
    list_row(3, navigation->confirm_focus == 0, true, reset ? "取消" : "不取消", UI_INK, "",
             &quota_font_12, UI_MUTED, 120);
    list_row(4, navigation->confirm_focus == 1, true, reset ? "确认删除" : "取消授权", UI_RED, "",
             &quota_font_12, UI_MUTED, 120);
    set_footer("上下 选择  OK 确定  长按OK 返回");
}

/* ---- hotspot and USB ------------------------------------------------------------------------ */

typedef struct {
    char label[48];
    quota_validation_t state;
} result_row_t;

static void add_result(result_row_t rows[LIST_ROWS], size_t *shown, size_t *total,
                       const char *label, quota_validation_t state)
{
    (*total)++;
    /* Failed first, then waiting, then the rest, so what needs work is always on screen. */
    static const quota_validation_t order[] = {QUOTA_VALIDATION_FAILED, QUOTA_VALIDATION_PENDING,
                                               QUOTA_VALIDATION_OK, QUOTA_VALIDATION_SAVED};
    size_t rank = 0;
    for (; rank < 4 && order[rank] != state; rank++) {
    }
    size_t at = *shown;
    for (size_t i = 0; i < *shown; i++) {
        size_t other = 0;
        for (; other < 4 && order[other] != rows[i].state; other++) {
        }
        if (rank < other) {
            at = i;
            break;
        }
    }
    if (at >= LIST_ROWS)
        return;
    size_t last = *shown < LIST_ROWS ? *shown : LIST_ROWS - 1;
    for (size_t i = last; i > at; i--)
        rows[i] = rows[i - 1];
    snprintf(rows[at].label, sizeof(rows[at].label), "%s", label);
    rows[at].state = state;
    if (*shown < LIST_ROWS)
        (*shown)++;
}

static const char *validation_text(quota_validation_t state, bool validating)
{
    switch (state) {
    case QUOTA_VALIDATION_OK:
        return "正常";
    case QUOTA_VALIDATION_FAILED:
        return "验证失败";
    case QUOTA_VALIDATION_PENDING:
        return validating ? "验证中" : "待验证";
    default:
        return "已保存";
    }
}

/* What the hotspot found when it was closed with 完成设置: every Wi-Fi and account, worst first. */
/* What the hotspot found when it was closed with 完成设置: every Wi-Fi and account, worst first. */
static void render_results(const quota_service_view_t *service)
{
    const quota_portable_view_t *portable = &service->portable;
    result_row_t rows[LIST_ROWS];
    size_t shown = 0, total = 0;
    for (size_t i = 0; i < portable->saved_network_count && i < QUOTA_PORTABLE_NETWORKS; i++) {
        char ssid[QUOTA_SSID_MAX_BYTES + 1];
        display_name(portable->saved_network_ssids[i], "Wi-Fi", (unsigned)i + 1, ssid,
                     sizeof(ssid));
        add_result(rows, &shown, &total, ssid,
                   (quota_validation_t)portable->saved_network_validation[i]);
    }
    for (size_t i = 0; i < service->snapshot.account_count; i++) {
        const quota_account_t *account = &service->snapshot.accounts[i];
        char name[48], detail[QUOTA_EMAIL_MAX_BYTES + 1];
        if (account->provider == QUOTA_PROVIDER_DEEPSEEK) {
            display_name(service->snapshot.balances[i].label, "DeepSeek", (unsigned)i + 1, detail,
                         sizeof(detail));
            if (strncmp(detail, "DeepSeek ", 9) == 0)
                snprintf(name, sizeof(name), "%.47s", detail);
            else
                snprintf(name, sizeof(name), "DeepSeek %.36s", detail);
        } else {
            quota_copy_display_ascii(account->email, detail, sizeof(detail));
            snprintf(name, sizeof(name), "ChatGPT %.36s", detail);
        }
        add_result(rows, &shown, &total, name, (quota_validation_t)portable->account_validation[i]);
    }
    char summary[40];
    if (portable->validating)
        snprintf(summary, sizeof(summary), "验证中");
    else if (portable->failed_items > 0)
        snprintf(summary, sizeof(summary), "%u 项失败", (unsigned)portable->failed_items);
    else if (portable->pending_items > 0)
        snprintf(summary, sizeof(summary), "%u 项待验证", (unsigned)portable->pending_items);
    else
        snprintf(summary, sizeof(summary), "全部正常");
    title("验证结果", summary);
    for (size_t i = 0; i < shown; i++) {
        quota_validation_t state = rows[i].state;
        int y = LIST_TOP + (int)i * LIST_ROW_H;
        s_ui.want_row[i] = true;
        if (!lv_color_eq(lv_obj_get_style_bg_color(s_ui.row_bg[i], 0), color(UI_BG)))
            lv_obj_set_style_bg_color(s_ui.row_bg[i], color(UI_BG), 0);
        put_text(T_ROW_LABEL(i), 22, y + 10, 146, 18, &quota_font_12, UI_INK, LV_TEXT_ALIGN_LEFT,
                 rows[i].label);
        put_text(T_ROW_VALUE(i), 168, y + 10, 52, 18, &quota_font_12,
                 state == QUOTA_VALIDATION_FAILED ? UI_RED
                 : state == QUOTA_VALIDATION_OK   ? UI_MINT
                                                  : UI_MUTED,
                 LV_TEXT_ALIGN_RIGHT, validation_text(state, portable->validating));
    }
    if (total > shown) {
        char more[24];
        snprintf(more, sizeof(more), "另有 %u 项", (unsigned)(total - shown));
        set_status(more, UI_MUTED);
    }
}
static void render_hotspot(const quota_navigation_t *navigation,
                           const quota_service_view_t *service)
{
    const quota_portable_view_t *portable = &service->portable;
    char head[24], right[16] = "";
    unsigned page = navigation->hotspot_page % QUOTA_HOTSPOT_PAGES;
    quota_hotspot_state_t state =
        quota_hotspot_state(portable->storage_error[0] != '\0', portable->validating,
                            portable->setup_active, portable->setup_ready, portable->setup_opening);
    const char *ok = "OK 下一页";
    if (portable->setup_active)
        format_countdown(portable->setup_seconds_left, right, sizeof(right));
    /* OK only does something where the footer says so: it turns pages, or opens a closed hotspot.
     * While the hotspot opens, validates or reports an error it does nothing, so it can never
     * restart a validation that is still using the Wi-Fi credentials. */
    if (portable->storage_error[0]) {
        title(portable_storage_title(portable->storage_error), "");
        put_text(T_SUB, MARGIN, 62, CONTENT_W, 40, &quota_font_12, UI_AMBER, LV_TEXT_ALIGN_LEFT,
                 portable_error_text(portable->storage_error));
        set_footer("长按OK 返回");
        return;
    }
    if (portable->validating || (state == QUOTA_HOTSPOT_CLOSED && portable->setup_result)) {
        render_results(service);
        set_footer(portable->validating ? "OK 请稍候  长按OK 返回" : "OK 重新开启  长按OK 返回");
        return;
    }
    if (state == QUOTA_HOTSPOT_CLOSED) {
        title("热点设置", right);
        put_text(T_SUB, MARGIN, 62, CONTENT_W, 22, &quota_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "热点已关闭");
        set_footer("OK 重新开启  长按OK 返回");
        return;
    }
    if (state == QUOTA_HOTSPOT_BUSY) {
        title("热点设置", right);
        put_text(T_SUB, MARGIN, 62, CONTENT_W, 22, &quota_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT,
                 "正在打开热点");
        set_footer("OK 请稍候  长按OK 返回");
        return;
    }
    snprintf(head, sizeof(head), "热点设置 %u/3", page + 1);
    title(head, right);
    char text[160];
    if (page == 0) {
        snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", portable->setup_ssid,
                 portable->setup_password);
        render_qr_data(text);
        snprintf(text, sizeof(text), "名称 %s", portable->setup_ssid);
        put_text(T_SUB, MARGIN, 206, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER,
                 text);
        snprintf(text, sizeof(text), "密码 %s", portable->setup_password);
        put_text(T_NAME0, MARGIN, 226, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER,
                 text);
    } else if (page == 1) {
        render_qr_data(portable->setup_page_url);
        put_text(T_SUB, MARGIN, 206, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "连上热点后扫码");
        put_text(T_NAME0, MARGIN, 226, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "打开设置页");
    } else {
        put_text(T_SUB, MARGIN, 66, CONTENT_W, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "在浏览器输入");
        put_text(T_NAME0, MARGIN, 94, CONTENT_W, 20, &lv_font_montserrat_14, UI_INK,
                 LV_TEXT_ALIGN_CENTER, "http://192.168.4.1");
        put_text(T_NAME1, MARGIN, 140, CONTENT_W, 22, &quota_font_16, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "访问码");
        put_text(T_VALUE0, MARGIN, 168, CONTENT_W, 20, &lv_font_montserrat_14, UI_MINT,
                 LV_TEXT_ALIGN_CENTER, portable->setup_secret);
        ok = "OK 回到第 1 页";
    }
    snprintf(text, sizeof(text), "%s  长按OK 关闭", ok);
    set_footer(text);
}

static void render_usb(const quota_service_view_t *service)
{
    const quota_portable_view_t *portable = &service->portable;
    bool open = service->usb_window_active || service->usb_window_preparing;
    char right[16] = "";
    if (service->usb_window_active)
        format_countdown(service->usb_window_seconds_left, right, sizeof(right));
    title("USB 设置", right);
    if (portable->storage_error[0]) {
        put_text(T_SUB, MARGIN, 62, CONTENT_W, 40, &quota_font_12, UI_AMBER, LV_TEXT_ALIGN_LEFT,
                 portable_error_text(portable->storage_error));
        set_footer(open ? "长按OK 返回" : "OK 重新打开  长按OK 返回");
        return;
    }
    /* The page is connected first; USB setup is opened last, and again if the Passport rebooted
     * while the page was still connecting. */
    put_text(T_SUB, MARGIN, 54, CONTENT_W, 114, &quota_font_12, UI_INK, LV_TEXT_ALIGN_LEFT,
             "1 用 USB 线连接 Passport\n2 用 Chrome 或 Edge 打开\n  zesming.github.io/\n"
             "  ai-passport-quota\n3 点“连接 Passport”并选择串口\n页面连上前 Passport 若重启，\n"
             "再打开一次 USB 设置即可");
    const char *state;
    if (service->usb_window_preparing)
        state = "准备中";
    else if (!service->usb_window_active)
        state = "已结束";
    else if (portable->validating)
        state = "验证中";
    else if (portable->saving)
        state = "保存中";
    else if (service->usb_page_connected)
        state = "已连接";
    else
        state = "等待连接";
    char text[40];
    snprintf(text, sizeof(text), "状态：%s", state);
    put_text(T_NAME0, MARGIN, 196, CONTENT_W, 22, &quota_font_16, open ? UI_MINT : UI_AMBER,
             LV_TEXT_ALIGN_LEFT, text);
    set_footer(open ? "长按OK 返回" : "OK 重新打开  长按OK 返回");
}

/* ---- ChatGPT authorization ------------------------------------------------------------------ */

static void render_auth(const quota_service_view_t *service)
{
    const quota_portable_view_t *portable = &service->portable;
    char right[16] = "", text[96];
    bool active = login_active(portable);
    if (active)
        format_countdown(portable->login_seconds_left, right, sizeof(right));
    title("ChatGPT 授权", right);
    /* The phase a person sees: a line of what is happening and a line of what to do. */
    const char *state = "等待批准", *headline = NULL, *hint = "";
    switch (portable->login_state) {
    case QUOTA_PORTABLE_LOGIN_QUEUED:
    case QUOTA_PORTABLE_LOGIN_CONNECTING:
        state = "连接中";
        headline = "正在连接 ChatGPT…";
        hint = "请稍候，Passport 正在联网";
        break;
    case QUOTA_PORTABLE_LOGIN_REQUESTING_CODE:
        state = "获取验证码";
        headline = "正在获取验证码…";
        hint = "请稍候";
        break;
    case QUOTA_PORTABLE_LOGIN_EXCHANGING:
        state = portable->saving ? "保存中" : "完成授权中";
        headline = "已批准，正在保存授权…";
        hint = "请勿断电";
        break;
    case QUOTA_PORTABLE_LOGIN_SUCCESS:
        state = "成功";
        headline = "授权成功";
        hint = "正在查询额度";
        break;
    case QUOTA_PORTABLE_LOGIN_EXPIRED:
        state = "失败，授权超时";
        headline = "授权已过期";
        hint = "请在设置页重新授权";
        break;
    case QUOTA_PORTABLE_LOGIN_CANCELED:
        state = "已取消";
        headline = "授权已取消";
        break;
    case QUOTA_PORTABLE_LOGIN_ERROR:
        state = "失败";
        headline = "授权未完成";
        hint = portable_error_text(portable->login_error);
        break;
    case QUOTA_PORTABLE_LOGIN_IDLE:
        state = "未开始";
        headline = "没有进行中的授权";
        break;
    default:
        break;
    }
    bool waiting =
        portable->login_state == QUOTA_PORTABLE_LOGIN_WAITING && !portable->storage_error[0];
    if (waiting) {
        render_qr_data(portable->login_url);
        /* Official page without scheme or query, e.g. auth.openai.com/codex/device */
        const char *start = strstr(portable->login_url, "://");
        snprintf(text, sizeof(text), "%.*s", 40, start ? start + 3 : portable->login_url);
        char *query = strchr(text, '?');
        if (query)
            *query = '\0';
        put_text(T_SUB, MARGIN, 202, CONTENT_W, 16, &quota_font_12, UI_MUTED, LV_TEXT_ALIGN_CENTER,
                 text);
        put_text(T_NAME0, MARGIN, 220, CONTENT_W, 16, &quota_font_12, UI_INK, LV_TEXT_ALIGN_CENTER,
                 "输入验证码");
        put_text(T_VALUE0, MARGIN, 236, CONTENT_W, 26, &lv_font_montserrat_20, UI_MINT,
                 LV_TEXT_ALIGN_CENTER, portable->login_user_code);
    } else if (portable->storage_error[0]) {
        put_text(T_SUB, MARGIN, 80, CONTENT_W, 22, &quota_font_16, UI_AMBER, LV_TEXT_ALIGN_CENTER,
                 portable_storage_title(portable->storage_error));
        put_text(T_NAME0, MARGIN, 108, CONTENT_W, 40, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_CENTER, portable_error_text(portable->storage_error));
    } else if (headline != NULL) {
        bool bad = portable->login_state == QUOTA_PORTABLE_LOGIN_ERROR ||
                   portable->login_state == QUOTA_PORTABLE_LOGIN_EXPIRED;
        put_text(T_SUB, MARGIN, 96, CONTENT_W, 22, &quota_font_16, bad ? UI_AMBER : UI_INK,
                 LV_TEXT_ALIGN_CENTER, headline);
        put_text(T_NAME0, MARGIN, 126, CONTENT_W, 36, &quota_font_12, UI_MUTED,
                 LV_TEXT_ALIGN_CENTER, hint);
    }
    snprintf(text, sizeof(text), "状态：%s", state);
    put(SLOT_STATUS, MARGIN, waiting ? 268 : 276, CONTENT_W, 18, &quota_font_12,
        portable->login_state == QUOTA_PORTABLE_LOGIN_ERROR ||
                portable->login_state == QUOTA_PORTABLE_LOGIN_EXPIRED
            ? UI_AMBER
            : UI_MUTED,
        LV_TEXT_ALIGN_LEFT, text);
    set_footer(active ? "OK 取消授权   长按OK 回主页" : "OK 回主页   长按OK 回主页");
}

/* ---- text for errors ------------------------------------------------------------------------ */

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
        return "Wi-Fi 密码错误或信号弱";
    if (strcmp(code, "wifi_not_found") == 0)
        return "找不到 Wi-Fi · 请检查热点";
    if (strcmp(code, "network_unavailable") == 0)
        return "服务连接失败或超时";
    if (strcmp(code, "rate_limited") == 0)
        return "请求过多 · 稍后重试";
    if (strcmp(code, "codex_expired") == 0 || strcmp(code, "auth_required") == 0)
        return "需要重新授权";
    if (strcmp(code, "deepseek_invalid_key") == 0)
        return "密钥无效 · 请更换密钥";
    if (strcmp(code, "time_required") == 0)
        return "待校时";
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

/* ---- entry points --------------------------------------------------------------------------- */

void quota_ui_init(void)
{
    if (s_root != NULL)
        return; /* the pool is built once for the life of the device */
    memset(&s_ui, 0, sizeof(s_ui));
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, color(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    create_pool();
    lv_screen_load(s_root);
}

void quota_ui_render(const quota_navigation_t *navigation, const quota_service_view_t *service,
                     int battery_percent, bool usb_powered)
{
    if (navigation == NULL || service == NULL || s_root == NULL)
        return;
    s_ui.want_logo = s_ui.want_qr = false;
    memset(s_ui.want_bar, 0, sizeof(s_ui.want_bar));
    memset(s_ui.want_row, 0, sizeof(s_ui.want_row));
    memset(s_ui.want_marker, 0, sizeof(s_ui.want_marker));
    s_sleep_notice = navigation->sleep_notice;
    render_status_bar(service, battery_percent, usb_powered);
    switch (navigation->screen) {
    case QUOTA_SCREEN_MENU:
        render_menu(navigation, service);
        break;
    case QUOTA_SCREEN_REFRESH:
    case QUOTA_SCREEN_SLEEP:
        render_options(navigation, service);
        break;
    case QUOTA_SCREEN_INFO:
        render_info(service, battery_percent);
        break;
    case QUOTA_SCREEN_CONFIRM:
        render_confirm(navigation, service);
        break;
    case QUOTA_SCREEN_HOTSPOT:
        render_hotspot(navigation, service);
        break;
    case QUOTA_SCREEN_USB:
        render_usb(service);
        break;
    case QUOTA_SCREEN_AUTH:
        render_auth(service);
        break;
    default:
        render_home(navigation, service);
        break;
    }
    show(s_ui.logo, s_ui.want_logo);
    show(s_ui.qr, s_ui.want_qr);
    for (size_t i = 0; i < 2; i++)
        show(s_ui.bar[i], s_ui.want_bar[i]);
    for (size_t i = 0; i < LIST_ROWS; i++) {
        show(s_ui.row_bg[i], s_ui.want_row[i]);
        lv_opa_t opa = s_ui.want_marker[i] ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (lv_obj_get_style_border_opa(s_ui.row_bg[i], 0) != opa)
            lv_obj_set_style_border_opa(s_ui.row_bg[i], opa, 0);
    }
    finish_slots();
}
