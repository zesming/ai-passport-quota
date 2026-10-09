# 服务来源、协议与数据契约

## 归属与来源

Passport 拥有一份账户目录、Wi-Fi 列表、选择状态和刷新/息屏设置。最多八个账户，每行用逻辑 ID、服务与代数绑定一个设备凭据槽。满额时拒绝新增，不保留隐藏行。手机通过设备热点打开设置页；电脑通过 USB 打开同一个页面（见[设置页](#设置页)）。

| 服务 | 契约 |
| --- | --- |
| ChatGPT（统计 Codex 用量） | 实验性官方客户端设备码流程，HTTPS 查询额度及可选重置详情 |
| DeepSeek | 官方 `GET https://api.deepseek.com/user/balance`，保存后验证 |

刷新不发送模型提示。保留来源 `observed_at`；读取缓存、修订号变化不代表新观察。缺失 5h/周窗口隐藏，真实 0% 可见。重置时间已过则等待来源数据，不自动恢复 100%。倒计时为 `🔄 xd xh`，到期时间为 `xd xh 到期`；不足一小时的正数为 `<1h`。

Codex 设备授权复现[官方客户端实现](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs)，使用可配置 `QUOTA_DIRECT_CODEX_CLIENT_ID` 及固定 `auth.openai.com`/`chatgpt.com` 来源。额度端点属于客户端实现细节，并非稳定第三方 API；本工程未注册 OAuth 集成。直连只映射恰好 18000/604800 秒的主窗口。剩余额度、可用重置次数来自 usage；可选详情失败只让到期未知，保留新额度。剩余额度保留来源字符串，不推测币种。仅在全部可用详情已知时展示最近到期；正数次数仍以来源为准。

DeepSeek 展示原始十进制人民币 `total_balance`，包括赠送/充值但不展开、不换算、不回退美元。别名不是认证邮箱。API 不提供消费历史、请求总数或累计 Tokens。更换密钥后该行回到待验证，余额待新密钥验证通过后更新；验证失败的行显示“验证失败”，修改密钥后重新验证。

主要参考：[Codex 授权](https://learn.chatgpt.com/codex/auth)、[app-server](https://learn.chatgpt.com/codex/app-server)、[DeepSeek 余额](https://api-docs.deepseek.com/api/get-user-balance/)。

## 设置页

设置页是一份静态网页：源码在 `main/setup/`，`tools/build_setup_page.mjs` 把它们内联成一个自包含的 `main/setup_page.html`（提交入库，固件构建不需要 Node）。同一个文件有三种打开方式：Passport 热点（`http://192.168.4.1`）、GitHub Pages（`/` 是最新版本，`/p<协议版本>/` 保留各协议版本的页面）和本地 `file://` 副本。页面不依赖 CDN，不写浏览器存储；Wi-Fi 密码和 DeepSeek 密钥只留在页面内存，直接发给 Passport。

传输由页面地址的协议决定：`http:` 用热点 HTTP，`https:` 和 `file:` 用 USB Web Serial。没有回环地址限制；USB 传输下页面不发起任何网络请求。meta 内容安全策略只放行页面自己的一个 `<style>` 和一个 `<script type="module">`（按哈希），不允许内联 `style=` 属性或事件处理器，构建脚本遇到这些就失败。页面脚本第一行拒绝在框架中运行；设备响应头除重复同一策略外再加 `frame-ancestors 'none'`（meta 写不了这一项）。

页面只有一屏：连接、账户（最多 8 个）、Wi-Fi（最多 3 个，普通 2.4 GHz）、刷新频率与自动息屏、底部的“完成设置”。满额时直接拒绝新增，提示先移除一个。还没有 Wi-Fi 时账户区灰显，Wi-Fi 表单直接展开。每一行有状态：待验证、正常、验证失败。

### 热点设置

实体操作长按 OK → **热点设置**开放十分钟临时 WPA2 热点，每次生成新的热点密码和新的访问码。访问码是 16 位 Crockford Base32（不含 I、L、O、U），显示为 `XXXX-XXXX-XXXX-XXXX`，约 80 bit。请求打开或已打开设置时暂停新的服务 HTTPS，收到的凭据仍完成保存。

| 设备步骤 | 手机操作 |
| --- | --- |
| 1. 热点 | 用 Wi-Fi 二维码或显示的名称、密码连接；提示无互联网时仍保持连接。 |
| 2. 网页 | 连上后扫描网页二维码，地址片段 `#code=` 携带访问码，页面读取后立即从地址栏清除。 |
| 3. 手动输入 | 在浏览器输入 `http://192.168.4.1`，页面提供访问码输入框；大小写、空格和 I/L/O 的混淆会被宽容处理。 |

连续 5 次错误访问码后，热点拒绝所有 API 请求（包括正确的访问码），直到在 Passport 上重新开启热点设置。访问码走请求头 `X-AIQ-Access`，只在内存中比较。热点设置不接受 `validate`；点“完成设置”发送 `setup_close`。Passport 先回复，约半秒后关闭热点，然后做验证，结果显示在设备屏幕上；之后要修改，需要在 Passport 上重新开启热点设置。原因：验证 Wi-Fi 会把无线电切到 STA，热点信道随之改变，手机本来就会掉线。

### USB 设置

顺序是：1 用 USB 线连接；2 在页面点“连接 Passport”，选择串口；3 在 Passport 上打开 USB 设置（实体操作，两分钟）。顺序不能反：浏览器打开串口可能让 C3 重启，会把已经打开的 USB 设置一起关掉，所以页面先开串口，不再等待启动日志，也不重发长超时请求；打开串口后每 1.5 秒发一次 `session_open`（每次最多等 1.5 秒），页面显示“请在 Passport 上打开 USB 设置”，Passport 一打开就自动连上。固件只在 USB 设置打开期间读串口，窗口外的帧没有任何回应。设置时间到期后串口保持打开，页面同样自动等待再次打开。窗口之外固件不读串口，硬件 FIFO 只有 64 字节，主机的写可能阻塞：等待期间页面不会在上一次写未完成时再发新帧，阻塞的写超时也不会关闭串口。设备忙于一次慢速 HTTPS 请求时最多 15 秒不读串口，帧只是被延后，不会丢：读状态没有回应不算故障，状态读的写阻塞也不关闭串口。累计 20 秒以上都没有回应，页面才判断 Passport 可能重启了，回到等待并提示“请在 Passport 上打开 USB 设置”，并用同一个打开者编号重新 `session_open`：设备没重启就原样交还同一个会话，重启过则本来就没有打开者。等待期间收到 `session_busy`（另一个页面刚连接过）时，页面等约 6 秒（设备释放空闲打开者所需）再重试，不会停下。浏览器没有 `navigator.serial` 时页面提示“请用桌面版 Chrome 或 Edge 打开”。`requestPort` 只列出 VID `0x303a`、PID `0x1001`。USB 验证后页面仍可修改：失败的行可直接编辑，再点“完成设置”只验证待验证和失败的行。

### 会话时长

热点基础 10 分钟，USB 基础 2 分钟。每个修改类命令（`network_save`、`network_remove`、`deepseek_save`、`codex_queue`、`account_remove`、`settings_save`、`validate`）被接受后，把剩余时间补到至少 5 分钟，总时长不超过打开后 20 分钟；`state_get`、`refresh`、`reconnect`、`operation_cancel` 和 `setup_close` 不补时。会话仍须在 Passport 上用按键物理打开；每次新窗口新生成会话编号。

### 验证

“验证”由一个固件函数 `quota_portable_validate_pending()` 完成，处理验证状态为待验证或失败的行，顺序是 Wi-Fi → DeepSeek 密钥 → ChatGPT 授权，两个入口调用同一个函数：USB 的 `validate` 命令（会话保持打开，剩余时间补到至少 5 分钟），以及热点 `setup_close` 在关闭热点之后。

- **保存不验证。** `network_save`、`deepseek_save`、`codex_queue` 立刻保存，行状态为 `pending`；只有 `validate`（USB）或 `setup_close`（热点）之后才变为 `ok` 或 `failed`。设备屏幕在有待验证的行时显示“待验证”。
- **Wi-Fi。** 逐个连接，限时 25 秒，成功即选用该网络。密码错误在收到拒绝时立即判定为 `wifi_auth_failed`，其余超时为 `wifi_not_found`。验证状态只保存在内存里，取值 `saved`（已保存）、`ok`、`pending`、`failed`：重启后已保存的网络是 `saved`，既不算待验证也不算失败；设备实际连上所选网络时变为 `ok`，所选网络被拒绝密码时变为 `failed`。修改正在使用的网络（所选的那个）时，新的名称和密码只暂存在内存里，行状态为 `pending`，已存的可用凭据继续保留并继续使用；验证通过才写入存储并选用，验证失败不写入（重启后回到旧凭据）。不是正在使用的网络立刻保存。暂存期间，用旧凭据连上网络不会把这一行改成 `ok`（失败属于新凭据）；暂存的网络不再是正在使用的那个时（例如另一个网络验证通过并被选用），暂存的凭据随同一次保存写入存储，并保留它的验证结果；同一网络之后经普通保存写入、或被移除时，暂存即被清除。设备屏幕可从视图读到每个网络的验证结果和待验证、失败的项数。
- **DeepSeek。** 密钥保存时即写入凭据（认证状态“待验证”，不请求 DeepSeek），验证时请求余额；密钥被拒为 `deepseek_invalid_key`。待验证和失败的密钥不参与后台刷新。换密钥只重置这一行。
- **ChatGPT。** `codex_queue` 只在内存里记下“新账户”或“重新授权”，不占用凭据缓冲；验证走到这一步才启动设备码授权（最多 15 分钟，页面提示“请在 Passport 上完成 ChatGPT 授权”），启动前最多等 45 秒网络和时钟就绪，否则以 `network_unavailable`（或 `time_required`）结束。同一时间只排一个授权（`login_pending`），排队的新账户计入 8 个上限。授权超时（`codex_expired`）、被取消（`canceled`）或网络不可用时，账户不会丢：它留在队列里，状态 `failed`，再次 `codex_queue`（同一账户）或直接再点“完成设置”重来；`account_remove` 才会删除。物理打开热点设置时，尚未开始询问的授权被取消，但排队项保持待验证。
- **进行中。** 验证从 `validate` 被接受起，所有修改类命令、`refresh` 和 `reconnect` 返回 `busy`；`state_get`、`operation_cancel`、`setup_close` 照常。会话到期不会中断验证或授权，它们继续到结束，结果照常写入。验证期间设备不息屏。物理打开热点设置会结束验证，未走到的行保持待验证。

## 传输与命令

USB 使用物理设置窗口及有界 `@AIQ:` 换行帧，协议版本 v3。`session_open` 的结果、`/api/state`、所有 v3 错误结果都带 `protocol:3` 和 `firmware`。帧版本不是 3 一律返回 `unsupported_version`；v2 固件返回的 `unsupported_version` 没有 `protocol` 字段，页面据此提示“Passport 固件过旧，请先升级固件”；`protocol` 比页面更新时提示“设置页版本过旧”并链接 `/p<protocol>/`。版本不一致时页面不显示任何修改控件。Wi-Fi 从浏览器内存直接传 USB。USB 无常驻读取任务。

`session_open` 将首次请求编号绑定到新生成的 32 位十六进制 `session_id`；相同编号重试不延长窗口，窗口未打开时固件不回应，一个静默超过 6 秒的打开者可被新页面接管，接管会重新生成会话编号并使旧页面的会话失效。`state_get` 返回统一的脱敏状态；`command` 包含扁平命令，内外请求编号必须一致。无效或过期会话不返回当前会话编号。入站帧仍为 4096 字节，命令 2048 字节，状态 16384 字节加封装；浏览器接收行上限为 32768 字节。

空闲 USB 窗口保留设备 Wi-Fi 和直接服务请求。进入准备、半帧暂存阻止新请求；暂存资料在 TLS 前清理释放，半帧三秒过期。网页只允许一个尚未确认的修改，最多在同一窗口内按原始内容、编号和时间重试一次，不跨重开窗口或重启自动重放。状态查询不淘汰任务回执。USB 状态额外返回 `login`（授权状态、固定官方授权地址、验证码和脱敏错误）；热点状态不含这一段。

设备服务验证证书包及固定来源。响应最多 32 KiB，完整头部最多 16 KiB。异步传输使用 15 秒进展预算、单次等待最多一秒；不宣称所有 DNS/头部情况下的硬总时限。令牌 POST 在 TLS 握手后、发送头部前预留响应；准入失败不得发送授权。响应开始后释放唯一 POST body，释放外层响应再顺序解析 JWT 元信息，不宣称独立 JWT 签名验证。

热点 API 只接受 AP 接口/子网、精确 Host 和访问码头 `X-AIQ-Access`。修改须同源 Origin，GET 若带 Origin 也须匹配；无 CORS/跨域预检。设备只路由 `GET /`（设置页）、`GET /api/state`（脱敏账户、Wi-Fi、设置、验证状态、任务）和 `POST /api/command`（扁平 JSON、八位小写十六进制 request ID、可选 UTC；最多 2048 字节，拒绝重复/未知字段、非法类型和内嵌 NUL，相同 ID 去重；202 仅代表入队）。命令体的 `v` 为 3。

**命令。** `network_save`（必须带 `ssid`，可带 `network_index` 修改已有的一个）、`network_remove {network_index}`、`deepseek_save`、`codex_queue`、`account_remove`、`settings_save`、`setup_close`、`refresh`、`reconnect`、`operation_cancel`，以及仅 USB 的 `validate`。`mode_select`、`network_scan`、`network_activate`、`account_activate`、`account_deactivate`、`external_import`、`collector_configure` 已删除；`codex_launch` 不再是线上命令，它是验证启动授权的内部步骤。命令内部捕获配置代数：在配置变化之前接受、之后才执行的命令返回 `configuration_changed`，所以网页等每个修改的任务结束后再发下一个。

**状态。** 顶层有 `protocol`、`firmware`、`validating`、`validation_step`（`wifi`/`deepseek`/`chatgpt`）；`network.saved_networks[]` 与 `accounts[]` 各有 `validation`（`pending|ok|failed`）和 `error_code`。`pending_accounts`、`network.pending_network`、`accounts[].source`、`source_changed`、`collector` 已删除。

**错误码。** 网页用中文显示：`wifi_auth_failed`、`wifi_not_found`、`deepseek_invalid_key`、`codex_expired`、`auth_required`、`busy`、`session_expired`（设置时间已到，热点和 USB 统一用这一个）、`account_limit`、`network_limit`、`login_pending`、`access_locked`、`unauthorized`、`serial_busy` 等。公开状态、QR、日志不含服务令牌、PKCE 或 Wi-Fi 密码；临时热点密码和访问码只在实体设置屏显示。

## 持久化与恢复

portable NVS 为 **0x7c0000 / 256 KiB**。model_v2 是一份 CRC blob，含统一配置、最多八个账户描述项（数组保留原有十六项大小，后八项恒为零）及一个 intent。已删除功能的字段（待启用网络、旧电脑端点）改为同样大小的保留字节，必须保持为零；`provider` 取值 0（Codex）和 2（DeepSeek），1 保留不复用，`source` 与 `activity` 只接受 0。单调序号与精确读回决定已应用/未应用/未知，不假设 nvs_commit 提供跨键事务。未知写入保留确切候选，网络与修改等待存储恢复。类型化读取区分缺失、损坏、I/O、内存、忙；仅真正缺失/有效墓碑为空槽。

新增或替换设备授权先存 intent，再存凭据整包，最后提交 model 绑定并清 intent。重启只无网络完成精确有效目标，或在精确前态匹配时清理中断 intent；冲突保留归属，不重放一次性交换。删除设备账户先移除描述项并保存删除 intent，再写精确下一代墓碑、清 intent。代数不回绕。

普通 Codex 续期不改绑定代数：请求前持久化 refresh_inflight，验证后整包替换并清标记。准入前取消可清标记，完整 429 可清标记并退避；已准入的不明超时、重定向、错误、异常成功要求重授权。重启不重放带标记令牌。收到的轮换保留同一个独占凭据缓冲，直到纯存储重试成功，不因息屏/设置/取消丢弃。

observations_v2 每十五分钟最多缓存八个账户观察值，包括可选扩展，绑定逻辑 ID、服务、行代数及精确设备凭据元组；不能提供身份、别名、选择或设置。加载只合并观察字段。

### 升级清理

读取 model_v2 时，通过 magic、版本、长度和 CRC 校验之后、`quota_catalog_valid` 之前，先执行 `quota_catalog_scrub_removed`：删除非 Codex/DeepSeek 服务的行、来自旧电脑来源的行和待启用行，清零待启用网络与旧电脑端点，压缩数组；`selected_account_id` 指向被删行时改为第一个剩余行，没有剩余行时置空；指向被删行的未完成 intent 一并清除。这样单个坏行不会让整个目录失效。

内容有变化时，按以下顺序处理，任何一步失败都会返回存储错误并在下次读取时重试：

1. 把被删行占用的凭据槽改为墓碑（保留代数，令牌清零）。
2. 清零观察缓存中被删行及无效来源/服务的行并重写，只在确有变化时写入。
3. 以 `sequence + 1` 原子提交清理后的目录。

提交放在最后，因为它是唯一能让下次启动不再重复清理的写入；前两步幂等，中途断电后整个过程重新执行并得到相同结果。目录没有变化时不写 Flash。

旧的默认 NVS 分区命名空间 `ai_quota` 和 portable 分区中的旧 `config`、`snapshot` 记录每次启动都会清除（不存在时不写入）。只有这些旧数据的设备按全新设备处理，没有账户和 Wi-Fi。没有账户目录时，所有凭据槽在建立新目录前先改为墓碑，旧密钥不会残留，也不占用账户容量。观察缓存里属于已删除服务或来源的行在加载时被忽略，不影响其余账户的缓存。

CRC/NVS 不提供保密性。Flash 未加密，无 secure-boot、eFuse 或物理读取防护声明。秘密不进 Git；网络/存储/USB 单一负责人，临时工作区按需释放。公共视图锁只覆盖有界复制，不包围 HTTP/NVS/LVGL/JSON 分配。

## 刷新、显示与验证

息屏关闭 AP/Wi-Fi、停止新 HTTP，存储重试与操作时限继续。热点请求/开放窗口及 USB 进入准备、收发暂存立即阻止新阶段；空闲 USB 窗口允许设备联网。亮屏先显示缓存、恢复 Wi-Fi，不推迟统一来源刷新点。手动或到点执行一轮，遵守账户退避，结束后起算下轮。缓存读取不伪造新观察。保存时间仅初始化时钟，设备 TLS 等待本次启动设置校时或 SNTP；显示 UTC+8。

首次完整唤醒操作只亮屏。面板 Sleep In、背光关闭，任务等待事件/截止时间。息屏进入 MCU 自动浅睡眠（POLL 唤醒）：应用先停按键轮询、暂停并停掉 LVGL 定时器和 tick、配置背光/LCD CS 睡眠电平，再释放 `quota_awake` 锁；唤醒时先取锁，再恢复这些状态和面板。息屏期间只有一个 50 ms 一次性采样定时器读按键 ADC，上、下、OK 任一键都能唤醒，唤醒那次按键只亮屏：BSP 丢弃该次手势的按键事件，直到按键状态机报告松开，或 ADC 持续松开（≥1900 mV）超过去抖时间加一个轮询周期（20 ms），最长 3 秒；应用层不再为它另设吞键标志。按键初始化失败时没有任何唤醒来源，息屏只关屏、不进入浅睡眠，并保持亮屏锁。息屏不读电量计，亮屏后立即读一次。USB 主机已连接时不进入浅睡眠。电池 SOC 缓存三十秒，不推断充电。固定中文用子集字体，不支持的账户字符回退 `?`。

主页上下切账户、短 OK 刷新全部、长 OK 设置；子页短 OK 确认、长 OK 返回。授权页短 OK 重新打开 USB 窗口且不取消授权；长 OK 关闭 USB 窗口并取消未发送操作，或从终态返回。统一刷新节奏为手动或 1/5/15/30 分钟；设置/授权只在有效窗口内保持亮屏，不修改从不/30/60/120/300/600 秒息屏设置。

按[验收指南](../development/README.md#验收与报告)分别报告构建、主机/网页、真机/服务及未验证证据。有日期的结果写[变更日志](../CHANGELOG.md)。
