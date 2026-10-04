[简体中文](README.zh_CN.md) · English

# Asset sources and generation

Keep redistribution notices with the assets. Do not commit private account data, credentials, device identifiers or unsanitized screenshots.

## Fonts

`fonts/quota_font_12.c` and `quota_font_16.c` are LVGL 9.5.0, 4-bpp subsets of Noto Sans SC Regular at 12/16 pixels. The source is the locked LVGL component's `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`; its SIL Open Font License 1.1 is retained in [NotoSansSC-OFL.txt](fonts/NotoSansSC-OFL.txt).

Coverage is ASCII U+0020–U+007E plus `fonts/quota-font-glyphs.txt`; `quota-font-codepoints.txt` records non-ASCII coverage. Generated sources stay read-only in Flash. After changing fixed UI literals, update the lists and regenerate both sizes with `lv_font_conv@1.5.3` (shown for size 12):

```bash
npx --yes lv_font_conv@1.5.3 \
  --font managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf \
  --range 0x20-0x7E --symbols "$(cat assets/fonts/quota-font-glyphs.txt)" \
  --size 12 --bpp 4 --format lvgl --no-compress \
  --lv-font-name quota_font_12 --lv-include lvgl.h \
  --output assets/fonts/quota_font_12.c
```

`tests/test_quota_fonts.py` checks generated cmap coverage against UI literals and ASCII. Widget font selection and board rendering still need verification; arbitrary non-ASCII account identity uses the application's `?` fallback.

The 12-pixel font uses `lv_font_montserrat_12` as a fallback for its built-in refresh glyph (U+F021); add `--lv-fallback lv_font_montserrat_12` when regenerating that size. The 16-pixel font has no fallback.

## Provider marks

Firmware SVG sources are `images/openai-quota.svg`, `claude-quota.svg` and `deepseek-quota.svg`. Desktop copies are `companion/public/assets/openai.svg`, `claude.svg` and `deepseek.svg`. Sources: Lobe Icons [OpenAI](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/openai.svg), [Claude](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/claude-color.svg) and [DeepSeek](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/deepseek-color.svg). The MIT notice is retained in [images/lobe-icons-LICENSE.txt](images/lobe-icons-LICENSE.txt) and the desktop asset directory. Provider trademarks remain with their owners; the marks identify providers without implying endorsement.

Firmware PNGs are 36 × 36 RGBA, rendered with `@resvg/resvg-js@2.6.2`; descriptors are in `main/quota_brand_assets.c`. OpenAI currentColor is replaced with `#EDF1F4`; others retain source colors. Transparent pixels composite against `#11181F` before little-endian RGB565 conversion. Preserve dimensions, alpha handling and background when regenerating.

## Documentation captures

`docs/screenshots/` contains web captures made with isolated synthetic accounts through `companion/scripts/readme-preview.mjs`. They are not board photographs or live account/telemetry evidence. Capture the changed UI with that fixture service and check for private data before sharing.
