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
        BIND(extra_name[i]); BIND(extra_value[i]);
    }
    for (unsigned i=0;i<QUOTA_PORTABLE_NETWORKS+1;i++) BIND(network_saved[i]);
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
        render_network(&service.portable);
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
    render_network(&service.portable); assert(!strcmp(s_ui.network_title->text,"尚未配置网络"));
    assert(strstr(s_ui.network_hint->text,"设备设置"));
    service.portable.saved_network_count=3; service.portable.selected_saved_network=1;
    for (unsigned i=0;i<3;i++) snprintf(service.portable.saved_network_ssids[i],
        sizeof(service.portable.saved_network_ssids[i]),"Saved-%u",i);
    service.portable.pending_saved_network_present=true;
    snprintf(service.portable.pending_saved_network_ssid,
        sizeof(service.portable.pending_saved_network_ssid),"Historical");
    render_network(&service.portable);
    assert(!strcmp(s_ui.network_saved[0]->text,"  Saved-0"));
    assert(!strcmp(s_ui.network_saved[1]->text,"* Saved-1"));
    assert(!strcmp(s_ui.network_saved[3]->text,"待启用 Historical"));
    service.portable.saved_network_count=1; service.portable.pending_saved_network_present=false;
    render_network(&service.portable);
    assert(s_ui.network_saved[1]->text[0]==0 && s_ui.network_saved[3]->text[0]==0);
    snprintf(service.portable.storage_error,sizeof(service.portable.storage_error),"storage_invalid");
    render_network(&service.portable); assert(s_ui.network_saved[0]->text[0]==0);
    service.portable.storage_error[0]=0;
    render_auth(&service.portable); assert(qr_visible);
    puts("physical storage-error visibility and recovery rendering passed");
}
'''
        compile_and_run(harness, "quota-physical-storage-ui-", (
            "main/quota_logic.c", "tests/cjson/cJSON.c"))

    def test_unchanged_menu_does_not_reallocate_text_or_repaint_rows(self):
        source = (ROOT / "main/quota_ui.c").read_text()
        functions = "\n".join(extract_function(source, name) for name in
            ("set_label_text", "set_row_focus", "set_account_row_visible", "render_accounts"))
        harness = r'''
#include <assert.h>
#include "quota_logic.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct { char text[64]; unsigned flags, bg; } lv_obj_t;
typedef unsigned lv_color_t;
typedef struct { quota_snapshot_t snapshot; } quota_service_view_t;
static struct { lv_obj_t *header_info, *account_rows[QUOTA_MAX_ACCOUNTS+1], *account_markers[QUOTA_MAX_ACCOUNTS+1],
    *account_primary[QUOTA_MAX_ACCOUNTS+1], *account_secondary[QUOTA_MAX_ACCOUNTS+1]; } s_ui;
#define UI_PANEL 1
#define UI_BG 0
#define LV_OBJ_FLAG_HIDDEN 1
static unsigned texts, backgrounds;
static unsigned color(unsigned c) { return c; }
static unsigned lv_obj_get_style_bg_color(lv_obj_t *o,unsigned part) { (void)part; return o->bg; }
static bool lv_color_eq(unsigned a,unsigned b) { return a==b; }
static const char *lv_label_get_text(lv_obj_t *o) { return o->text; }
static void lv_label_set_text(lv_obj_t *o,const char *t) { ++texts; snprintf(o->text,sizeof(o->text),"%s",t); }
static bool lv_obj_has_flag(lv_obj_t *o,unsigned f) { return (o->flags&f)!=0; }
static void lv_obj_add_flag(lv_obj_t *o,unsigned f) { o->flags|=f; }
static void lv_obj_clear_flag(lv_obj_t *o,unsigned f) { o->flags&=~f; }
static void lv_obj_set_style_bg_color(lv_obj_t *o,unsigned c,unsigned part) { (void)part; ++backgrounds; o->bg=c; }
'''+functions+r'''
int main(void) {
    lv_obj_t row[6]={0}, marker[6]={0}, label[6]={0};
    for (unsigned i=0;i<6;i++) marker[i].flags=LV_OBJ_FLAG_HIDDEN;
    for (unsigned frame=0;frame<3;frame++) for (unsigned i=0;i<6;i++) {
        set_label_text(&label[i],"设置"); set_row_focus(&row[i],&marker[i],i==0);
    }
    assert(texts==6 && backgrounds==1);
    for (unsigned i=0;i<6;i++) set_row_focus(&row[i],&marker[i],i==1);
    assert(backgrounds==3 && texts==6);
    set_label_text(&label[1],""); assert(texts==7 && !label[1].text[0]);
    s_ui.account_rows[0]=&row[1]; s_ui.account_markers[0]=&marker[1];
    s_ui.account_primary[0]=&label[0]; s_ui.account_secondary[0]=&label[1];
    set_account_row_visible(0,true); set_row_focus(&row[1],&marker[1],true);
    assert(backgrounds==3 && row[1].bg==UI_PANEL);
    set_account_row_visible(0,false); set_account_row_visible(0,true);
    set_row_focus(&row[1],&marker[1],false);
    assert(backgrounds==4 && row[1].bg==UI_BG && (marker[1].flags&LV_OBJ_FLAG_HIDDEN));
    lv_obj_t account_rows[QUOTA_MAX_ACCOUNTS+1]={0}, markers[QUOTA_MAX_ACCOUNTS+1]={0};
    lv_obj_t primary[QUOTA_MAX_ACCOUNTS+1]={0}, secondary[QUOTA_MAX_ACCOUNTS+1]={0}, header={0};
    s_ui.header_info=&header;
    for(unsigned i=0;i<QUOTA_MAX_ACCOUNTS+1;i++) {
        s_ui.account_rows[i]=&account_rows[i]; s_ui.account_markers[i]=&markers[i];
        s_ui.account_primary[i]=&primary[i]; s_ui.account_secondary[i]=&secondary[i];
        markers[i].flags=LV_OBJ_FLAG_HIDDEN;
    }
    quota_service_view_t view={0}; quota_navigation_t nav={0}; view.snapshot.account_count=2;
    strcpy(view.snapshot.accounts[0].email,"first@example.invalid");
    strcpy(view.snapshot.accounts[1].email,"second@example.invalid");
    texts=backgrounds=0; render_accounts(&nav,&view);
    assert(backgrounds==1 && account_rows[0].bg==UI_PANEL);
    unsigned initial_texts=texts; render_accounts(&nav,&view);
    assert(backgrounds==1 && texts==initial_texts);
    nav.account_focus=1; render_accounts(&nav,&view);
    assert(backgrounds==3 && account_rows[1].bg==UI_PANEL);
    view.snapshot.account_count=0; nav.account_focus=0; render_accounts(&nav,&view);
    view.snapshot.account_count=2; render_accounts(&nav,&view);
    assert(account_rows[0].bg==UI_PANEL && account_rows[1].bg==UI_BG && (markers[1].flags&LV_OBJ_FLAG_HIDDEN));
    puts("unchanged menu setters and two-row focus movement passed");
}
'''
        compile_and_run(harness, "quota-menu-redraw-", ("main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
