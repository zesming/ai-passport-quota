"""Run the whole UI source against a struct-backed LVGL stand-in (tests/host_sdk/lvgl.h)."""
import unittest
from runtime_helpers import compile_and_run

UI_SOURCES = (
    "main/quota_logic.c",
    "main/quota_brand_assets.c",
    "tests/host_sdk/lvgl_stub.c",
    "tests/cjson/cJSON.c",
)


class PhysicalUiRuntime(unittest.TestCase):
    def test_storage_barrier_is_visible_without_setup_or_authorization(self):
        # quota_ui.c is included so the harness can read its private widget table.
        harness = r"""
#include "quota_ui.c"
#include <assert.h>

static quota_service_view_t service;
static quota_navigation_t nav;
static void show(quota_screen_t screen)
{
    nav.screen = screen;
    quota_ui_render(&nav, &service, 50);
}
static bool qr_visible(void)
{
    return s_ui.qr_data[0] != 0;
}

int main(void)
{
    quota_ui_init();
    assert(sizeof(service.portable.storage_error) == 32);
    service.portable.setup_active = true;
    service.portable.setup_ready = true;
    service.portable.login_state = QUOTA_PORTABLE_LOGIN_WAITING;
    snprintf(service.portable.login_url, sizeof(service.portable.login_url),
             "https://official.example.invalid");
    snprintf(service.portable.login_user_code, sizeof(service.portable.login_user_code),
             "USER-CODE");
    const char *codes[] = {"storage_invalid", "recovery_conflict", "storage_write_unknown"};
    const char *titles[] = {"存储记录损坏", "存储冲突", "保存待确认"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(service.portable.storage_error, sizeof(service.portable.storage_error), "%s",
                 codes[i]);
        show(QUOTA_SCREEN_HOME);
        assert(!strcmp(s_ui.home_provider->text, titles[i]));
        assert(!strstr(s_ui.home_plan->text, "打开设备设置"));
        assert(s_ui.home_status->text_color == UI_AMBER);
        show(QUOTA_SCREEN_NETWORK);
        assert(!strcmp(s_ui.network_title->text, titles[i]));
        assert(strstr(s_ui.network_info->text, titles[i]));
        nav.phone_step = 2;
        show(QUOTA_SCREEN_PHONE);
        assert(!strcmp(s_ui.qr_title->text, titles[i]) && !qr_visible());
        assert(s_ui.manual_secret->text[0] == 0);
        show(QUOTA_SCREEN_AUTH);
        assert(!strcmp(s_ui.qr_title->text, titles[i]) && !qr_visible());
        assert(!strstr(s_ui.qr_countdown->text, "有效时间"));
        show(QUOTA_SCREEN_SETUP);
        assert(!strcmp(s_ui.setup_countdown->text, titles[i]));
    }
    service.portable.storage_error[0] = 0;
    show(QUOTA_SCREEN_HOME);
    assert(!strcmp(s_ui.home_provider->text, "暂无账户"));
    assert(strstr(s_ui.home_plan->text, "打开设备设置"));
    show(QUOTA_SCREEN_NETWORK);
    assert(!strcmp(s_ui.network_title->text, "尚未配置网络"));
    assert(strstr(s_ui.network_hint->text, "设备设置"));
    service.portable.saved_network_count = 3;
    service.portable.selected_saved_network = 1;
    for (unsigned i = 0; i < 3; i++)
        snprintf(service.portable.saved_network_ssids[i],
                 sizeof(service.portable.saved_network_ssids[i]), "Saved-%u", i);
    show(QUOTA_SCREEN_NETWORK);
    assert(!strcmp(s_ui.network_saved[0]->text, "  Saved-0"));
    assert(!strcmp(s_ui.network_saved[1]->text, "* Saved-1"));
    assert(!strcmp(s_ui.network_saved[2]->text, "  Saved-2"));
    service.portable.saved_network_count = 1;
    show(QUOTA_SCREEN_NETWORK);
    assert(s_ui.network_saved[1]->text[0] == 0 && s_ui.network_saved[2]->text[0] == 0);
    snprintf(service.portable.storage_error, sizeof(service.portable.storage_error),
             "storage_invalid");
    show(QUOTA_SCREEN_NETWORK);
    assert(s_ui.network_saved[0]->text[0] == 0);
    service.portable.storage_error[0] = 0;
    show(QUOTA_SCREEN_AUTH);
    assert(qr_visible());
    puts("physical storage-error visibility and recovery rendering passed");
}
"""
        compile_and_run(harness, "quota-physical-storage-ui-", UI_SOURCES, host_sdk=True)

    def test_unchanged_menu_does_not_reallocate_text_or_repaint_rows(self):
        harness = r"""
#include "quota_ui.c"
#include <assert.h>

int main(void)
{
    lv_obj_t row[6] = {0}, marker[6] = {0}, label[6] = {0};
    for (unsigned i = 0; i < 6; i++) {
        marker[i].flags = LV_OBJ_FLAG_HIDDEN;
        row[i].bg = UI_BG;
    }
    lv_stub_text_sets = lv_stub_bg_sets = 0;
    for (unsigned frame = 0; frame < 3; frame++)
        for (unsigned i = 0; i < 6; i++) {
            set_label_text(&label[i], "设置");
            set_row_focus(&row[i], &marker[i], i == 0);
        }
    assert(lv_stub_text_sets == 6 && lv_stub_bg_sets == 1);
    for (unsigned i = 0; i < 6; i++)
        set_row_focus(&row[i], &marker[i], i == 1);
    assert(lv_stub_bg_sets == 3 && lv_stub_text_sets == 6);
    set_label_text(&label[1], "");
    assert(lv_stub_text_sets == 7 && !label[1].text[0]);
    s_ui.account_rows[0] = &row[1];
    s_ui.account_markers[0] = &marker[1];
    s_ui.account_primary[0] = &label[0];
    s_ui.account_secondary[0] = &label[1];
    set_account_row_visible(0, true);
    set_row_focus(&row[1], &marker[1], true);
    assert(lv_stub_bg_sets == 3 && row[1].bg == UI_PANEL);
    set_account_row_visible(0, false);
    set_account_row_visible(0, true);
    set_row_focus(&row[1], &marker[1], false);
    assert(lv_stub_bg_sets == 4 && row[1].bg == UI_BG && (marker[1].flags & LV_OBJ_FLAG_HIDDEN));
    lv_obj_t account_rows[QUOTA_MAX_ACCOUNTS + 1] = {0}, markers[QUOTA_MAX_ACCOUNTS + 1] = {0};
    lv_obj_t primary[QUOTA_MAX_ACCOUNTS + 1] = {0}, secondary[QUOTA_MAX_ACCOUNTS + 1] = {0},
                                          header = {0};
    s_ui.header_info = &header;
    for (unsigned i = 0; i < QUOTA_MAX_ACCOUNTS + 1; i++) {
        s_ui.account_rows[i] = &account_rows[i];
        s_ui.account_markers[i] = &markers[i];
        s_ui.account_primary[i] = &primary[i];
        s_ui.account_secondary[i] = &secondary[i];
        markers[i].flags = LV_OBJ_FLAG_HIDDEN;
        account_rows[i].bg = UI_BG;
    }
    quota_service_view_t view = {0};
    quota_navigation_t nav = {0};
    view.snapshot.account_count = 2;
    strcpy(view.snapshot.accounts[0].email, "first@example.invalid");
    strcpy(view.snapshot.accounts[1].email, "second@example.invalid");
    lv_stub_text_sets = lv_stub_bg_sets = 0;
    render_accounts(&nav, &view);
    assert(lv_stub_bg_sets == 1 && account_rows[0].bg == UI_PANEL);
    unsigned initial_texts = lv_stub_text_sets;
    render_accounts(&nav, &view);
    assert(lv_stub_bg_sets == 1 && lv_stub_text_sets == initial_texts);
    nav.account_focus = 1;
    render_accounts(&nav, &view);
    assert(lv_stub_bg_sets == 3 && account_rows[1].bg == UI_PANEL);
    view.snapshot.account_count = 0;
    nav.account_focus = 0;
    render_accounts(&nav, &view);
    view.snapshot.account_count = 2;
    render_accounts(&nav, &view);
    assert(account_rows[0].bg == UI_PANEL && account_rows[1].bg == UI_BG &&
           (markers[1].flags & LV_OBJ_FLAG_HIDDEN));
    puts("unchanged menu setters and two-row focus movement passed");
}
"""
        compile_and_run(harness, "quota-menu-redraw-", UI_SOURCES, host_sdk=True)


if __name__ == "__main__":
    unittest.main()
