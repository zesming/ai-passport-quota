/* The setup page assets that the firmware build embeds with EMBED_TXTFILES. */
extern const unsigned char setup_html_start[] __asm__("_binary_portable_setup_html_start");
extern const unsigned char setup_html_end[] __asm__("_binary_portable_setup_html_end");
extern const unsigned char setup_js_start[] __asm__("_binary_portable_setup_mjs_start");
extern const unsigned char setup_js_end[] __asm__("_binary_portable_setup_mjs_end");
extern const unsigned char serial_js_start[] __asm__("_binary_portable_serial_mjs_start");
extern const unsigned char serial_js_end[] __asm__("_binary_portable_serial_mjs_end");

const unsigned char setup_html_start[] = "<html></html>";
const unsigned char setup_html_end[] = {0};
const unsigned char setup_js_start[] = "export {};";
const unsigned char setup_js_end[] = {0};
const unsigned char serial_js_start[] = "export {};";
const unsigned char serial_js_end[] = {0};
