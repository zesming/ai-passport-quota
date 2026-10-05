<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

> 本 Fork 在此记录功能变更、验证和真机验收。根 README 只阐述当前功能、用法及限制。下方保留继承的上游条目；`Unreleased` 不代表已经发布版本。

## Unreleased

### 2026-10-05 — 统一设备账户与设置

取消整套设备／电脑方式切换，改为设备持有一个账户目录，手机和电脑使用同一设置页。各账户独立使用设备凭据或可选电脑采集器。迁移保留已有凭据、选中账户、历史账户和网络；目录满时可明确交换启用账户，不必删除。更换电脑来源需要验证并确认重绑，未知远端账户需明确导入。

新增单记录精确回读的模型和绑定来源的观测缓存。授权意图先于请求准入保存；保存结果不确定时阻止后续请求并保留原候选。已收到的凭据完成存储恢复后才允许其他操作。合并重复电脑工作缓冲、移除常驻 USB 任务、缩小显示缓冲，并在发送授权请求前预留有界 POST 响应空间。旧授权结果不再反复抢占重新打开的设置页。

Host tests：**PASS**，完整静态门禁，含 43 个控制器 ASan/UBSan 场景、来源／存储边界、来源绑定缓存、物理渲染和导航检查。电脑端：**53 项测试和生产构建 PASS**。Browser：**合成数据 PASS**，覆盖不持久化密钥的手动连接、8+8 账户无损启用交换、电脑来源确认重绑、共享设置及窄屏／电脑布局。设置截图已换为共享生产页面。

独立设计与实现复查已关闭发现的实质性问题，包括迁移读写重试、选中账户 ID 碰撞、保存结果不确定时的准备／取消确认、凭据重新授权状态优先于旧缓存，以及断网／息屏的过期截止时间。移除存储故障时会阻止息屏的遗留启动 USB 配对。首次 SDK 构建拒绝含糊的语句排版；仅空白修改通过独立复查和完整静态门禁。

Build：**PASS**，ESP-IDF 5.5.3 完整固件门禁及匹配归档验证。归档 `31c0f267df5ed0cdc655a48263f110ce3a04f54362ddf8528765401b7e4e9a32`，应用 1,758,240 字节，版本 `c7a0544`，ELF SHA-256 `869e9ac8e4fa262c08ca4d0ef7b90695011bde9d4a11536e77de9e92b34968c3`。分区表与此前已安装归档逐字节一致。ELF 静态 DRAM data+BSS 从 125,084 降至 78,708 字节；这不代表运行时 TLS 余量已实测。

Device tests：**未运行**；未发现 Passport USB 设备，未刷写或擦除。待验收：保留资料的迁移／启动、真实 Codex 授权和脱离电脑的额度查询、TLS 堆／DMA／栈最低余量、实体设置密钥可读性及息屏／唤醒／重启恢复。后续仅写入已验证三项组件，保留 NVS／PHY；主机／网页／构建门禁不能证明这些真机结果。

### 2026-10-05 — 单一所有者的凭据记录

下一次真机授权以 `NO_MEMORY` 结束；轮询中最大空闲块为 10 KiB，令牌接收和存储却分别申请约 13 KiB 副本。控制器、来源和 NVS 现共用一份静态、带记录头的凭据缓冲。令牌验证后原地替换，保存时借用直至提交，最后由所有者清空。借用期间其他凭据操作等待。保留版本 1 NVS 布局、身份校验和单次交换规则。响应分配失败只记录容量；阶段变化立即记录，重复间隔为十五秒。

Build 与 Host tests：**PASS**，ESP-IDF 5.5.3 完整检查与交叉复查，包含来源/存储 ASan/UBSan 及四项控制器检查，覆盖借用生命周期、畸形响应不修改旧凭据、只重试存储、调用方管理销毁、旧尾部清理和无大块分配的版本 1 存储。已安装归档 `e8072cc49d41aa2019fa2f46fd3c0163210ecdcd36ef5987ec47643cd7912a97`，应用 1,716,576 字节，版本 `a34222e`，匹配 ELF SHA-256 `e6c603b5724f4027b419cb34c213484c3565df3560e4318d56a910d1598a7435`。Device tests：**三段组件写入哈希校验、启动及获取验证码 PASS**，保留存储资料。轮询稳定在可用堆 37,284 字节、最大块 15,872 字节，网络栈最低余量 668 字节。随后交换失败：`esp-aes` 报分配失败，TLS 读取返回 `-1`；最低可用堆为 656 字节，网络栈余量为 576 字节。这是资源失败，不能说明 Wi-Fi 已断开。授权和额度读取未通过。GitHub 两项检查均通过，暴露了主机覆盖与真实 TLS 资源使用之间的缺口。

### 2026-10-05 — 授权码交换的有界分配

持续采集、不复位时，用户仍停留在授权完成阶段，息屏/唤醒也未恢复。代码检查发现请求内存分配和凭据保存均有静默重试路径。交换表单改为按实际转义长度申请，替代固定预留 13,696 字节。登录诊断区分获取码、轮询、交换和保存，每十五秒以内不重复记录堆/栈测量，不记录凭据内容。保留已接收的凭据，以及不重放结果不明交换的规则。

Build 与 Host tests：**PASS**，ESP-IDF 5.5.3 完整检查和独立复核。针对性 ASan/UBSan 检查覆盖不足 512 字节的短交换、最大转义输入、无效边界及分配失败。已安装归档 `303de36a151a4eff9c90480d3be7af7630b761a2c76a96179a3008e66bd6e4f9`，应用 1,716,352 字节，版本 `0624e66`，匹配 ELF SHA-256 `ecf8a37e47e5c93604d468a0e2e0b3b541e3534ad69c785d340c75cd6dda57e7`。Device tests：**三段组件写入哈希校验与启动 PASS**，保留存储资料。此前持续采集最终记录授权到期，最低可用堆为 1,412 字节，未捕获崩溃；没有进入无限期保存重试。实际交换失败仍需新阶段诊断确认。

### 2026-10-05 — 设备授权的 TLS 内存分配

手机设置恢复后，用户开始 Codex 授权，设备提示失败。串口捕获到 `mbedtls_ssl_setup` 返回分配错误 `-0x7F00`。启用 ESP-IDF 的动态 TLS 记录缓冲，保留 16 KiB 接收/4 KiB 发送上限及证书验证。授权结束日志仅记录结果代码与堆内存测量。

Build 与 Host tests：**PASS**，ESP-IDF 5.5.3 完整检查及独立兼容复核。生成配置保留 IN 16384/OUT 4096 和对端证书，未开启 FREE_CONFIG_DATA/CA；链接映射包含动态 setup/read/write 包装函数。已安装归档 `3c7ac7e5ebf43d6677ad09b5e4a52b40d3fcff0d4afb9754e96b567fda8b20fd`，应用 1,715,600 字节，版本 `8a0d003`，匹配 ELF SHA-256 `a945454167e81f1e24bdb6adbbef19bf32c6650ca2a0db07ee61088f3de16635`。

Device tests：**三段写入哈希校验和启动 PASS**。应用户随后要求，仅擦除旧 NVS（`0x9000`，`0x6000` 字节）和 portable NVS（`0x7c0000`，`0x40000` 字节），模拟新机。两次擦除均完成，保留固件/PHY。全新启动匹配归档，选择 DIRECT 方式、初始化存储并打开手机设置。用户确认手机网页正常打开、Codex 授权码已显示；观察期间未再出现原 TLS 分配错误。GitHub 静态/电脑端与固件检查通过。完成授权、额度读取及最坏情况下的 TLS 内存余量仍待验收。

用户在批准授权后报告白屏重启。下一份串口采集起始记录复位原因 `0x15`（USB UART）、保存位置为 CPU 空闲等待，未捕获 panic。限时采集刚结束并重连，可能改变原生 USB 控制线并复位 C3。证据强烈支持测试工具干扰，不代表授权已经完成。改为同一连接持续被动采集，不写控制线，并请求使用新验证码再次授权。针对性复查未发现令牌接收/持久化路径的明确崩溃缺陷；最坏情况下的栈余量尚未实测。

### 2026-10-05 — 手机设置的双栈 HTTP 兼容

用户连接设备热点、在倒计时有效时扫描第二步二维码，静态网页却返回 `setup_closed`。已安装 ELF 与 IDF/lwIP 源码确认：HTTP 服务使用 IPv6 双栈套接字，IPv4 客户端以 IPv4-mapped IPv6 地址返回，原热点校验只接受 `AF_INET`。

现完整接收套接字地址，将 IPv4 或严格 `::ffff:IPv4` 映射归一化后，保持原热点本地地址/子网限制。继续拒绝原生 IPv6、IPv4-compatible、错误接口与截断地址。错误使用页面既有代码，区分 `session_expired` 与 `unauthorized`。

Build 与 Host tests：**PASS**，ESP-IDF 5.5.3 完整检查，覆盖映射/混合地址类型、套接字错误、拒绝边界及 IPv6 启用/关闭两种主机编译。已安装归档 `f5d4161b2b474884e2e98c4bc59ca463e9515c38dad8c53058f7943b2ebdbef6`，应用 1,711,136 字节，版本 `2bec2e2`，匹配 ELF SHA-256 `172197d98c9cb5ebaf3e03e5db742cf49518430d304eaa7f44a843f9db26b5f3`。Device tests：**三段写入哈希校验、启动、缓存恢复和热点就绪 PASS**；未写入 NVS/PHY。随后用户已从手机网页开始授权；来源授权因上方记录的 TLS 分配错误失败。完整配网及来源验收尚未完成。

### 2026-10-05 — 实验性设备账户与手机设置

新增 DIRECT 方式，由设备保存独立签发的 Codex 设备码凭据并查询用量，以及 DeepSeek 人民币余额；已配对设备保留 COMPANION 默认方式。临时 WPA2 热点和两步屏幕二维码打开设备内嵌手机网页，管理网络、账户和设置；结束设置后由设备恢复联网。Claude 仍使用电脑同步；企业 EAP 和原生 BLE 中转仅完成后续路线调研，尚未实现。

内部设计与实现复核解决 AP 切换、候选网络/密钥回滚、请求去重、静默唤醒周期、缓存身份和一次性令牌处理。两种方式复用原有单一网络任务。收到的新令牌整包原子保存，结果不明的续期不重放。新增 `0x7c0000` portable NVS，保留旧 NVS/PHY。

首次 `aa723541…` 固件启动正常；实测初始化可用堆为 126,536 字节，该阶段尚未启动应用/网络/串口任务和 Wi-Fi。随后复核移除完整 JSON 复制，令牌保存前释放请求/响应，并将 ESP 响应从固定 32 KiB 改为 4 KiB 起步、有界增长。减少可避免的峰值分配，不代表已验证真实 TLS 余量。

Build：**PASS**，最终 ESP-IDF 5.5.3 完整门禁及归档校验，应用 1,710,896 字节。Host tests：**PASS**，包含实际 C 存储、设置接口、控制器与 ESP 传输模拟；ASan/UBSan 解析/来源检查；非 NUL、最大正文、令牌边界、分配失败、轮换/持久化、网络回滚与息屏门控。Browser checks：**合成数据 PASS**，覆盖窄屏、网络/密钥待验证、Codex 切换、到期和模式切换。可选电脑端未改动；52 项测试与构建在 `ad2dab9` 已通过，本次固件改动未在本机重跑。

已安装归档：`build/firmware/8efa9b06f73fb16ed75f7f09d3a1351fe05f74ff487da84724a0f03e0a9052a1/`；匹配 ELF SHA-256 `71ddcc75179377d5f369100ee6ceee686c8e307aef779cc12709cd99eb05f099`，版本 `ad2dab9-dirty`。Device tests：**三段写入、启动和旧缓存恢复 PASS**。`0x0` / `0x8000` / `0x10000` 写入均通过哈希校验；未写 NVS/PHY，未全片擦除。第一次更新分区前确认新增 256 KiB 区域全为擦除值。最终 25 秒启动观察匹配 ELF 前缀、初始化 portable 存储并进入应用就绪，未观察到崩溃标记。原始日志留本地，串口已释放。

GitHub 电脑端测试/构建也已通过。首次 Linux 主机运行发现测试模拟代码的缩进警告和平台特定的时间函数声明；现已模拟时钟写入并明确缩进，三个本机检查通过。此次跟进不改变生产固件。

Unverified：真实 iPhone/Android/HarmonyOS 配网、独立来源授权/续期和关闭电脑后的额度读取，真实 TLS/最大令牌堆余量，新二维码/字体/按键真机回归，扩展离线恢复与实测电流。主机/浏览器证据不替代这些检查。

### 2026-10-05 — 移除电脑自启动；调研便携联网

按用户要求移除已安装的 macOS LaunchAgent、安装命令、自动拉起和配对监听重试计时器，恢复手动启动管理服务，并删除该功能的测试及 README 用法。账号、设置和配对保留，4317／4318 无服务监听。

记录[便携联网方案](development/portable-connectivity.zh_CN.md)：设备保存鉴权并直连 HTTPS、二维码／SoftAP 手机配网、热点备用，以及可选企业 EAP 或原生 BLE 中转。平台授权／支持和真机资源检查仍是前提，此方案尚未实现。

Build：**PASS**，电脑端生产构建通过。Host tests：**PASS**，电脑端 52 项测试及仓库检查通过。Device tests：**未运行**，固件未修改、未刷机。Unverified：设备独立登录／续期、手机配网、企业 Wi-Fi 和 BLE 中转。

### 2026-10-05 — 电脑端启动与网络恢复

息屏唤醒和手动刷新失败的原因是电脑重启后管理服务未运行。重启服务后，配对监听与账户采集恢复，用户确认设备刷新正常。新增当前用户的 macOS 登录后台服务，支持退出自动恢复、安装／移除、重装保留配置及有界启动检查；替换已加载服务时重试 launchd 注销期间的临时错误。plist 不保存凭证，日志仅本地私有保存。

电脑端监听不可用时，每 15 秒重试恢复已保存的配对地址，保留地址、令牌和证书，遵守明确停止；地址变化或证书不可用仍需重新配对。恢复操作在串行队列内检查已有监听，避免重叠重试关闭有效连接的认证。

Build：**PASS，限电脑端**，Vite 生产构建通过。Host tests：**PASS**，电脑端 54 项测试、仓库检查及脚本语法通过；覆盖网络延迟就绪、恢复队列重叠、临时绑定失败、证书不可用、明确停止及关闭。独立复查无剩余实质性问题。已安装的后台服务通过重装和 SIGTERM 后自动恢复检查，账户、设置、CLI 可用性及配对均保留。Device tests：**PASS，限安装后台服务前用户观察到的刷新恢复**。Unverified：新服务随实际注销／登录或重启启动，以及最终后台进程恢复后的真机唤醒。固件未修改，未重新构建或刷机。

### 2026-10-04 — 紧凑倒计时固件已刷入

按用户请求刷入已验证的 `80ce1005…` 归档。设备分区表与归档一致，`0x0` / `0x8000` / `0x10000` 三项组件写入均通过哈希校验，未覆盖 NVS 与 PHY 区域。

Build 和 Host tests：**PASS**，复用该确切归档的完整检查及电脑端 52 项测试。写入前再次验证归档；源码提交 `eff037c` 的 GitHub [静态检查](https://github.com/zesming/ai-passport-quota/actions/runs/37180800306)和[固件检查](https://github.com/zesming/ai-passport-quota/actions/runs/37180800273)均通过。Device tests：**PASS，限写入、启动和认证通信**。30 秒启动采集匹配版本 `7b76eb5-dirty`，ELF 前缀匹配 `55520faeb409259d7d0464ee8529e4e19379e641f35ec02620e0e43e99751622`；确认应用就绪、缓存恢复、电池检测，未见崩溃标记。设备恢复已认证请求，电脑端设置与账户数量保持不变。串口已释放，原始日志仅本地保存。

Unverified：真机刷新图标、倒计时及同步后的紧凑到期行等待用户观察。该归档已成为最新刷入镜像；此前记录保留各自产物当时的验收阶段。

### 2026-10-04 — 紧凑倒计时

额度重置显示为 `🔄 xd xh`，可用重置显示为 `N次 · xd xh 到期`。天／小时取完整小时，剩余不足一小时且尚未到期时显示 `<1h`。两端订阅视图使用一致格式。设备 12 像素字体回退到内置刷新图标。剩余额度保留接口原始字符串；截图改用整数示例，不强制将接口小数取整。

Build：**PASS** — ESP-IDF 5.5.3 完整检查与调试归档验证通过，应用为 1,499,856 字节。Host tests：**PASS** — 固件、运行时及字体检查、电脑端 52 项测试和 Vite 构建通过。浏览器检查通过：双窗口 Codex、没有 5 小时窗口的 Pro、Claude 及文字宽度，示例截图已更新。

已验证归档：`build/firmware/80ce100545ad885e002dd2c16481f38e1a225d0439b4383f75df053a110c51e3/`。匹配 ELF SHA-256：`55520faeb409259d7d0464ee8529e4e19379e641f35ec02620e0e43e99751622`；应用版本 `7b76eb5-dirty`。Device tests：**NOT RUN** — 未刷机，倒计时及回退图标的真机渲染尚未验证，设备仍为 `a3251c5c…`。NVS 布局和刷新行为未变。

### 2026-10-04 — 重置倒计时与到期标注

两端订阅窗口显示距离重置的剩余时间。设备尚未校时则等待同步；已到期窗口仍等待新来源数据。Credits 文案改为「剩余额度」。Codex 请求重置明细，仅在可用明细完整且有效时标注最早到期时间。保留官方次数；缺失或截断明细时隐藏日期，缓存的到期时间已过时等待更新。公开数据不含私有重置明细；设备扩展项仍仅存 RAM，NVS 布局不变。

Build：**PASS** — ESP-IDF 5.5.3 完整检查及归档核验，应用 1,500,944 字节。Host tests：**PASS** — 固件、运行时和字体检查、电脑端 52 项测试，Vite 构建通过。独立复查发现并确认冷启动校时修复。浏览器已验证 Pro、Codex 双窗口、Claude、过期/未知日期、短倒计时及 390 像素布局；合成截图已更新。新版电脑端从真实账号获取了最近到期时间，账户、设置和配对保持有效。

已验证归档：`build/firmware/2696d04b1e14ec43ff9a7941b4ac765e36db8757fcd5ecb12d1a4f824a4d563e/`。匹配 ELF SHA-256：`a6235641c1cfc421f038cbf365837a9a7289633d819475339170987b92cc07be`；应用版本 `a6b43a2-dirty`。Device tests：**NOT RUN** — 此归档尚未刷入，最新已安装镜像仍为 `a3251c5c…`。Unverified：真机倒计时/到期文案及冷启动校时行为。原始账号数据和日志仅留本地。

### 2026-10-04 — 刷入新固件并验收

用户要求后，刷入下方记录的已核验归档 `a3251c5c…`。设备分区表与归档一致，`0x0` / `0x8000` / `0x10000` 三个组件写入均通过哈希校验，未写入 NVS 和 PHY 区域。电脑端服务重启后加载新版采集逻辑，账户、设置和配对保持有效。

Build、Host tests：**PASS**，沿用该确切固件的完整检查；写入前再次通过归档核验。Device tests：**组件写入、启动及用户观察的行为 PASS**。启动版本 `d18d939-dirty` 和 ELF 前缀 `30f7bf01a` 匹配保留的归档；缓存和电量计初始化正常，30 秒启动采集中未观察到崩溃。真实 Codex Credits 和可用重置次数已返回并显示。用户确认息屏、先显示缓存的唤醒、唤醒按键不切换账户，以及 Wi-Fi 恢复连接正常；限时观察也捕获到重连和设备认证请求。

首次新增数据在电脑端服务重启及下一次缓存同步后显示。息屏期间新增数据保留在内存中，断电重启后重新从电脑端获取。Unverified：实测息屏电流或续航改善、长时及离线恢复、请求进行中息屏。原始日志仅留本地。

### 2026-10-04 — Codex 扩展额度、独立窗口与息屏省电控制

两端独立显示订阅窗口：缺失窗口隐藏，真实 0% 仍显示，过期窗口等待新来源数据。Codex 从官方选中 bucket 读取 Credits，读取账户级储备重置可用次数；Credits 仅在可用/不限量时展示，重置次数仅大于零时展示。保留来源余额字符串，不指定货币单位。Claude statusline 无对应扩展项。设备扩展项仅留 RAM，冷启动后同步前不显示，NVS 布局不变。

用户允许息屏关闭 Wi-Fi 后，设备息屏停止 Wi-Fi，向 LCD 发送 Sleep In，暂停显示刷新及应用/网络/串口轮询，允许 CPU DFS 降至 40 MHz。亮屏持有最高频率锁。保留 ADC 功能键检测和 LVGL 5 ms tick，不使用 MCU light/deep sleep。唤醒先恢复缓存显示，再重连并静默读取电脑端缓存，保留来源截止时间，仅实际来源刷新显示进度。

Build：**PASS** — ESP-IDF 5.5.3 完整检查、分段/合并镜像及调试归档校验通过，应用 1,498,352 字节。Host tests：**PASS** — 固件/BSP/运行时检查、电脑端 51 项测试和 Vite 生产构建。独立复核发现并修复布局重叠与旧 Wi-Fi 事件恢复竞态。合成账号浏览器检查覆盖双/单/缺失窗口、真实 0%、Claude 过期数据、不可用/仅 Credits、条件重置次数、账号切换及 216 × 8 比例进度条。文档截图已更新，无私有数据。

已验证归档：`build/firmware/a3251c5c35b719d4465669df606aef3f174fbbe6e49ad6e5b9ad3c942b8a3c35/`；目录名为完整镜像 SHA-256。匹配 ELF SHA-256：`30f7bf01ab7559989809d4a48bfe67de8f9a6910f9554808aac164516ae1c82e`。应用版本：`d18d939-dirty`。Device tests：**NOT RUN** — 没有新增刷机或电流测量。Unverified：真实账号 Codex 扩展数据、真机窗口/扩展布局、Wi-Fi 停止/重连、LCD 睡眠唤醒、按键唤醒时序、离线/活动请求转换，以及实测电流或续航改善。之前设备验收不证明本次变化。

### 2026-10-03 — 项目精简

维护文件从 355 个减少到 135 个。移除未编译的硬件 demo 和像素 UI、未使用的音频/codec 与蓝牙配置及对应测试/stub、重复的技能/文档框架、静态 Sites 打包、上游社区/发布/同步流程。保留实际额度应用、必要 BSP、品牌资源/许可证和有行为价值的测试。移除内容已在本机可恢复的清理目录中保留。

README 仅描述当前使用和截图；简短 agent 约束与统一开发指南指向来源/协议、硬件和资源契约。固件共用刷新间隔校验，去除只写不读的 UI/事件字段。电脑端共享前后端设置和地址规则，统一设置激活动作与公开 HTTP 错误映射，去除死分支。运行时 C 测试共用函数提取/编译逻辑；仓库检查保留链接、双语、Action 固定版本、敏感内容和冲突检查，移除未使用的政策框架。

依赖维持原有版本，LVGL 明确固定为 9.5.0。关闭未使用的 LVGL examples/demos，移除 codec 依赖。最终全新应用镜像为 1,488,912 字节，比当前已刷版本减少 7,296 字节；编译步骤从 1,962 降至 1,547。NVS 格式、分区、额度/余额语义、私有 profile、TLS/USB 保护和息屏唤醒截止时间未变。

Build：**PASS** — ESP-IDF 5.5.3 构建、分段/合并镜像与调试归档校验通过。Host tests：**PASS** — 保留的固件/BSP/仓库/归档测试和电脑端 47 项测试通过，Vite 生产构建通过。独立代码复核无阻塞回归。合成账号浏览器检查确认对话框重置、鼠标/OK 设置入口、比例进度条和仅人民币余额显示正常。

已验证本机归档：`build/firmware/32b4b7debb6394fb34d627c73b59da117f4cac2a01e0d670fbdce4a58c6e75d0/`；目录名是完整镜像 SHA-256。匹配 ELF SHA-256：`2c938c0cd7cb455632f7997260efb69738065175d427099dd69cd8dd87d8b04e`。应用版本：`c094fb1-dirty`。Device tests：**NOT RUN** — 此产物未刷写。Unverified：精简版本的真机回归及下方未完成的真实服务/USB/离线验收。最后刷入的镜像仍为 `6ed4d397…`。

### 2026-10-03 — 刷入修复固件并验收静默唤醒

用户明确要求后，将核验后的确切归档 `6ed4d397dc3a19337873d5c0bc1a7fb86dc663364cd5afad9960916bfcbda50e` 刷入 `/dev/cu.usbmodem1101`。实际分区表匹配 SHA-256 `420931e3f5af072d899b5357c043fd9f283bf165fc9df6113b6d43d82b48a06d`。在 `0x0` / `0x8000` / `0x10000` 写入的三个组件均通过哈希校验。未写入 NVS 和 PHY 区域，没有整片擦除或重新构建。

Build：**PASS**，Host tests：**PASS**，沿用该确切固件的完整检查；写入前再次通过归档核验。源码提交 `4204ee3` 的 GitHub [静态检查](https://github.com/zesming/ai-passport-quota/actions/runs/37114083133)和[固件检查](https://github.com/zesming/ai-passport-quota/actions/runs/37114083130)均通过。

Device tests：**刷写校验、启动、认证通信及已观察的静默唤醒 PASS**。30 秒启动采集确认版本 `1fb05cb-dirty`，启动 ELF 前缀 `9656324ec` 匹配归档 ELF SHA-256 `9656324ecc89a8fe922f4214709336ef5e4937aafa47a2879a5d2e2f563c0d0d`；应用就绪、额度缓存恢复、电量计检测正常。没有观察到 panic、断言、栈保护错误或看门狗异常。电脑端收到刷机后的设备认证请求；配对及已保存的五分钟刷新、息屏设置可用，自动刷新仍开启。按长按下键息屏、等待约 25 秒、功能键唤醒的测试步骤，用户确认刷新点前静默亮屏且账户未切换。串口已释放，私有日志仅留本地。

Unverified：真机长时或离线计时、过期自动刷新，以及被取消请求的恢复。此归档现在为最新刷入镜像；下方早期条目描述各自当时的验证阶段。

### 2026-10-03 — 静默唤醒同步与保留刷新时间

用户确认 `ca0a27c…` 固件的手动息屏、功能键唤醒及唤醒按键抑制正常，但反馈亮屏后显示刷新提示，来源数据看起来没有更新。此前唤醒路径不等待下一个刷新点，始终先发起来源刷新 POST，再读取快照。电脑端的 `202` 确认只排队异步来源任务；数值未变或快照修订号变化，都不能证明来源已重新采集。Claude 刷新继续读取已有状态栏缓存，保留原始来源时间。

唤醒现在立即静默读取电脑端缓存，关闭自动刷新时也执行，不再单独触发来源 POST 或重置来源刷新时间。手动请求和已启用且到点的自动刷新使用已有刷新提示；息屏期间错过的刷新点在唤醒 GET 后补执行。HTTP 尚未接纳就被取消的来源请求，会保留到点时间和已取出的手动请求。普通本地或 HTTP 失败继续遵守原有周期，避免每次工作循环都重试。息屏暂停、过期代次拒绝及离线唤醒恢复保持有效，持久化结构和分区未变。

Build：**PASS** — ESP-IDF 5.5.3 下完整 `./tools/validate.sh` 通过，包括合并镜像检查及调试归档核验。Host tests：**PASS** — 仓库、工作流和全部主机检查通过。实际刷新、快照函数及网络循环测试覆盖静默唤醒、到点前反复唤醒、过期唤醒、慢缓存读取、保留手动请求、接纳前取消、普通失败周期，以及重连和代次门控。取消边界修复后的独立复查没有剩余可操作问题。电脑端源码未变。

核验后的归档为 `build/firmware/6ed4d397dc3a19337873d5c0bc1a7fb86dc663364cd5afad9960916bfcbda50e/`，完整镜像 SHA-256 为 `6ed4d397dc3a19337873d5c0bc1a7fb86dc663364cd5afad9960916bfcbda50e`，匹配 ELF SHA-256 为 `9656324ecc89a8fe922f4214709336ef5e4937aafa47a2879a5d2e2f563c0d0d`。应用大小 1,496,208 字节，合并镜像 1,561,744 字节，构建版本为 `1fb05cb-dirty`。工作区固件在对应源码提交之前构建。固件及原始日志仅留本地。

Device tests：**本次修复 NOT RUN** — 只读发现设备 `/dev/cu.usbmodem1101`，没有打开或重置串口，也没有写入固件。最新已刷入镜像仍为 `ca0a27c…`。Unverified：新固件在来源刷新未到点时不显示唤醒刷新提示、立即同步缓存、到点的自动或手动刷新，以及被取消请求的恢复。

### 2026-10-03 — 真机升级绿色电池固件

用户明确要求刷机后，将此前已验证的 `ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20` 归档刷入 `/dev/cu.usbmodem1101`。写入前确认实际分区表与归档 SHA-256 `420931e3f5af072d899b5357c043fd9f283bf165fc9df6113b6d43d82b48a06d` 一致。在 `0x0` / `0x8000` / `0x10000` 写入并校验组件，未写入 NVS 和 PHY 区域，没有整片擦除或重新构建。原有缓存及配对已恢复。

Build：**PASS**，Host tests：**PASS**，沿用该确切固件的完整检查；写入前再次通过归档校验。代码提交 `17c2ed9` 的 GitHub 静态检查与固件检查均通过。

Device tests：**组件校验和限时启动检查 PASS**。30 秒启动采集确认应用版本 `b39dc02-dirty`、启动 ELF 前缀 `795791a0e` 与保留的 ELF `795791a0eb12d27f486a178ce454a255034470129700a56f541c71848e3a6971` 唯一匹配，应用就绪、缓存恢复、电量计检测正常。没有观察到 panic、断言、栈保护错误或看门狗异常。电脑端收到刷机后的设备认证请求。串口已释放，私有日志仅留本地。

用户随后确认手动息屏、功能键唤醒及唤醒按键抑制正常。反馈的唤醒刷新提示问题由上方尚未刷入的新修复处理。Unverified：绿色图标与去除数字的实际显示、流量暂停/恢复、自动息屏与长时或离线计时。当前保存的刷新、息屏间隔均为五分钟，自动刷新已开启。此归档为最新刷入镜像；下方未刷机的历史条目描述的是各自当时的验证阶段。

### 2026-10-03 — 绿色电池比例进度

用户明确所需效果为绿色电池填充，并保持实际剩余电量比例。固件与网页预览移除可见数字百分比，比例填充使用 `#34C759`，图标与时钟、Wi-Fi 状态对齐。读取不可用时显示斜线，避免误认为实测电量为空。绿色为固定配色，没有推断充电状态或播放循环充电动画。四张文档截图均使用隔离的示例账户重新采集。

重新核查公开的出厂 AI 身份证固件，补充此前仅检查 BSP 的调查。[官方默认固件清单](https://ai-passport.folotoy.cn/assets/firmwares/ai-passport-default/manifest.json)标记版本 `1.2.2`、提交 `5f3f673`、硬件 `v1.0.0`。[应用镜像](https://ai-passport.folotoy.cn/assets/firmwares/ai-passport-default/trae_card.bin)为 2,678,880 字节，SHA-256 为 `d911b86737818b624c5621d2bc7e344e3083cc01a06d6335b0153725349db555`，嵌入 ELF SHA-256 为 `e1a62f0e614256247471be9ae7a81ba58b453e2571fd4b4582491bae97f46ee0`。只读 RISC-V 反汇编已定位电池定时器（`0x4200d3a4`），经过电量采样、EMA 平滑，到渲染器 `0x4200faee` 的路径；该路径按电量档位改变填充与颜色。其额外的 ID ADC 读取函数（`0x42017098`，寄存器 `0x0E–0x0F`）在初始化时用于诊断电压日志，没有用于这条运行时状态栏路径。该路径没有找到经过确认的充电来源。结论仅适用于已检查的镜像，不代表所有出厂固件或硬件版本；用户原先观察到的出厂提示尚未与确切构建对应。研究文件与原始日志均未进入 Git。

Build：**PASS** — ESP-IDF 5.5.3 完整 `./tools/validate.sh` 与调试归档验证通过。Host tests：**PASS** — 仓库、固件全部主机检查，以及电脑端 47 项测试和生产构建通过。浏览器确认主页、设置、DeepSeek、配对预览正常；示例电量为 76% 时，网页填充准确占轨道的 76%，显示绿色且无可见电池数字。截图为网页模拟展示，不代表真机验收。

已验证本地归档为 `build/firmware/ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20/`，完整镜像 SHA-256 为 `ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20`，ELF SHA-256 为 `795791a0eb12d27f486a178ce454a255034470129700a56f541c71848e3a6971`。应用为 1,496,512 字节，合并镜像为 1,562,048 字节，工作区构建版本为 `b39dc02-dirty`。固件仅保留本地，未提交。

Device tests：**NOT RUN** — 按用户要求没有硬件、串口访问或刷机。Unverified：新图标的真机渲染、此前息屏同步功能，以及未来设计需要时的真实充电状态来源。最后刷入镜像仍为 `c0eb0a89…`。

### 2026-10-03 — 按息屏状态控制同步

设备息屏时暂停快照轮询、来源刷新、设置 HTTP 请求和主动 Wi-Fi 重试及配置应用，保留缓存、配对信息及功能键检测。唤醒后立即排队一次合并的刷新与快照同步，即使关闭自动刷新也执行；先结束已接纳的限时请求，离线唤醒保留到连接恢复。随后从唤醒同步周期恢复正常轮询和已启用的自动刷新。电脑端保留独立的定时刷新。已接纳的 Wi-Fi 初始化允许完成；HTTP 请求保持限时，显示状态切换代数用于阻止息屏或快速息屏/唤醒后的旧结果和后续请求。

充电状态检查：现有 CW2017/BSP 提供电量和电压，板级引脚表没有充电状态输入，也没有随项目提供的原理图。[官方充电说明](https://ai-passport.folotoy.cn/en/guides/getting-started/)使用绿色指示灯。真实充电动画仍需要有明确资料、芯片可读的 CHG/STAT 信号；未用 USB 连接、电量上涨或电压变化替代真实充电状态。

用户要求本轮**只开发验证、不刷机**。本轮不连接设备、不写入固件；之前的真机观察不能用于验证这次新增的息屏同步行为。

#### 验证

构建：**通过** — ESP-IDF 5.5.3 下完整 `./tools/validate.sh` 通过，包括合并镜像和调试归档。主机测试：**通过** — 仓库/工作流检查和全部主机测试通过，四项运行时测试直接执行实际 HTTP 准入、快照发布、刷新/设置函数及网络循环。覆盖息屏暂停、关闭自动刷新后唤醒、离线恢复、快速断线/重连、保留被取消的 GET、过期代次、设置延后、来源时间、缺失窗口、按完成时间恢复周期及 135 个非 ASCII 字形。新增取快照前重连的回归测试在修复前失败、修复后通过；最终独立复查没有剩余可操作问题。代码提交 `f557281` 的 GitHub 静态检查和固件检查均通过。本地工作区固件在该代码提交之前构建。电脑端源码未变，之前的验证记录见下文。

核验后的本地归档为 `build/firmware/2a795a6280ca8f8b74bfa02966a03d4c8bb72e0c782f5ebdfb1789e49af261ac/`，合并镜像 SHA-256 为 `2a795a6280ca8f8b74bfa02966a03d4c8bb72e0c782f5ebdfb1789e49af261ac`，ELF SHA-256 为 `c420668eea7f7f393ea267fb20c0aa65bd687046b9f3fea4dda681d3db038f8c`。应用大小 1,496,576 字节，合并镜像 1,562,112 字节；工作区构建报告版本 `3c823a3-dirty`。固件和原始日志仅留本地，未提交。

真机测试：**未运行** — 未访问硬件或串口，未刷机。上次刷入的仍为 `c0eb0a89…` 归档。未验证：新固件真机上的息屏/唤醒流量暂停、离线恢复、刷新计时、动态内存和耗电，以及真实硬件充电状态接口。充电动画需求仍等待该接口，尚未实现。

### 2026-10-02 — AI Passport Quota

将固件和本地电脑端应用整理到用户的私有仓库，加入隔离示例截图，说明账户连接、USB 配置及开发入口。新增息屏控制和状态图标，修正刷新状态及额度预览，接入 DeepSeek 并只显示一项人民币可用余额。

核对 DeepSeek 官方 API 文档：[余额接口](https://api-docs.deepseek.com/api/get-user-balance/)支持可用余额，未找到公开文档支持的账户累计/区间消费、请求次数或历史 Tokens 总量接口。[模型响应中的用量](https://api-docs.deepseek.com/api/create-chat-completion/)只对应单次请求。官网统计等待验证数据来源后接入。取消两端赠送/充值明细展示，保留来源数据兼容性。

#### 验证与真机验收

电脑端验证：**47/47 项测试通过**，正式构建通过。网页检查确认只显示一项人民币可用余额、不展开赠送/充值明细，正确的额度填充比例、正式服务的设备状态保持未知、息屏时间保存。文档截图使用隔离示例。

代码提交 `e0b56c8` 的 GitHub 静态检查和固件检查均通过。

**2026-10-02 固件验证状态**：完整检查通过，包括 ESP-IDF 构建、合并镜像/调试归档核验、全部主机检查、刷新状态测试、NVS 兼容测试和 135 个非 ASCII 字形覆盖。新增息屏、显示状态和 DeepSeek 单一人民币余额的固件**已刷入并检查启动**，用户确认主页、长按下键息屏和功能键唤醒均正常。完整功能验收仍待完成。DeepSeek 尚未使用真实密钥及响应测试；余额测试使用模拟响应。

当前已刷入的调试归档为 `build/firmware/c0eb0a8988a819ae43a2cb4b3e388747e14e79c6a667d7916a2fb73a04123de4/`，ELF SHA-256 为 `4a87f07e1535d9ea084a811aeff055dbcf8fdc4ea3e5fb4ae2cee8ff42e18a8f`。该产物由工作区（`5ef51a1-dirty`）构建，随后代码提交为 `e0b56c8`。2026-10-02 核对实机分区表与归档一致，按 `0x0` / `0x8000` / `0x10000` 分段写入并校验引导程序、分区表和应用，保留 NVS 与 PHY 区域，没有全片擦除。30 秒启动观察匹配应用版本和启动日志中的 ELF 前缀，确认显示初始化、应用就绪、额度缓存恢复，未发现 panic/看门狗重启。随后电脑端收到已认证的 HTTPS 同步请求。初次 Wi-Fi 连接有一条正在连接提示，之后同步成功。原始日志仅本地留存，串口已释放。

此前真机观察确认初始配对页中文清晰、两分钟窗口关闭正常、启动未见崩溃。一个真实 Codex 账户已提供周额度，其数据源没有提供 5 小时窗口。

上述早期真机观察使用整理仓库之前的完整镜像 `690229c2dca7ec2ee0a794d1c4f8625468e4feedab54507b7c3cae0448b1af7c`，匹配 ELF SHA-256 为 `15a33663935734a7df19fd553db87e461f53d1a22e2984973342ca8df3bc0858`。本次升级观察对应上面记录的新归档。分析崩溃前必须匹配实际刷入镜像的 ELF。

**仍需验收 USB 配对**：用户报告过配对窗口内仍出现 15 秒超时。网页已改为打开后立即接收、有限等待启动、4096 字节数据流缓冲和具体错误提示；主机检查通过，实际重试尚未确认。本次升级恢复已有配对配置，不能证明浏览器重新配对可用。原生串口发送短帧和实际配置长度的安全无效帧均得到匹配编号的确认。打开串口监视器可能重启设备，避免同时占用串口，区分重启与配对超时。

本次增加可设置的自动息屏、完整唤醒按键抑制、设备真实 Wi-Fi/电池图标、本次启动校时后显示的时钟、订阅名称展示首字母大写、黑白 OpenAI Logo 和 DeepSeek 余额。后台读取缓存不再提示正在刷新。DeepSeek 需要本次新固件，旧固件会拒绝该平台。

网页低额度/危险额度预览曾把整块数字背景着色，现已修正并在浏览器检查。保持数字背景透明，进度条只填充剩余比例；不要把模拟数据流检查或网页预览当成真机验收。

本次升级已确认初次 Wi-Fi/HTTPS 通信、额度缓存恢复、主页显示、手动息屏和功能键唤醒正常。后续验证浏览器重新配对、Claude 授权与真实额度、其余页面/按键/Logo、设置双向同步、持续断网及重启缓存行为、按时间自动息屏、DeepSeek 余额、TLS 动态内存。未知或重置过期窗口保持未知，保留来源时间，不伪造新额度。

### 继承的上游条目

- 加入厂家为优特利 520mAh 电芯生成的 80 字节 CW2017 profile，并实现内容与更新标志检查、写入后校验、规定的重启时序以及有上限的 SOC 就绪等待。

- 扩充环境引导文档：新增乐鑫 Git 服务镜像（`git.espressif.com.cn`）作为中国大陆首选线路，覆盖 ESP-IDF v5.5.3 及其子模块；补充子模块长等待/超时处理、原地修复，以及 `esp32-wifi-lib` 等大仓的按钉死 commit 浅取；提示按仓库残留的 Jihulab `insteadOf` 旧配置；并把官方离线 release 压缩包加入兜底方案（经验来自 `esp-mosaico/esp-mosaico-vibe`）。

- 按功能域整理文档并采用双入口：根目录 `AGENTS.md` 变为薄路由（只保留硬约束与任务路由），详细的 AI 开发工作流下沉到 `docs/development/ai-guide.md`，`agent-guide.md` 并入其中。为 `docs/development/` 增加二级分区（`engineering/`、`ci/`、`release/`），把 `plays/` 应用档案与 `experiences/` 移入带专属 README 的 `docs/reference/` 参考区；删除 `docs/software-design/`（空脚手架）；把 `assets/{fonts,images,music}/README` 三个叶子 README 并入 `assets/` README；把 `project-completion` 的六个子文档压平为单文件；并把每个目录统一为单一 README，消除所有 `INDEX` 文件与一处重复经验索引。所有交叉引用与文献链接已更新；未丢弃任何内容。

- 删除位于 `0x700000` 的旧 app/test 分区，以及相关的 bootloader、校验和
  文档要求；固定的 `cardid` 保护分区及其 CI 校验保持不变。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。
