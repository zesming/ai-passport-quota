# 开发、校验与刷写

## 结构

| 范围 | 入口 |
| --- | --- |
| 固件输入与显示生命周期 | `main/main.c` |
| 纯状态、解析与时序 | `main/quota_logic.c`、`main/quota_logic.h` |
| 网络任务、事件与视图 | `main/quota_service.c` |
| Wi-Fi 驱动 | `main/quota_wifi.c` |
| USB 设置窗口、会话与帧解析 | `main/quota_usb.c`、`main/quota_json.c` |
| 账户目录、控制器、授权/查询与存储 | `main/quota_catalog.c`、`main/quota_portable_service.c`、`main/quota_direct*.c`、`main/quota_store.c` |
| 设置页（热点与 USB 共用） | 源码 `main/setup/`（`page.html`、`style.css`、`app.mjs`、`transport_http.mjs`、`transport_serial.mjs`），生成文件 `main/setup_page.html`；热点服务端 `main/quota_portal.c` |
| 看板与显示资源 | `main/quota_ui.c`（固定对象池）、`main/quota_brand_assets.c`、`assets/`；主机预览 `tools/ui_preview/` |
| 板卡驱动 | `components/bsp/include/`、`components/bsp/src/` |
| 主机检查与固件打包 | `tests/`、`tools/` |

状态和协议计算应能脱离 ESP-IDF/LVGL 测试。固件按键回调投递事件，应用与网络工作任务处理慢操作。非 LVGL 任务访问 UI 须持有 BSP 锁。修改协议、来源数据、刷新或持久化前阅读[应用契约](../applications/ai-quota-monitor.md)；修改驱动或硬件配置前阅读[硬件参考](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)。

## 构建与检查

固件针对 ESP32-C3，使用 ESP-IDF **5.5.3**。网页检查需要 Node.js。激活合适的已有安装；缺少时按照[乐鑫安装指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/index.html)安装该版本。保留其他工具链和用户配置。

```bash
# 先激活 ESP-IDF 5.5.3
source <esp-idf-v5.5.3>/export.sh
idf.py --version
./tools/validate.sh --static    # 仓库/workflow 检查和固件主机测试
./tools/validate.sh --firmware  # 隔离构建和镜像/归档检查
./tools/validate.sh             # 两项固件门禁
```

## 格式与测试结构

C 用 `.clang-format`（LLVM 基础，4 空格，100 列），JS 用根目录 `package.json` 锁定的 prettier，二者随 `--static` 检查。clang-format 固定为 23.1.3（`pipx install clang-format==23.1.3`）。数据数组用 `// clang-format off` 包住。`tools/check_repo.py` 另检查 `main/`、`components/`、`tests/` 的行宽，豁免项写在该脚本里。

主机测试把固件源文件整体编译并链接，不再从源码中抽取函数。固件内部需要给测试看的符号用 `QUOTA_TESTABLE`（`main/quota_testable.h`）标记：固件里是 `static`，测试以 `-DQUOTA_HOST_TEST` 构建时对外可见。`tests/host_sdk/` 提供 ESP-IDF 和 FreeRTOS 的主机替身，以及可被测试覆盖的弱默认实现。`tests/runtime_helpers.py` 的 `compile_and_run(..., host_sdk=True)` 负责接线。仅两项 SDK 测试仍从第三方源码（IDF 的 USB 控制台、按键组件）截取函数，用 `vendor_function`。

## 设备界面预览

`python3 tools/ui_preview/build.py --sheets` 在主机上用真实 LVGL 9.5.0 编译 `main/quota_ui.c`、`main/quota_logic.c`、子集字体和品牌图，按固件的 LVGL 选项（`tools/ui_preview/sdkconfig.h`，取自固件构建的 sdkconfig：RGB565、240 × 20 局部缓冲、30 像素圆角遮罩）渲染 `tools/ui_preview/fixtures.json` 的每个界面。输出在 `build/ui-preview/png/`：每个界面一张 PNG，`--sheets` 另生成拼图 `sheet-N.png`（用 `--scale 2` 放大）。LVGL 取自 `managed_components/lvgl__lvgl`，不存在时（例如 CI）`tools/fetch_lvgl.py` 下载 GitHub 的 v9.5.0 标签包（约 100 MB）并校验固定的 SHA-256，下载结果放在 `build/lvgl/`，CI 用 `actions/cache` 缓存它。GitHub 不发布标签包的校验值，这个 SHA-256 是本仓库自己下载计算的（包内 `src/lv_conf_internal.h` 与锁定组件一致），它固定的是“审阅过的那份包”，不是上游签名；升级 LVGL 须重新计算。新增界面或文案时在 `fixtures.json` 加一条，`tests/test_quota_ui_preview.py` 会断言它渲染成功、全部界面共用同一组对象。

每个界面的输出行带有 LVGL 内存：`used` 是当前占用，`summary` 行的 `max_used` 是峰值。主机池为 64 KB 且指针为 64 位，对象、样式和指针比设备大，对本界面逐块估算的结果是设备约为主机的 0.7 倍，所以设备门限 20 KB 大致对应主机 28 KB：测试以 28 KB 为主机上限，作为回退告警，不是测量值；门限以真机 `lv_mem_monitor().max_used ≤ 20 KB` 为准。每个界面渲染两遍，第二遍不得重绘任何像素（预览程序检查）。调试构建用 `sdkconfig.resource_log` 时，每 30 秒的日志包含 `lvgl_pool: total/used/max_used`，在 LVGL 锁内读取。`sdkconfig.defaults` 关闭了界面用不到的 `CONFIG_LV_USE_SPAN` 和 `CONFIG_LV_USE_THEME_DEFAULT`（所有对象自己设样式，预览逐像素对比渲染未变）。预览不能证明字体在面板上的观感、二维码的可扫性或真实堆余量。改动界面文案后运行 `python3 tools/font_glyphs.py --build` 更新字形表并重新生成两种字体（见[资源](../../assets/README.md)）。

## 资源余量

调试构建加 `sdkconfig.resource_log` 覆盖层，每 30 秒记录一次 `quota_app`、`quota_network` 的栈剩余最小值和堆的当前与历史最小空闲值：

```bash
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.resource_log" idf.py -B <临时目录> build
```

门限：`quota_app` 与 `quota_network` 栈剩余不少于 1024 B，最小空闲堆不少于 16 KB。测量场景为热点会话中有待保存的 TLS、8 个账户连续刷新、ChatGPT 授权全过程。日志数值只有真机才有意义；主机测试构建该路径并检查两个任务和堆都被读取。

## 息屏浅睡眠测量

息屏默认是 POLL 模式的自动浅睡眠。电流须用电池供电并把电流计（PPK2 一类）串在电池线上测，连着 USB 主机时不会进入浅睡眠，USB 电流表无效。分别测 30 秒平均电流：亮屏、旧固件息屏、新固件息屏，数值写入 CHANGELOG。

profiling 构建加 `sdkconfig.profiling` 覆盖层（`CONFIG_PM_PROFILING`、`CONFIG_ESP_TIMER_PROFILING`、`CONFIG_BSP_SLEEP_PROFILE`）。固件在息屏开始时和 60 秒后各把 `esp_pm_dump_locks()` 与 `esp_timer_dump()` 的统计存进内存，息屏期间不打印；亮屏后每 15 秒重复打印这一对快照（最多 20 次）。测量时 USB 必须拔掉，唤醒亮屏后再插上 USB，用 `idf.py monitor --no-reset` 读串口（不加 `--no-reset` 会复位设备，丢掉内存里的快照），把输出存成日志，再用脚本算两次快照的差值：

```bash
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.profiling" idf.py -B <临时目录> build
```

通过标准：用 `python3 tools/pm_profile_delta.py <串口日志>` 得到的差值里，息屏窗口内唯一的周期性 esp_timer 是 `bsp_wake_poll`（约 20 次/秒），LVGL tick 和按键定时器没有触发，浅睡眠时间占比不低于 90%（差值法，不用累计值，开机以来的累计时间会掺入亮屏阶段）；上、下、OK 各唤醒 20 次全部成功，唤醒那次按键不执行任何操作，息屏到亮屏不超过 250 ms（IDF 驱动的 SLPOUT 固定等待 100 ms）；“松开”一律指 ADC ≥1900 mV；连着 USB 主机时不进入浅睡眠且串口不断开；睡眠中背光不漏光、LCD 不闪。主机测试只证明顺序和状态记账，不证明以上任何一项。

迭代时运行聚焦检查。固件交付使用完整门禁。CI 调用同一门禁，不另维护竞争的构建命令序列。仅文档修改需要文档检查，无需刷写无关固件。

受版本管理的默认值来自 `sdkconfig.defaults`、`partitions.csv` 和 `dependencies.lock`。LVGL 固定为 9.5.0，以 `CONFIG_LV_BUILD_EXAMPLES=n` 和 `CONFIG_LV_BUILD_DEMOS=n` 关闭未使用的 examples/demos。依赖锁变化须与组件 manifest 一起审阅。固件门禁在临时目录生成独立 `sdkconfig` 并构建，被忽略的本地设置不会进入该产物。需要本地变体时明确解决配置差异。已有 ccache 时可用 `IDF_CCACHE_ENABLE=1` 复用编译结果。

成功门禁生成 `build/FoloToy-AI-Passport-full.bin`，并将匹配产物存于 `build/firmware/<full-image-sha256>/`。归档包含合并镜像、应用 ELF/MAP/镜像、bootloader、分区表、`flash_args` 及大小/哈希 manifest。校验命令：

```bash
python3 tools/archive_firmware.py verify <bundle-directory>
```

分析崩溃时使用匹配 ELF，后续重构建可能具有不同身份。失败运行可能保留旧输出，必须报告确切成功归档路径和镜像哈希。生成固件和调试归档不提交，也不自动上传。额外自定义分区载荷不会单独保存于此归档。

## 设置页

`main/setup_page.html` 由 `npm run build:setup`（`tools/build_setup_page.mjs`）从 `main/setup/` 生成并提交入库，所以固件构建不需要 Node。改了 `main/setup/` 就重新生成；`tools/check_repo.py` 在生成文件过期时失败。构建脚本还会拒绝 `style=` 属性、`on*=` 事件处理器、`javascript:` 地址和拼进 `innerHTML` 的 `<style>`，因为页面的 meta CSP 只按哈希放行页面自己的一个样式块和一个脚本块。设置页文件不做格式检查（生成文件），源码用 prettier。

`node tools/preview_portable.mjs [--scenario=default|empty|pending|failed|full] [--firmware=3|2|4]` 启动手动预览：输出的 `http://127.0.0.1` 地址模拟热点传输（响应头与设备相同，包括 `frame-ancestors 'none'`），输出的 `file://` 地址模拟 USB 传输，用软件模拟的串口代替 `navigator.serial`。`--firmware=2` 模拟只会说协议 2 的旧固件，`--firmware=4` 模拟更新的固件。模拟设备（`tools/fake_device.mjs`）同时是页面测试 `tests/test_portable_phone.mjs` 的对端。这只是开发预览，不连接设备或服务。

`tests/test_setup_page_browser.mjs` 在真实 Chrome 里加载生成的页面，检查没有控制台错误、没有 CSP 违规、框架中不渲染，并跑一遍 USB 会话。它需要浏览器，所以只在设置了 `CHROME` 环境变量时运行：`CHROME=".../Google Chrome" node tests/test_setup_page_browser.mjs`。

### 发布

推送 `v*` 标签触发 `.github/workflows/pages.yml`：把 `main/setup_page.html` 复制为 `p<协议版本>/index.html` 和最新的 `index.html`，提交到 `gh-pages` 分支并部署到 GitHub Pages。其他协议版本的 `/pN/` 目录原样保留，同一协议的新版本替换自己的目录。步骤在 `tools/publish_setup_page.sh`，`tests/test_publish_setup_page.py` 演练多次发布。首次使用前需要在仓库设置里把 Pages 的来源设为 GitHub Actions，并在 Settings → Environments → `github-pages` 的 Deployment branches and tags 里加一条允许 `v*` 标签的规则（默认只允许默认分支，标签触发的部署会被拒绝）。第一个 `v*` 标签之前，设备界面（P4）的用词和菜单应已更新，页面和 README 才与设备屏幕一致。

## 刷写与 NVS

默认 8 MB 布局：NVS 为 `0x9000`/`0x6000`，PHY data 为 `0xF000`/`0x1000`，factory app 为 `0x10000`/`0x7B0000`，独立 portable NVS 为 `0x7C0000`/`0x40000`；分区表通常位于 `0x8000`。校验器检查配置偏移、边界、不重叠、分区 MD5 和镜像一致性。允许合法自定义布局，以实际产物记录的偏移为准。

写入前识别目标设备和确切已验证产物，确认分区兼容性并说明数据影响。获得适用于该设备、产物和写入范围的授权。其他构建的旧授权不适用于新产物；重构建会改变待交付文件。不要求先读回原固件。

合并 `full.bin` 从 `0x0` 写入时包含填充间隙，可能重置 NVS/PHY 数据，适用于空设备或明确的完整刷新。保留设置时，先确认分区表兼容且写入不覆盖存储数据，再按记录偏移仅写已验证组件镜像。需要保留的数据应使用应用支持的方法保存。不得将 app-only 镜像写在 `0x0`，未经明确授权不得擦除全片。产物校验本身不保证用户数据保留。

升级后首次启动会清理已删除功能的数据，见[变更日志](../CHANGELOG.md#升级数据影响)。清理在读取账户目录时完成，可重复执行，不依赖特殊的刷写步骤。

## 平台边界

| 路线 | 当前边界 |
| --- | --- |
| 手机热点 | 使用能联网的 2.4 GHz 热点，不能假设所有手机都转发企业 WLAN 或开热点时还可加入设备 AP。 |
| 企业 EAP | ESP-IDF/C3 支持企业方法，本项目尚无证书/身份配网 UI；受管理手机凭据不能靠 QR 直接导出。 |
| 原生蓝牙中转 | C3 支持 BLE、不支持 Classic PAN；后续原生隧道需明确 GATT 缓冲、断线/后台处理和设备端到端 TLS。 |
| 网页蓝牙 | 普通网页不是通用 iOS/Android/鸿蒙互联网中转，原生平台集成仍是独立工作。 |

主要参考：[ESP-IDF Wi-Fi 安全](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi-security.html)、[C3 BLE](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/ble/overview.html)、[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)、[WebKit 策略](https://webkit.org/tracking-prevention/)、[鸿蒙 BLE](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-bluetooth-ble)、[Apple 热点](https://support.apple.com/en-ca/guide/security/secfd166f620/web)。

## 验收与报告

授权全程保持原生 USB 串口连接，避免改变调制解调器控制线。重新连接日志工具可能复位 C3，使该轮验收失效。

分开报告 `Build`、`Host tests`、`Device tests` 和 `Unverified`，列出实际完成的检查。浏览器截图和主机模拟不能证明板卡显示、物理 USB 设置或服务授权，也不能证明手机兼容、TLS 内存余量或物理保护。相关检查包括独立/缺失窗口、真实 0%、过期等待、按条件显示 Codex 扩展项，以及重启后恢复来源绑定匹配的扩展缓存；未观测到的扩展在同步前保持隐藏。真机验收还包括字体/按键、手机和电脑设置、Wi-Fi 停止/重连、LCD Sleep In/out、保留刷新截止时间、存储中断、容量、取消/设置/息屏竞态及重启持久化。注明 USB/电池条件并比较亮屏/息屏实测电流，配置和主机测试不能证明电流降低。真实 DeepSeek 凭证与扩展/离线时序仍需要各自验收证据；已发生结果见[变更日志](../CHANGELOG.md)，不能把旧验收自动移用于新构建。
