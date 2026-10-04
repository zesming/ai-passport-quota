简体中文 · [English](README.md)

# 资源来源与生成

资源保留再分发声明，不得提交私有账号数据、凭证、设备标识或未脱敏截图。

## 字体

`fonts/quota_font_12.c` 和 `quota_font_16.c` 是适用于 LVGL 9.5.0 的 Noto Sans SC Regular 12/16 像素、4-bpp 子集。源字体为锁定 LVGL 组件的 `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`；SIL Open Font License 1.1 保留于 [NotoSansSC-OFL.txt](fonts/NotoSansSC-OFL.txt)。

覆盖 ASCII U+0020–U+007E 和 `fonts/quota-font-glyphs.txt`，`quota-font-codepoints.txt` 记录非 ASCII 覆盖。生成源只读留在 Flash。固定 UI 文本变化后更新列表，并用 `lv_font_conv@1.5.3` 重新生成两种字号（以下为 12）：

```bash
npx --yes lv_font_conv@1.5.3 \
  --font managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf \
  --range 0x20-0x7E --symbols "$(cat assets/fonts/quota-font-glyphs.txt)" \
  --size 12 --bpp 4 --format lvgl --no-compress \
  --lv-font-name quota_font_12 --lv-include lvgl.h \
  --output assets/fonts/quota_font_12.c
```

`tests/test_quota_fonts.py` 检查生成 cmap 对 UI 字面文本和 ASCII 的覆盖。控件字体选择与真机显示仍须验证；任意非 ASCII 账号身份采用应用的 `?` 回退。

12 像素字体回退到 `lv_font_montserrat_12` 中的内置刷新图标（U+F021）；重新生成此字号时添加 `--lv-fallback lv_font_montserrat_12`。16 像素字体没有回退字体。

## 服务标志

固件 SVG 源为 `images/openai-quota.svg`、`claude-quota.svg` 和 `deepseek-quota.svg`，电脑端副本为 `companion/public/assets/openai.svg`、`claude.svg` 和 `deepseek.svg`。来源为 Lobe Icons 的 [OpenAI](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/openai.svg)、[Claude](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/claude-color.svg) 和 [DeepSeek](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/deepseek-color.svg)。MIT 声明保留于 [images/lobe-icons-LICENSE.txt](images/lobe-icons-LICENSE.txt) 和电脑端资源目录。商标归各服务所有，标志用于识别，不代表背书。

固件 PNG 为 36 × 36 RGBA，以 `@resvg/resvg-js@2.6.2` 渲染；描述符位于 `main/quota_brand_assets.c`。OpenAI currentColor 替换为 `#EDF1F4`，其他标志保留原色。透明像素先与 `#11181F` 合成，再转为小端 RGB565。重新生成时保留尺寸、透明处理和背景。

## 文档截图

`docs/screenshots/` 使用 `companion/scripts/readme-preview.mjs` 的隔离合成账号生成网页截图，不是真机照片或实时账号/遥测证据。UI 变化使用该示例服务截图，公开前检查私有数据。
