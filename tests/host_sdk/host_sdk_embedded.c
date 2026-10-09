/* The setup page the firmware build embeds with EMBED_TXTFILES, and the firmware identity that
 * quota_portable_service.c answers with. */
extern const unsigned char setup_html_start[] __asm__("_binary_setup_page_html_start");
extern const unsigned char setup_html_end[] __asm__("_binary_setup_page_html_end");

const unsigned char setup_html_start[] =
    "<html><head><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
    "script-src 'sha256-test'\"></head></html>";
const unsigned char setup_html_end[] = {0};

const char *quota_portable_service_firmware(void)
{
    return "3.0.0-test";
}
