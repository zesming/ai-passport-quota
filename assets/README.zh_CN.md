<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 额度看板资源

- `fonts/quota_font_12.c`、`fonts/quota_font_16.c`：Noto Sans SC Regular 的 LVGL 9、4-bpp 字体子集，分别为 12 和 16 像素。输入为固定 LVGL 组件的 `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`；SIL OFL 1.1 许可证保存在 `fonts/NotoSansSC-OFL.txt`。使用 `lv_font_conv@1.5.3` 生成，覆盖 ASCII U+0020–U+007E 及 `fonts/quota-font-glyphs.txt`；精确非 ASCII 字码记录在 `fonts/quota-font-codepoints.txt`。界面中文使用这些字体；账户提供的非 ASCII 邮箱和订阅名称采用明确的 ASCII 回退，避免缺字。字体源只读链接到 Flash，不在 RAM 加载完整中文字库，仍需真机验证显示。
- `images/openai-quota.svg`、`images/claude-quota.svg`、`images/deepseek-quota.svg`：已批准设计采用的平台标识，来源为 [Lobe Icons OpenAI](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/openai.svg) 和 [Lobe Icons Claude](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/claude-color.svg)。DeepSeek 来源为 [Lobe Icons DeepSeek](https://github.com/lobehub/lobe-icons/blob/master/packages/static-svg/icons/deepseek-color.svg)。图标包采用 MIT 许可；平台商标权仍归原权利人。仅用于标识订阅平台，不代表官方背书。
- `images/openai-quota.png`、`images/claude-quota.png`、`images/deepseek-quota.png`：36 × 36 RGBA PNG，使用 `@resvg/resvg-js@2.6.2` 从 SVG 确定性渲染。生成的 RGB565 LVGL 描述符位于 `main/quota_brand_assets.c`，用于主页和账户列表；OpenAI 的 `currentColor` 替换为 `#EDF1F4` 白灰色，其他平台保持各自颜色；透明像素合成到 `#11181F` 背景后转换为小端 RGB565。

修改界面文本后，先更新两份字形列表，在仓库根目录运行以下命令；16 像素字体使用对应尺寸、字体名和输出路径：

```bash
npx --yes lv_font_conv@1.5.3 \
  --font managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf \
  --range 0x20-0x7E --symbols "$(cat assets/fonts/quota-font-glyphs.txt)" \
  --size 12 --bpp 4 --format lvgl --no-compress \
  --lv-font-name quota_font_12 --lv-include lvgl.h \
  --output assets/fonts/quota_font_12.c
```

`tests/test_quota_fonts.py` 对照界面实际文本和 ASCII 范围检查生成字体 cmap 表。图标原始版权声明保存在 `images/lobe-icons-LICENSE.txt`。
