#pragma once
/* The small slice of LVGL that main/quota_ui.c draws with, backed by plain structs so a test can
 * read back text, colour, size and flags. Implementation: lvgl_stub.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lv_obj {
    char text[256];
    uint32_t flags;
    int x, y, width, height;
    uint32_t bg, text_color, line_color;
    int bar_value;
} lv_obj_t;
typedef uint32_t lv_color_t;
typedef struct {
    int line_height;
} lv_font_t;
typedef struct {
    int32_t x, y;
} lv_point_precise_t;
typedef struct {
    uint32_t magic, cf, flags, w, h, stride;
    void *reserved_2;
} lv_image_header_t;
typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    void *reserved, *reserved_2;
} lv_image_dsc_t;
typedef enum { LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT } lv_text_align_t;
typedef enum { LV_LABEL_LONG_MODE_WRAP, LV_LABEL_LONG_MODE_DOTS } lv_label_long_mode_t;
typedef enum { LV_RESULT_INVALID, LV_RESULT_OK } lv_result_t;

#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 0x12
#define LV_OBJ_FLAG_HIDDEN 1u
#define LV_OBJ_FLAG_SCROLLABLE 2u
#define LV_OPA_COVER 255
#define LV_PART_MAIN 0
#define LV_PART_INDICATOR 0x20000
#define LV_ANIM_OFF 0
#define LV_FONT_DECLARE(font) extern const lv_font_t font

extern const lv_font_t lv_font_montserrat_12, lv_font_montserrat_14, lv_font_montserrat_20;

/* Counters a test can reset to prove an unchanged screen is not redrawn. */
extern unsigned lv_stub_text_sets, lv_stub_bg_sets;

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_line_create(lv_obj_t *parent);
lv_obj_t *lv_bar_create(lv_obj_t *parent);
lv_obj_t *lv_image_create(lv_obj_t *parent);
lv_obj_t *lv_qrcode_create(lv_obj_t *parent);
void lv_obj_delete(lv_obj_t *object);
void lv_obj_remove_style_all(lv_obj_t *object);
void lv_obj_add_flag(lv_obj_t *object, uint32_t flag);
void lv_obj_clear_flag(lv_obj_t *object, uint32_t flag);
bool lv_obj_has_flag(const lv_obj_t *object, uint32_t flag);
void lv_obj_set_pos(lv_obj_t *object, int x, int y);
void lv_obj_set_x(lv_obj_t *object, int x);
void lv_obj_set_y(lv_obj_t *object, int y);
void lv_obj_set_size(lv_obj_t *object, int width, int height);
void lv_obj_set_width(lv_obj_t *object, int width);
void lv_obj_set_height(lv_obj_t *object, int height);
int lv_obj_get_width(const lv_obj_t *object);
void lv_obj_set_style_bg_color(lv_obj_t *object, lv_color_t color, int selector);
void lv_obj_set_style_bg_opa(lv_obj_t *object, int opacity, int selector);
void lv_obj_set_style_radius(lv_obj_t *object, int radius, int selector);
void lv_obj_set_style_border_width(lv_obj_t *object, int width, int selector);
void lv_obj_set_style_border_color(lv_obj_t *object, lv_color_t color, int selector);
void lv_obj_set_style_pad_all(lv_obj_t *object, int padding, int selector);
void lv_obj_set_style_line_width(lv_obj_t *object, int width, int selector);
void lv_obj_set_style_line_color(lv_obj_t *object, lv_color_t color, int selector);
void lv_obj_set_style_line_rounded(lv_obj_t *object, bool rounded, int selector);
void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font, int selector);
void lv_obj_set_style_text_color(lv_obj_t *object, lv_color_t color, int selector);
void lv_obj_set_style_text_align(lv_obj_t *object, lv_text_align_t align, int selector);
void lv_obj_set_style_text_line_space(lv_obj_t *object, int space, int selector);
lv_color_t lv_obj_get_style_bg_color(const lv_obj_t *object, int selector);
lv_color_t lv_obj_get_style_line_color(const lv_obj_t *object, int selector);
void lv_label_set_text(lv_obj_t *label, const char *text);
void lv_label_set_long_mode(lv_obj_t *label, lv_label_long_mode_t mode);
const char *lv_label_get_text(const lv_obj_t *label);
void lv_line_set_points(lv_obj_t *line, const lv_point_precise_t *points, uint32_t count);
void lv_bar_set_range(lv_obj_t *bar, int min, int max);
void lv_bar_set_value(lv_obj_t *bar, int value, int animation);
void lv_image_set_src(lv_obj_t *image, const void *source);
void lv_image_set_scale(lv_obj_t *image, uint16_t scale);
void lv_qrcode_set_size(lv_obj_t *qr, int size);
void lv_qrcode_set_dark_color(lv_obj_t *qr, lv_color_t color);
void lv_qrcode_set_light_color(lv_obj_t *qr, lv_color_t color);
void lv_qrcode_set_quiet_zone(lv_obj_t *qr, bool quiet);
lv_result_t lv_qrcode_update(lv_obj_t *qr, const void *data, uint32_t length);
void lv_screen_load(lv_obj_t *screen);
lv_color_t lv_color_hex(uint32_t hex);
lv_color_t lv_color_black(void);
lv_color_t lv_color_white(void);
bool lv_color_eq(lv_color_t a, lv_color_t b);
