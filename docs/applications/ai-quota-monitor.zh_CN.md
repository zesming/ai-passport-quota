简体中文 · [English](ai-quota-monitor.md)

# 来源、协议与数据契约

## 模式与归属

| 模式 | 账号与刷新归属 | 支持来源 |
| --- | --- | --- |
| `COMPANION` | 本地电脑伴侣端；设备读取其已认证缓存 | Codex app-server、Claude statusline、DeepSeek 余额 |
| `DIRECT` | 设备；手机辅助设置与服务授权 | 实验性 Codex 设备授权、DeepSeek 余额；无原生 Claude 授权 |

模式须明确选择。已有 USB 配对的设备默认 `COMPANION`，无旧配置的设备默认 `DIRECT`。凭证和缓存相互独立，切换模式不导入 CLI 凭证，也不复用电脑的 refresh-token 链。每种模式最多八个账号。手机界面与按键操作见[便携连接设计](../development/portable-connectivity.zh_CN.md)。

## 服务数据

在 `COMPANION` 中，最多八个账号各自使用 `~/.local/share/ai-passport-quota/profiles/<id>`（或 `AIQ_STATE_DIR`）下的隔离私有 profile，不导入已有 CLI 凭证。删除账号立即解除状态关联；Codex/Claude 会尝试退出授权并保留其 profile 文件。DeepSeek 删除本地密钥，服务侧撤销须另行操作。

| 服务 | 来源 | 含义 |
| --- | --- | --- |
| Codex | 官方 app-server 登录与 `account/rateLimits/read` | Codex 用量；将 300/10080 分钟窗口映射到五小时/七日额度 |
| Claude | 官方订阅登录及正常响应后的 statusline 回调 | 回调绑定到已验证隔离账号；缓存回调保留来源时间 |
| DeepSeek | 私有 API key 调用 `GET https://api.deepseek.com/user/balance` | 可用余额；无验证邮箱、消费历史、请求次数或累计 token 总数 |

在 `DIRECT` 中，Codex 实现[官方设备码流程](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs)，随后使用 access token 和 `ChatGPT-Account-ID` 读取 `https://chatgpt.com/backend-api/wham/usage` 及 `/wham/rate-limit-reset-credits`。这些路径是官方客户端实现细节，不是稳定的第三方公开 API。可配置的 `QUOTA_DIRECT_CODEX_CLIENT_ID` 默认使用官方 Codex client ID `app_EMoamEEZ73f0CkXaXp7hrann`，并非为本产品注册的 OAuth 客户端。账号/工作区开启设备码登录、使用该 client 的许可及真机兼容性，仍是接受这项实验性集成的前提。手机在 `https://auth.openai.com/codex/device` 登录并输入设备显示的验证码，不使用浏览器回调到设备。直连 DeepSeek 使用同一公开余额接口。Claude 仅支持 `COMPANION`。

额度/余额刷新不发送模型提示。保留 `observed_at`；读取缓存或快照 revision 变化不代表新服务观察。五小时/七日窗口独立处理：缺失窗口保持未知并在两端隐藏，有效 0% 仍显示。重置时间显示为「🔄 xd xh」，可用重置到期显示为「xd xh 到期」。不足一小时显示「<1h」，天/小时采用已满小时数。到期后该窗口等待新来源数据，不能自动恢复为 100%。缺失余额不是零。旧数据、离线和过期状态须明确标识。

在 `COMPANION` 中，Codex 从官方选中的限额 bucket 读取 Credits，从账户级 `rateLimitResetCredits.availableCount` 读取储备重置次数。可选快照字段为 `credits: {has_credits, unlimited, balance}` 和 `banked_reset: {available_count, next_expires_at}`。Credit 标志为布尔值，balance 为不含控制字符、最多 32 个 UTF-8 字节的来源字符串或 null。不得推断货币单位、换算或合成余额。重置次数为非负安全整数，仅大于零时显示。Credits 显示为「剩余额度」，仅在 `has_credits` 或 `unlimited` 为 true 时显示；不可用扩展项独立于窗口隐藏。用 `excludeResetCreditDetails: false` 请求重置明细。仅当 `available_count` 条明细全部为可用 Codex 重置且到期值有效或明确为 null 时，将最早的有限到期时间写入 `next_expires_at`；缺失、截断或无效明细时为 null。保留官方次数，不公开私有明细；缓存的到期时间已过时显示「等待更新」。在 `DIRECT` 中，仅 18000/604800 秒的主限额窗口映射到五小时/七日，额外限额不能替代它们。用量响应提供 Credits 与权威重置次数摘要；可选重置明细读取失败时，保留新的用量/次数，到期时间保持未知。Claude statusline 没有对应扩展项，不得伪造。

在 `COMPANION` 中，DeepSeek 密钥存于仅限当前用户的 `profiles/<id>/deepseek/api-key.json`，不得进入公开状态、设备快照或 USB。金额保留有符号十进制字符串，CNY/USD 独立。当前视图仅选择 CNY `total_balance`，包含赠款和充值，不展开其组成、不换汇、不回退 USD。正常刷新失败保留带状态的缓存，更换密钥则在验证前清除旧钱包。本地标签不是授权邮箱。单请求模型用量不代表账户级平台历史。

在 `DIRECT` 中，DeepSeek 密钥归设备持有，设置 API 不回传密钥。新密钥可先存为待验证状态；收到配置不代表认证成功。替换已有密钥时使用候选值，经 station Wi-Fi 验证成功后，才提交密钥、身份 generation 和余额。失败保留原凭证与钱包。仅修改标签无需新增来源观察。

来源参考：[Codex 授权](https://learn.chatgpt.com/codex/auth)、[app-server](https://learn.chatgpt.com/codex/app-server)、[Claude 授权](https://code.claude.com/docs/en/authentication)、[statusline](https://code.claude.com/docs/en/statusline)、[DeepSeek 余额](https://api-docs.deepseek.com/api/get-user-balance/)。

## 信任与传输

在 `COMPANION` 中，设置 API 的 4317 端口只监听本机。私有地址的 4318 HTTPS 监听器在配对时启动，或恢复先前已授权配置。配对提供随机 bearer token、带 IP subject alternative name 的 ECDSA 证书及可信时间。固件固定该证书并验证主机；设备 HTTP 不跟随重定向。停止同步撤销令牌，地址变化须重新配对。

Wi-Fi 凭证从浏览器内存直接通过 Web Serial 发送，不经过电脑端 API。固件仅在物理 120 秒窗口内接受配置，已配置设备启动时关闭该窗口。USB 帧以 `@AIQ:` 开始、换行结束，上限 4096 字节。版本 1 的 `configure` 包含八位十六进制 request ID、Wi-Fi 字段、私有 HTTPS base URL、令牌、证书和初始时间。`result` 须匹配 ID，不能回显凭证。USB 打开后立即接收，区分超时/无输入、传输中断、响应不匹配和明确拒绝。

在 `DIRECT` 中，服务请求使用证书包验证、主机名检查和固定 HTTPS 来源（`auth.openai.com`、`chatgpt.com`、`api.deepseek.com`），拒绝重定向。HTTP 响应体上限为 32 KiB，完整响应头累计上限为 16 KiB。同一任务的异步传输检查 15 秒进度预算，每次传输等待最多一秒；这不是对所有 DNS/响应头行为的已测绝对总时长保证。凭证不得进入公开状态、额度快照、日志或二维码。从已验证 HTTPS 令牌响应解析 JWT 元数据，用于账号/用户匹配和到期判断；这不表示验证了 JWT 签名。

手机设置页使用物理 600 秒窗口内的临时 WPA2 AP。第一张二维码连接 Wi-Fi，第二张打开 `http://192.168.4.1/#s=<session-secret>`。fragment secret 进入页面内存后由 `replaceState` 移除，不存入浏览器存储。本地页面使用 AP 会话保护下的 HTTP，服务请求使用已验证 HTTPS。完成、超时或息屏时关闭 AP。Codex 启动先关闭 AP，再经 station 网络授权；手机须恢复互联网连接，才能批准官方验证码。授权等待上限为 15 分钟，轮询间隔有界，不使用浏览器回调。

## COMPANION 设备 API

所有设备请求使用 `Authorization: Bearer <pair-token>`。

| 方法/路径 | 契约 |
| --- | --- |
| `GET /v1/snapshot` | 版本 1、服务时间、revision、设置和已授权账号；最多八个账号、8192 字节 |
| `POST /v1/refresh` | 合并来源刷新请求；接受请求不证明已有新观察 |
| `PATCH /v1/settings` | `refresh_seconds`：60/300/900/1800；布尔 `auto_refresh`；可选 `screen_timeout_seconds`：0/30/60/120/300/600 |

省略息屏时间保留已有设置；旧电脑状态默认 120 秒。旧快照/ACK 缺少该字段时固件保留已存息屏值。仅改息屏时间不重置电脑额度计时器。设置以服务端为准，设备确认后再持久化。

DeepSeek 载荷使用 `provider: "deepseek"`、空 `email`、`plan: "API"`、最多 32 个 UTF-8 字节的 `label`、null 额度窗口及可空 `balance: {is_available, balance_infos}`。最多两项不重复的 CNY/USD 条目包含字符串 `total_balance`、`granted_balance`、`topped_up_balance`，每项最多 20 个十进制字符。旧固件拒绝 DeepSeek，添加前须更新。可执行结构见 `companion/shared/contract.mjs`、`companion/server/protocol.mjs` 和 `main/quota_logic.h`。

## 本地手机设置 API

这些接口仅在物理 AP 会话内存在，与伴侣端 `/v1` 接口独立。

| 方法/路径 | 契约 |
| --- | --- |
| `GET /` | 内嵌生产设置页，无凭证状态 |
| `GET /api/state` | 脱敏模式、设置、网络/账号状态及有界任务结果；要求 `X-AIQ-Setup` |
| `POST /api/command` | 带 request ID 的有界命令；要求 `X-AIQ-Setup` 和同源 `Origin`；接受表示排队，不是验证完成 |

所有请求须从 AP 子网访问 AP 接口，`Host` 必须精确为 `192.168.4.1` 或 `192.168.4.1:80`。GET 若带 `Origin`，必须匹配本地来源；修改请求必须提供它。外部/null 来源和跨域预检均拒绝。命令包含已存网络选择、设置/模式修改、DeepSeek 验证、Codex 排队/启动、删除账号及刷新/重连。重复 request ID 必须携带相同载荷。公开结果不得回显 Wi-Fi 密码、服务令牌、私有授权 ID 或 PKCE 值。参见 `main/quota_portal.c`、`main/quota_portable.h` 和[手机流程](../development/portable-connectivity.zh_CN.md)。

## 刷新与屏幕生命周期

在 `COMPANION` 中，电脑端维持独立刷新计划。两种模式下，设备息屏暂停快照/来源/设置 HTTP 及 Wi-Fi 重试/配置，随后停止 Wi-Fi。已获准的有界请求可在工作任务停无线电前完成；显示 generation 防止旧结果发布或继续触发操作。应用、网络和串口任务息屏时等待事件，不再轮询。LCD 暂停刷新，关闭背光并进入 Sleep In。CPU DFS 在息屏时允许 40 MHz，唤醒持有配置最高频率锁。保留 LVGL 5 ms tick 与 ADC 功能键轮询，不使用 MCU light/deep sleep。

在 `COMPANION` 中，唤醒先从 RAM 缓存恢复显示，再重连 Wi-Fi，排入一次静默缓存 GET，即使关闭自动刷新也执行；先等待已获准的有界请求结束，离线则等待连接恢复。该读取不启动服务刷新、不显示进度、不推迟原截止时间。读取后，待执行手动刷新或已启用且到期自动刷新才发送 POST 并显示进度。下一来源间隔从完成时计算。请求准入前取消时保留到期/手动工作；普通失败维持有界节奏。

在 `DIRECT` 中，唤醒先显示本模式缓存并重连，不发送伴侣缓存 GET，也不重置来源截止时间。仅待执行手动请求或已启用且到期的刷新读取来源。一个网络任务统一管理两种模式、授权、存储修改和 HTTP；直连使用一个全局刷新轮次，不为每个账号单设计时器。轮次完成后计算下一间隔。各账号遵守自身 429 退避，普通失败保留带状态的缓存。Codex 在 access token 即将到期前刷新，或收到 401 后仅刷新一次，再最多重试一次额度读取。DeepSeek 验证及登录完成可独立于自动刷新设置请求来源刷新。

即使屏幕已休眠，收到的新令牌也必须完成提交。待持久化状态只重试存储，不重复令牌 POST；取消/删除须等待提交。手动息屏暂停授权联网，截止时间继续计算。设置和待授权状态临时抑制自动息屏，不改变已存息屏时间。

第一个唤醒功能键手势全部被消耗。配对抑制自动息屏，关闭后重置空闲时间。在 `COMPANION` 中，冷启动时间保持未知，直到本次启动配对或固定证书快照校准。在 `DIRECT` 中，已存时间可初始化时钟，但服务 HTTP 须等待本次启动的手机时间或清醒期间 SNTP 同步；已存时间本身不算本次启动校准。来源观察时间优先采用 HTTPS 响应 Date，回退到已校准 UTC 时钟。设备时间使用 UTC+8。电量读数缓存三十秒，SOC 填充不推断充电。

## 持久化与显示数据

在 `COMPANION` 中，设备 NVS 保存配对和脱敏快照，快照写入最多每十五分钟一次。保留旧配对/额度缓存布局。设备 Credits 和储备重置扩展项仅存 RAM，按账号 ID 匹配，冷启动后直到来源同步前不显示，不改变旧 NVS 结构。`screen_to` 为独立息屏键。DeepSeek 的 `balance_cache` 是 CRC 保护的旁路缓存，绑定快照 revision、配置身份与保存时间。不匹配/损坏数据应拒绝，不能为掩盖初始化错误擦除无关 NVS。

`DIRECT` 使用位于 `0x7c0000`、独立的 256 KiB `portable` NVS 分区；旧 NVS/PHY 偏移不变，factory 应用分区在它之前结束。配置、凭证和快照使用带版本与 CRC 校验的记录，拒绝无效数据。每个账号以一个 blob 保存逻辑 ID、服务身份、generation、令牌/密钥、到期时间和 `refresh_inflight`。刷新 POST 前先提交该标记；验证响应后，原子替换同一 blob 并清除标记。HTTP 请求头发出前的连接失败或取消可安全清除标记；发出后，完整 HTTP 429 可清除标记并退避，超时、重定向、服务端错误或畸形成功响应意味着令牌链不确定，须重新授权。启动时标记仍在，不得重放旧令牌。新令牌存储失败时保留在 RAM，直到只重试存储成功。服务账号 ID 与逻辑显示 ID 独立。

直连脱敏缓存与伴侣缓存独立，绑定服务/账号 ID 和凭证 generation，最多每十五分钟保存一次。删除/替换账号不能继承旧 generation 的钱包或额度。两种模式的 Codex Credits/重置扩展项仍仅存 RAM，冷启动后直到来源同步前不显示。便携分区**未加密**；CRC 和 NVS 提交机制用于损坏/提交检查，不保证保密或阻止闪存读取。不声明 secure boot、flash encryption 或 eFuse 保护。

网络任务使用一份带记录头的凭据缓冲，供登录、密钥验证和轮询复用。待保存状态借用同一缓冲，必须保持有效且不改写，直至存储完成。其他凭据操作等待，配置/缓存使用独立记录。令牌验证后原地替换，NVS 直接写同一版本 1 记录，不复制整份凭据。

设备固定文本使用 `assets/fonts/` 的两种子集字体。任意非 ASCII 账号身份字符回退为 `?`；精确显示需 ASCII 标签或明确扩展覆盖。真机显示、USB 配对、离线恢复、扩展时序和真实来源行为需要[验收检查](../development/README.zh_CN.md#验收与报告)，现有证据记于[变更日志](../CHANGELOG.zh_CN.md)。
