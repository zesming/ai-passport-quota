/* Host renderer for the device UI: the real quota_ui.c and quota_logic.c on the real LVGL with the
 * firmware's display settings (240 x 320, RGB565, a 240 x 20 partial buffer, rounded corners).
 * It draws every fixture of fixtures.json, one after the other on the same object pool, writes a
 * PNG for each and reports LVGL memory use.
 *
 *   ui_preview <fixtures.json> <output-dir> [scale]
 */
#include "bsp_display.h"
#include "bsp_display_rounding.h"
#include "bsp_pins.h"
#include "cJSON.h"
#include "lvgl.h"
#include "quota_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DRAW_LINES 20

static uint16_t s_frame[BSP_LCD_H][BSP_LCD_W];
static uint8_t s_draw_buffer[BSP_LCD_W * DRAW_LINES * 2] __attribute__((aligned(4)));
static unsigned s_flushes;

/* Device side of bsp_display_lvgl.c: pixels outside the rounded screen are black. */
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    int32_t width = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; y++) {
        const uint16_t *row = (const uint16_t *)(pixels + (y - area->y1) * width * 2);
        int32_t x1, x2;
        bool visible =
            bsp_display_rounded_row_span(y, BSP_LCD_W, BSP_LCD_H, BSP_LVGL_SCREEN_RADIUS, &x1, &x2);
        for (int32_t x = area->x1; x <= area->x2; x++) {
            bool inside = visible && x >= x1 && x <= x2;
            s_frame[y][x] = inside ? row[x - area->x1] : 0;
        }
    }
    s_flushes++;
    lv_display_flush_ready(display);
}

/* ---- PNG (stored deflate blocks: no compression library needed) ------------------------------ */

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++)
        crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put32(FILE *file, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                        (uint8_t)value};
    fwrite(bytes, 1, 4, file);
}

static void chunk(FILE *file, const char *type, const uint8_t *data, size_t length)
{
    put32(file, (uint32_t)length);
    fwrite(type, 1, 4, file);
    if (length)
        fwrite(data, 1, length, file);
    uint32_t crc = crc_update(0xffffffffu, (const uint8_t *)type, 4);
    crc = crc_update(crc, data, length);
    put32(file, crc ^ 0xffffffffu);
}

static bool write_png(const char *path, const uint8_t *rgb, int width, int height)
{
    size_t row_bytes = (size_t)width * 3 + 1, raw_bytes = row_bytes * (size_t)height;
    uint8_t *raw = malloc(raw_bytes);
    if (raw == NULL)
        return false;
    for (int y = 0; y < height; y++) {
        raw[row_bytes * y] = 0;
        memcpy(raw + row_bytes * y + 1, rgb + (size_t)width * 3 * y, (size_t)width * 3);
    }
    size_t blocks = (raw_bytes + 65534) / 65535;
    size_t zlib_bytes = 2 + raw_bytes + blocks * 5 + 4;
    uint8_t *zlib = malloc(zlib_bytes);
    if (zlib == NULL) {
        free(raw);
        return false;
    }
    size_t at = 0;
    zlib[at++] = 0x78;
    zlib[at++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t done = 0; done < raw_bytes;) {
        size_t size = raw_bytes - done > 65535 ? 65535 : raw_bytes - done;
        zlib[at++] = done + size == raw_bytes;
        zlib[at++] = (uint8_t)size;
        zlib[at++] = (uint8_t)(size >> 8);
        zlib[at++] = (uint8_t)~size;
        zlib[at++] = (uint8_t)(~size >> 8);
        memcpy(zlib + at, raw + done, size);
        for (size_t i = 0; i < size; i++) {
            a = (a + raw[done + i]) % 65521;
            b = (b + a) % 65521;
        }
        at += size;
        done += size;
    }
    uint32_t adler = (b << 16) | a;
    zlib[at++] = (uint8_t)(adler >> 24);
    zlib[at++] = (uint8_t)(adler >> 16);
    zlib[at++] = (uint8_t)(adler >> 8);
    zlib[at++] = (uint8_t)adler;
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL;
    if (ok) {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        fwrite(signature, 1, 8, file);
        uint8_t header[13] = {(uint8_t)(width >> 24),
                              (uint8_t)(width >> 16),
                              (uint8_t)(width >> 8),
                              (uint8_t)width,
                              (uint8_t)(height >> 24),
                              (uint8_t)(height >> 16),
                              (uint8_t)(height >> 8),
                              (uint8_t)height,
                              8,
                              2,
                              0,
                              0,
                              0};
        chunk(file, "IHDR", header, sizeof(header));
        chunk(file, "IDAT", zlib, at);
        chunk(file, "IEND", NULL, 0);
        ok = fclose(file) == 0;
    }
    free(zlib);
    free(raw);
    return ok;
}

static bool save_frame(const char *path, int scale)
{
    int width = BSP_LCD_W * scale, height = BSP_LCD_H * scale;
    uint8_t *rgb = malloc((size_t)width * height * 3);
    if (rgb == NULL)
        return false;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t pixel = s_frame[y / scale][x / scale];
            uint8_t *out = rgb + ((size_t)y * width + x) * 3;
            out[0] = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
            out[1] = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
            out[2] = (uint8_t)((pixel & 0x1f) * 255 / 31);
        }
    }
    bool ok = write_png(path, rgb, width, height);
    free(rgb);
    return ok;
}

/* ---- fixtures -------------------------------------------------------------------------------- */

static const cJSON *member(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static void text_field(const cJSON *object, const char *name, char *out, size_t capacity)
{
    const cJSON *value = member(object, name);
    if (cJSON_IsString(value))
        snprintf(out, capacity, "%s", value->valuestring);
}

static double number_field(const cJSON *object, const char *name, double fallback)
{
    const cJSON *value = member(object, name);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

static bool bool_field(const cJSON *object, const char *name, bool fallback)
{
    const cJSON *value = member(object, name);
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}

static int choice(const cJSON *object, const char *name, const char *const *names, int count,
                  int fallback)
{
    const cJSON *value = member(object, name);
    if (!cJSON_IsString(value))
        return fallback;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], value->valuestring) == 0)
            return i;
    }
    fprintf(stderr, "unknown %s \"%s\"\n", name, value->valuestring);
    exit(2);
}

static void load_window(const cJSON *json, quota_window_t *window, uint64_t now)
{
    if (!cJSON_IsObject(json))
        return;
    window->present = true;
    window->remaining_percent = (uint8_t)number_field(json, "remaining", 0);
    if (member(json, "resets_in")) {
        window->has_resets_at = true;
        window->resets_at = now + (uint64_t)number_field(json, "resets_in", 0);
    }
}

static void load_account(const cJSON *json, size_t index, quota_service_view_t *view)
{
    static const char *const providers[] = {"codex", "deepseek"};
    static const char *const statuses[] = {"ok", "waiting", "expired", "error", "unsupported"};
    static const char *const validations[] = {"saved", "ok", "pending", "failed"};
    quota_snapshot_t *snapshot = &view->snapshot;
    quota_account_t *account = &snapshot->accounts[index];
    uint64_t now = view->now_epoch;
    snprintf(account->id, sizeof(account->id), "%032zx", index + 1);
    account->provider = choice(json, "provider", providers, 2, 0) == 0 ? QUOTA_PROVIDER_CODEX
                                                                       : QUOTA_PROVIDER_DEEPSEEK;
    text_field(json, "email", account->email, sizeof(account->email));
    text_field(json, "plan", account->plan, sizeof(account->plan));
    account->status = (quota_source_status_t)choice(json, "status", statuses, 5, 0);
    if (member(json, "observed_ago")) {
        account->has_observed_at = true;
        account->observed_at = now - (uint64_t)number_field(json, "observed_ago", 0);
    }
    load_window(member(json, "five_hour"), &account->five_hour, now);
    load_window(member(json, "seven_day"), &account->seven_day, now);
    if (account->provider == QUOTA_PROVIDER_DEEPSEEK) {
        quota_balance_t *balance = &snapshot->balances[index];
        balance->present = true;
        balance->is_available = true;
        text_field(json, "label", balance->label, sizeof(balance->label));
        const cJSON *amount = member(json, "balance");
        if (cJSON_IsString(amount)) {
            balance->currency_count = 1;
            text_field(json, "currency", balance->balance_infos[0].currency, 4);
            if (balance->balance_infos[0].currency[0] == '\0')
                snprintf(balance->balance_infos[0].currency, 4, "CNY");
            snprintf(balance->balance_infos[0].total_balance, QUOTA_BALANCE_AMOUNT_BYTES + 1, "%s",
                     amount->valuestring);
        }
    } else {
        quota_codex_extras_t *extras = &snapshot->codex_extras[index];
        if (member(json, "credits")) {
            extras->has_credits = true;
            text_field(json, "credits", extras->credits_balance, sizeof(extras->credits_balance));
            extras->unlimited_credits = bool_field(json, "unlimited_credits", false);
        }
        if (member(json, "resets")) {
            extras->has_banked_reset = true;
            extras->available_resets = (uint64_t)number_field(json, "resets", 0);
        }
        if (member(json, "reset_expires_in")) {
            extras->has_next_reset_expiry = true;
            extras->next_reset_expires_at =
                now + (uint64_t)number_field(json, "reset_expires_in", 0);
        }
    }
    quota_portable_view_t *portable = &view->portable;
    text_field(json, "error", portable->account_errors[index],
               sizeof(portable->account_errors[index]));
    if (member(json, "retry_in"))
        portable->account_retry_at[index] = now + (uint64_t)number_field(json, "retry_in", 0);
    portable->account_validation[index] =
        (uint8_t)choice(json, "validation", validations, 4, QUOTA_VALIDATION_OK);
}

static void load_view(const cJSON *json, quota_service_view_t *view)
{
    static const char *const network_states[] = {
        "off", "ap", "connecting", "connected", "time_required", "ready", "error"};
    static const char *const login_states[] = {"idle",     "queued",     "connecting", "requesting",
                                               "waiting",  "exchanging", "success",    "error",
                                               "canceled", "expired"};
    static const char *const validations[] = {"saved", "ok", "pending", "failed"};
    memset(view, 0, sizeof(*view));
    view->now_epoch = (uint64_t)number_field(json, "now", 1791527520);
    view->clock_synchronized = bool_field(json, "clock_synchronized", true);
    view->snapshot_valid = view->configured = true;
    view->refreshing = bool_field(json, "refreshing", false);
    view->refresh_seconds = (uint16_t)number_field(json, "refresh_seconds", 300);
    view->auto_refresh = bool_field(json, "auto_refresh", true);
    view->screen_timeout_seconds = (uint16_t)number_field(json, "screen_timeout_seconds", 120);
    const cJSON *accounts = member(json, "accounts");
    int count = cJSON_IsArray(accounts) ? cJSON_GetArraySize(accounts) : 0;
    view->snapshot.account_count =
        (uint8_t)(count > QUOTA_MAX_ACCOUNTS ? QUOTA_MAX_ACCOUNTS : count);
    for (int i = 0; i < view->snapshot.account_count; i++)
        load_account(cJSON_GetArrayItem(accounts, i), (size_t)i, view);

    quota_portable_view_t *portable = &view->portable;
    const cJSON *source = member(json, "portable");
    portable->network_state =
        (quota_portable_network_state_t)choice(source, "network_state", network_states, 7, 0);
    view->connected = portable->network_state == QUOTA_PORTABLE_NETWORK_READY ||
                      portable->network_state == QUOTA_PORTABLE_NETWORK_CONNECTED;
    view->wifi_rssi = view->connected ? (int8_t)number_field(source, "rssi", 0) : 0;
    text_field(source, "ssid", portable->network_ssid, sizeof(portable->network_ssid));
    text_field(source, "ip", portable->network_ip, sizeof(portable->network_ip));
    text_field(source, "storage_error", portable->storage_error, sizeof(portable->storage_error));
    text_field(source, "firmware", portable->firmware, sizeof(portable->firmware));
    portable->validating = bool_field(source, "validating", false);
    portable->setup_opening = bool_field(source, "setup_opening", false);
    portable->setup_result = bool_field(source, "setup_result", false);
    portable->saving = bool_field(source, "saving", false);
    portable->factory_reset_failed = bool_field(source, "factory_reset_failed", false);
    portable->pending_items = (uint8_t)number_field(source, "pending", 0);
    portable->failed_items = (uint8_t)number_field(source, "failed", 0);
    portable->setup_active = bool_field(source, "setup_active", false);
    portable->setup_ready = bool_field(source, "setup_ready", portable->setup_active);
    portable->setup_seconds_left = (uint32_t)number_field(source, "setup_seconds_left", 0);
    text_field(source, "setup_ssid", portable->setup_ssid, sizeof(portable->setup_ssid));
    text_field(source, "setup_password", portable->setup_password,
               sizeof(portable->setup_password));
    text_field(source, "setup_secret", portable->setup_secret, sizeof(portable->setup_secret));
    text_field(source, "setup_page_url", portable->setup_page_url,
               sizeof(portable->setup_page_url));
    portable->login_state =
        (quota_portable_login_state_t)choice(source, "login_state", login_states, 10, 0);
    text_field(source, "login_url", portable->login_url, sizeof(portable->login_url));
    text_field(source, "login_user_code", portable->login_user_code,
               sizeof(portable->login_user_code));
    text_field(source, "login_error", portable->login_error, sizeof(portable->login_error));
    portable->login_seconds_left = (uint32_t)number_field(source, "login_seconds_left", 0);
    const cJSON *networks = member(source, "saved_networks");
    int network_count = cJSON_IsArray(networks) ? cJSON_GetArraySize(networks) : 0;
    portable->saved_network_count =
        (uint8_t)(network_count > QUOTA_PORTABLE_NETWORKS ? QUOTA_PORTABLE_NETWORKS
                                                          : network_count);
    for (int i = 0; i < portable->saved_network_count; i++) {
        const cJSON *item = cJSON_GetArrayItem(networks, i);
        text_field(item, "ssid", portable->saved_network_ssids[i],
                   sizeof(portable->saved_network_ssids[i]));
        portable->saved_network_validation[i] =
            (uint8_t)choice(item, "validation", validations, 4, QUOTA_VALIDATION_OK);
    }
    const cJSON *usb = member(json, "usb");
    view->usb_window_active = bool_field(usb, "active", false);
    view->usb_window_preparing = bool_field(usb, "preparing", false);
    view->usb_page_connected = bool_field(usb, "connected", false);
    view->usb_window_seconds_left = (uint32_t)number_field(usb, "seconds_left", 0);
}

static void load_navigation(const cJSON *json, quota_navigation_t *navigation)
{
    static const char *const screens[] = {"home",  "menu", "hotspot", "usb",    "refresh",
                                          "sleep", "info", "auth",    "confirm"};
    static const char *const kinds[] = {"factory_reset", "cancel_auth"};
    quota_navigation_init(navigation, true, 300, true, 120);
    navigation->screen = (quota_screen_t)choice(json, "screen", screens, 9, 0);
    navigation->selected_account = (uint8_t)number_field(json, "selected_account", 0);
    navigation->menu_focus = (uint8_t)number_field(json, "menu_focus", 0);
    navigation->option_focus = (uint8_t)number_field(json, "option_focus", 0);
    navigation->hotspot_page = (uint8_t)number_field(json, "hotspot_page", 0);
    navigation->confirm_kind = (quota_confirm_kind_t)choice(json, "confirm_kind", kinds, 2, 0);
    navigation->confirm_focus = (uint8_t)number_field(json, "confirm_focus", 0);
    navigation->factory_resetting = bool_field(json, "factory_resetting", false);
    navigation->factory_failed = bool_field(json, "factory_failed", false);
    navigation->sleep_notice = bool_field(json, "sleep_notice", false);
}

static unsigned count_objects(lv_obj_t *object)
{
    unsigned count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
        count += count_objects(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (text != NULL && fread(text, 1, (size_t)size, file) == (size_t)size)
        text[size] = '\0';
    else {
        free(text);
        text = NULL;
    }
    fclose(file);
    return text;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s fixtures.json output-dir [scale]\n", argv[0]);
        return 2;
    }
    int scale = argc > 3 ? atoi(argv[3]) : 2;
    if (scale < 1 || scale > 4)
        scale = 2;
    setenv("TZ", "CST-8", 1);
    tzset();
    char *text = read_file(argv[1]);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    const cJSON *fixtures = root ? member(root, "fixtures") : NULL;
    if (!cJSON_IsArray(fixtures)) {
        fprintf(stderr, "cannot read fixtures from %s\n", argv[1]);
        return 2;
    }
    crc_init();
    lv_init();
    lv_display_t *display = lv_display_create(BSP_LCD_W, BSP_LCD_H);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_draw_buffer, NULL, sizeof(s_draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    unsigned before_ui = (unsigned)(monitor.total_size - monitor.free_size);
    quota_ui_init();
    lv_mem_monitor(&monitor);
    unsigned after_init = (unsigned)(monitor.total_size - monitor.free_size);

    static quota_service_view_t view;
    static quota_navigation_t navigation;
    unsigned baseline = 0, rendered = 0;
    const cJSON *fixture;
    cJSON_ArrayForEach(fixture, fixtures)
    {
        const cJSON *name = member(fixture, "name");
        if (!cJSON_IsString(name)) {
            fprintf(stderr, "fixture without a name\n");
            return 2;
        }
        load_view(member(fixture, "view"), &view);
        load_navigation(member(fixture, "nav"), &navigation);
        int battery = (int)number_field(fixture, "battery", 82);
        memset(s_frame, 0, sizeof(s_frame));
        bool usb_powered = bool_field(fixture, "usb_powered", false);
        quota_ui_render(&navigation, &view, battery, usb_powered);
        lv_obj_invalidate(lv_screen_active());
        s_flushes = 0;
        lv_refr_now(display);
        if (s_flushes == 0) {
            fprintf(stderr, "%s: the first render drew nothing\n", name->valuestring);
            return 1;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.png", argv[2], name->valuestring);
        if (!save_frame(path, scale)) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        /* The second pass over the same state, as the device's one-second refresh does, must not
         * touch a single pixel: nothing may restyle, retext or reshow what has not changed. */
        quota_ui_render(&navigation, &view, battery, usb_powered);
        s_flushes = 0;
        lv_refr_now(display);
        if (s_flushes != 0) {
            fprintf(stderr, "%s: an unchanged second render repainted %u areas\n",
                    name->valuestring, s_flushes);
            return 1;
        }
        lv_mem_monitor(&monitor);
        unsigned objects = count_objects(lv_screen_active());
        if (baseline == 0)
            baseline = objects;
        printf("fixture %s objects=%u used=%u max_used=%u\n", name->valuestring, objects,
               (unsigned)(monitor.total_size - monitor.free_size), (unsigned)monitor.max_used);
        if (objects != baseline) {
            fprintf(stderr, "object count changed: %u -> %u\n", baseline, objects);
            return 1;
        }
        rendered++;
    }
    lv_mem_monitor(&monitor);
    printf("summary fixtures=%u objects=%u pool=%u before_ui=%u after_init=%u max_used=%u\n",
           rendered, baseline, (unsigned)monitor.total_size, before_ui, after_init,
           (unsigned)monitor.max_used);
    cJSON_Delete(root);
    free(text);
    return 0;
}
