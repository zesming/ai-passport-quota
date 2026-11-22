/* Host replacement for the embedded setup page (single standard-C array, no cross-object pointer
 * subtraction) and the firmware identity that quota_portable_service.c answers with. */
#include <stddef.h>

static const char s_setup_html[] =
    "<html><head><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
    "script-src 'sha256-test'\"></head></html>";

const char *quota_portal_setup_html(size_t *length_out)
{
    if (length_out)
        *length_out = sizeof(s_setup_html) - 1;
    return s_setup_html;
}

const char *quota_portable_service_firmware(void)
{
    return "3.0.0-test";
}
