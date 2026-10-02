<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Quota monitor assets

- `fonts/quota_font_12.c` and `fonts/quota_font_16.c`: LVGL 9, 4-bpp subsets of Noto Sans SC Regular at 12 and 16 pixels. Input: the locked LVGL component's `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`; license: SIL Open Font License 1.1, preserved in `fonts/NotoSansSC-OFL.txt`. Generated with `lv_font_conv@1.5.3`, ASCII U+0020–U+007E plus `fonts/quota-font-glyphs.txt`. `fonts/quota-font-codepoints.txt` records the exact non-ASCII coverage. Quota UI text uses these fonts; account-provided non-ASCII email/plan text is rendered with an explicit ASCII fallback to avoid missing glyphs. The font sources are linked read-only in Flash; no full CJK font is loaded into RAM. On-device rendering is still required for acceptance.
- `images/openai-quota.svg`, `images/claude-quota.svg` and `images/deepseek-quota.svg`: the approved design's provider marks, sourced from [Lobe Icons OpenAI](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/openai.svg) and [Lobe Icons Claude](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/claude-color.svg). DeepSeek uses [Lobe Icons DeepSeek](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/deepseek-color.svg). The icon package is MIT-licensed; provider trademarks remain with their owners. They identify connected subscription providers, not endorsement.
- `images/openai-quota.png`, `images/claude-quota.png` and `images/deepseek-quota.png`: 36 × 36 RGBA PNG, deterministically rendered with `@resvg/resvg-js@2.6.2` from the SVGs. Their generated RGB565 LVGL image descriptors live in `main/quota_brand_assets.c` and are used on the dashboard and account list. OpenAI currentColor is replaced by white-gray `#EDF1F4`; other providers retain their colors. Transparent pixels are composited against `#11181F`, then converted to little-endian RGB565.

To regenerate the font subsets after changing literal UI text, update the two glyph lists, then run from the repository root (repeat with size 16 and its matching font name/output):

```bash
npx --yes lv_font_conv@1.5.3 \
  --font managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf \
  --range 0x20-0x7E --symbols "$(cat assets/fonts/quota-font-glyphs.txt)" \
  --size 12 --bpp 4 --format lvgl --no-compress \
  --lv-font-name quota_font_12 --lv-include lvgl.h \
  --output assets/fonts/quota_font_12.c
```

`tests/test_quota_fonts.py` checks the generated cmap tables against actual UI literals and the ASCII range. The original icon copyright notice is retained in `images/lobe-icons-LICENSE.txt`.
