#include "lvgl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

const lv_font_t lv_font_montserrat_12 = {0}, lv_font_montserrat_14 = {0},
                lv_font_montserrat_20 = {0};
const lv_font_t quota_font_12 = {0}, quota_font_16 = {0};
unsigned lv_stub_text_sets, lv_stub_bg_sets;

static lv_obj_t pool[16384];
static size_t used;

static lv_obj_t *make(void)
{
    assert(used < sizeof(pool) / sizeof(pool[0]));
    return &pool[used++];
}
lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    (void)parent;
    return make();
}
lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
lv_obj_t *lv_line_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
lv_obj_t *lv_bar_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
lv_obj_t *lv_image_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
lv_obj_t *lv_qrcode_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
void lv_obj_delete(lv_obj_t *object)
{
    (void)object;
}
void lv_obj_remove_style_all(lv_obj_t *object)
{
    (void)object;
}
void lv_obj_add_flag(lv_obj_t *object, uint32_t flag)
{
    object->flags |= flag;
}
void lv_obj_clear_flag(lv_obj_t *object, uint32_t flag)
{
    object->flags &= ~flag;
}
bool lv_obj_has_flag(const lv_obj_t *object, uint32_t flag)
{
    return (object->flags & flag) != 0;
}
void lv_obj_set_pos(lv_obj_t *object, int x, int y)
{
    object->x = x;
    object->y = y;
}
void lv_obj_set_x(lv_obj_t *object, int x)
{
    object->x = x;
}
void lv_obj_set_y(lv_obj_t *object, int y)
{
    object->y = y;
}
void lv_obj_set_size(lv_obj_t *object, int width, int height)
{
    object->width = width;
    object->height = height;
}
void lv_obj_set_width(lv_obj_t *object, int width)
{
    object->width = width;
}
void lv_obj_set_height(lv_obj_t *object, int height)
{
    object->height = height;
}
int lv_obj_get_width(const lv_obj_t *object)
{
    return object->width;
}
void lv_obj_set_style_bg_color(lv_obj_t *object, lv_color_t color, int selector)
{
    (void)selector;
    object->bg = color;
    lv_stub_bg_sets++;
}
void lv_obj_set_style_bg_opa(lv_obj_t *object, int opacity, int selector)
{
    (void)object;
    (void)opacity;
    (void)selector;
}
void lv_obj_set_style_radius(lv_obj_t *object, int radius, int selector)
{
    (void)object;
    (void)radius;
    (void)selector;
}
void lv_obj_set_style_border_width(lv_obj_t *object, int width, int selector)
{
    (void)object;
    (void)width;
    (void)selector;
}
void lv_obj_set_style_border_color(lv_obj_t *object, lv_color_t color, int selector)
{
    (void)object;
    (void)color;
    (void)selector;
}
void lv_obj_set_style_pad_all(lv_obj_t *object, int padding, int selector)
{
    (void)object;
    (void)padding;
    (void)selector;
}
void lv_obj_set_style_line_width(lv_obj_t *object, int width, int selector)
{
    (void)object;
    (void)width;
    (void)selector;
}
void lv_obj_set_style_line_color(lv_obj_t *object, lv_color_t color, int selector)
{
    (void)selector;
    object->line_color = color;
}
void lv_obj_set_style_line_rounded(lv_obj_t *object, bool rounded, int selector)
{
    (void)object;
    (void)rounded;
    (void)selector;
}
void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font, int selector)
{
    (void)object;
    (void)font;
    (void)selector;
}
void lv_obj_set_style_text_color(lv_obj_t *object, lv_color_t color, int selector)
{
    (void)selector;
    object->text_color = color;
}
void lv_obj_set_style_text_align(lv_obj_t *object, lv_text_align_t align, int selector)
{
    (void)object;
    (void)align;
    (void)selector;
}
void lv_obj_set_style_text_line_space(lv_obj_t *object, int space, int selector)
{
    (void)object;
    (void)space;
    (void)selector;
}
lv_color_t lv_obj_get_style_bg_color(const lv_obj_t *object, int selector)
{
    (void)selector;
    return object->bg;
}
lv_color_t lv_obj_get_style_line_color(const lv_obj_t *object, int selector)
{
    (void)selector;
    return object->line_color;
}
void lv_label_set_text(lv_obj_t *label, const char *text)
{
    snprintf(label->text, sizeof(label->text), "%s", text);
    lv_stub_text_sets++;
}
void lv_label_set_long_mode(lv_obj_t *label, lv_label_long_mode_t mode)
{
    (void)label;
    (void)mode;
}
const char *lv_label_get_text(const lv_obj_t *label)
{
    return label->text;
}
void lv_line_set_points(lv_obj_t *line, const lv_point_precise_t *points, uint32_t count)
{
    (void)line;
    (void)points;
    (void)count;
}
void lv_bar_set_range(lv_obj_t *bar, int min, int max)
{
    (void)bar;
    (void)min;
    (void)max;
}
void lv_bar_set_value(lv_obj_t *bar, int value, int animation)
{
    (void)animation;
    bar->bar_value = value;
}
void lv_image_set_src(lv_obj_t *image, const void *source)
{
    (void)image;
    (void)source;
}
void lv_image_set_scale(lv_obj_t *image, uint16_t scale)
{
    (void)image;
    (void)scale;
}
void lv_qrcode_set_size(lv_obj_t *qr, int size)
{
    qr->width = size;
    qr->height = size;
}
void lv_qrcode_set_dark_color(lv_obj_t *qr, lv_color_t color)
{
    (void)qr;
    (void)color;
}
void lv_qrcode_set_light_color(lv_obj_t *qr, lv_color_t color)
{
    (void)qr;
    (void)color;
}
void lv_qrcode_set_quiet_zone(lv_obj_t *qr, bool quiet)
{
    (void)qr;
    (void)quiet;
}
lv_result_t lv_qrcode_update(lv_obj_t *qr, const void *data, uint32_t length)
{
    (void)qr;
    (void)data;
    (void)length;
    return LV_RESULT_OK;
}
void lv_screen_load(lv_obj_t *screen)
{
    (void)screen;
}
lv_color_t lv_color_hex(uint32_t hex)
{
    return hex;
}
lv_color_t lv_color_black(void)
{
    return 0;
}
lv_color_t lv_color_white(void)
{
    return 0xFFFFFF;
}
bool lv_color_eq(lv_color_t a, lv_color_t b)
{
    return a == b;
}
