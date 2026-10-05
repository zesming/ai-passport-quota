"""Execute physical storage-error rendering using real UI functions and fake labels."""
import re
import unittest
from runtime_helpers import ROOT, compile_and_run, extract_function


class PhysicalUiRuntime(unittest.TestCase):
    def test_storage_barrier_is_visible_without_setup_or_authorization(self):
        source = (ROOT / "main/quota_ui.c").read_text()
        def text_function(name):
            return re.search(r"^static const char \*" + name + r"\([^;]+?\)\s*\n\{.*?^\}",
                             source, re.M | re.S)[0]
        home = extract_function(source, "render_home")
        # Only the changed empty-account branch; account metrics remain covered elsewhere.
        home = home[:home.index("    size_t selected =")] + "    (void)deepseek;\n}\n"
        functions = "\n".join([text_function("portable_storage_title"),
                                text_function("portable_error_text"), home] + [
            extract_function(source, name) for name in
            ("render_network", "render_phone", "render_auth", "render_setup")])
        objects = re.search(r"typedef struct \{.*?\} quota_ui_objects_t;", source, re.S)[0]
        harness = r'''
#include "quota_portable.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { char text[256]; unsigned flags, width, color; } lv_obj_t;
typedef struct {
    quota_snapshot_t snapshot; quota_portable_view_t portable;
    bool pairing_preparing, pairing_active; uint32_t pairing_seconds_left;
} quota_service_view_t;
#define UI_AMBER 1
#define UI_MUTED 2
#define UI_MINT 3
#define LV_OBJ_FLAG_HIDDEN 1
static int quota_font_12, quota_font_16;
'''+objects+r'''
static quota_ui_objects_t s_ui;
static lv_obj_t nodes[64];
static bool qr_visible;
static void set_label_text(lv_obj_t *label, const char *text) {
    assert(label); snprintf(label->text, sizeof(label->text), "%s", text);
}
static unsigned color(unsigned value) { return value; }
static void lv_obj_add_flag(lv_obj_t *label, unsigned flag) { label->flags |= flag; }
static void lv_obj_clear_flag(lv_obj_t *label, unsigned flag) { label->flags &= ~flag; }
static void lv_obj_set_style_text_font(lv_obj_t *label, const void *font, int part) { (void)label; (void)font; (void)part; }
static void lv_obj_set_style_text_color(lv_obj_t *label, unsigned value, int part) { (void)part; label->color=value; }
static void lv_obj_set_y(lv_obj_t *label, int value) { (void)label; (void)value; }
static void lv_obj_set_x(lv_obj_t *label, int value) { (void)label; (void)value; }
static void lv_obj_set_height(lv_obj_t *label, int value) { (void)label; (void)value; }
static unsigned lv_obj_get_width(lv_obj_t *label) { return label->width; }
static void lv_qrcode_set_size(lv_obj_t *label, unsigned value) { label->width=value; }
static void set_row_focus(lv_obj_t *row, lv_obj_t *marker, bool focus) { (void)row; (void)marker; (void)focus; }
static void render_qr_data(const char *value) { qr_visible=value && value[0]; }
'''+functions+r'''
static void bind(void) {
    unsigned next=0;
#define BIND(field) s_ui.field=&nodes[next++]
    BIND(header_info); BIND(home_logo); BIND(home_provider); BIND(home_plan);
    BIND(home_email); BIND(home_status); BIND(home_empty);
    for (unsigned i=0;i<2;i++) {
        BIND(balance_heading[i]); BIND(balance_total[i]); BIND(metric_name[i]);
        BIND(metric_reset[i]); BIND(metric_value[i]); BIND(metric_bar[i]);
        BIND(extra_name[i]); BIND(extra_value[i]); BIND(network_rows[i]); BIND(network_markers[i]);
    }
    BIND(network_title); BIND(network_info); BIND(network_hint);
    BIND(qr); BIND(qr_title); BIND(qr_hint); BIND(qr_countdown);
    BIND(manual_address); BIND(manual_caption); BIND(manual_secret); BIND(footer);
    BIND(setup_countdown); BIND(setup_hint);
#undef BIND
    assert(next<=64); s_ui.qr->width=168;
}
int main(void) {
    quota_service_view_t service={0}; quota_navigation_t nav={0}; bind();
    assert(sizeof(service.portable.storage_error)==32);
    service.portable.setup_active=true; service.portable.setup_ready=true;
    service.portable.login_state=QUOTA_PORTABLE_LOGIN_WAITING;
    snprintf(service.portable.login_url,sizeof(service.portable.login_url),"https://official.example.invalid");
    snprintf(service.portable.login_user_code,sizeof(service.portable.login_user_code),"USER-CODE");
    const char *codes[]={"storage_invalid","recovery_conflict","storage_write_unknown"};
    const char *titles[]={"存储记录损坏","存储冲突","保存待确认"};
    for (unsigned i=0;i<3;i++) {
        snprintf(service.portable.storage_error,sizeof(service.portable.storage_error),"%s",codes[i]);
        render_home(&nav,&service);
        assert(!strcmp(s_ui.home_provider->text,titles[i]));
        assert(!strstr(s_ui.home_plan->text,"打开设备设置"));
        assert(s_ui.home_status->color==UI_AMBER);
        render_network(&nav,&service.portable);
        assert(!strcmp(s_ui.network_title->text,titles[i]));
        assert(strstr(s_ui.network_info->text,titles[i]));
        nav.phone_step=2; render_phone(&nav,&service.portable);
        assert(!strcmp(s_ui.qr_title->text,titles[i]) && !qr_visible);
        assert(s_ui.manual_secret->text[0]==0);
        render_auth(&service.portable);
        assert(!strcmp(s_ui.qr_title->text,titles[i]) && !qr_visible);
        assert(!strstr(s_ui.qr_countdown->text,"有效时间"));
        render_setup(&service);
        assert(!strcmp(s_ui.setup_countdown->text,titles[i]));
    }
    service.portable.storage_error[0]=0;
    render_home(&nav,&service); assert(!strcmp(s_ui.home_provider->text,"暂无账户"));
    assert(strstr(s_ui.home_plan->text,"打开设备设置"));
    render_network(&nav,&service.portable); assert(!strcmp(s_ui.network_title->text,"尚未配置网络"));
    assert(strstr(s_ui.network_hint->text,"2.4 GHz"));
    render_auth(&service.portable); assert(qr_visible);
    puts("physical storage-error visibility and recovery rendering passed");
}
'''
        compile_and_run(harness, "quota-physical-storage-ui-", (
            "main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
