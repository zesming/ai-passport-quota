# 资源来源与生成

资源保留再分发声明，不得提交私有账号数据、凭证、设备标识或未脱敏截图。

## 字体

`fonts/quota_font_12.c` 和 `quota_font_16.c` 是适用于 LVGL 9.5.0 的 Noto Sans SC Regular 12/16 像素、4-bpp 子集。源字体为锁定 LVGL 组件的 `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`；SIL Open Font License 1.1 保留于 [NotoSansSC-OFL.txt](fonts/NotoSansSC-OFL.txt)。

覆盖三类字符：ASCII U+0020–U+007E；常用汉字 `fonts/hanzi-level1.txt`（GB 2312 一级汉字 3,755 个，用来显示备注名和 Wi-Fi 名里的日常中文）；界面字符串里出现的其余非 ASCII 字符和常用全角标点（`tools/font_glyphs.py` 的 `PUNCTUATION`）。三类合并的非 ASCII 列表是 `fonts/quota-font-glyphs.txt`（3,800 个）。

`hanzi-level1.txt` 的来源：GB 2312-1980 的第 16–55 区（一级汉字，按拼音排序），由 Python 的 `gb2312` 编码表导出，只是 Unicode 码位的列表，不含字形，无版权要求；`python3 tools/font_glyphs.py --check`（也是 `tests/test_quota_fonts.py` 的一项）重新导出并逐字比对。它不是《通用规范汉字表》一级字表（约 3,500 字）：两张表大部分重合但不完全相同，选用 GB 2312 是因为它可以离线、可复现地验证；若要换成规范汉字表，替换这个文件并去掉 `--check` 里的 GB 2312 比对即可。字形仍来自 Noto Sans SC，SIL OFL 1.1，见 [NotoSansSC-OFL.txt](fonts/NotoSansSC-OFL.txt)。

界面文本变化或要改汉字范围后运行（需要 npx 和网络，使用 `lv_font_conv@1.5.3`，字体源与参数见两个 `.c` 文件头部）：

```bash
python3 tools/font_glyphs.py --build
```

只更新字形表而不重新生成字体时去掉 `--build`。生成的两个 `.c` 各约 2–3 MB，是 4-bpp 未压缩位图，只读留在 Flash。`tests/test_quota_fonts.py` 检查字形表与生成器一致、两种字号都含全部字形和日常中文样例、界面字面文本和 ASCII 都被覆盖。控件字体选择与真机显示仍须验证。字库没有的字（生僻字、表情等）在设备上整体回退为“DeepSeek N”或“Wi-Fi N”（N 为列表顺序），邮箱只显示 ASCII，其余字符为 `?`。两种字号都没有回退字体。

## 服务标志

固件 SVG 源为 `images/openai-quota.svg` 和 `deepseek-quota.svg`。来源为 Lobe Icons 的 [OpenAI](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/openai.svg) 和 [DeepSeek](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/deepseek-color.svg)。MIT 声明保留于 [images/lobe-icons-LICENSE.txt](images/lobe-icons-LICENSE.txt)。商标归各服务所有，标志用于识别，不代表背书。

固件 PNG 为 36 × 36 RGBA，以 `@resvg/resvg-js@2.6.2` 渲染；描述符位于 `main/quota_brand_assets.c`。OpenAI currentColor 替换为 `#EDF1F4`，DeepSeek 保留原色。透明像素先与 `#11181F` 合成，再转为小端 RGB565。重新生成时保留尺寸、透明处理和背景。

## 文档截图

文档截图（目前没有）使用隔离合成账号，不是真机照片或实时账号/遥测证据。设备界面预览见[开发指南](../docs/development/README.md#设备界面预览)；可用 `node tools/preview_portable.mjs` 的示例设备截取设置页；公开前检查私有数据。
