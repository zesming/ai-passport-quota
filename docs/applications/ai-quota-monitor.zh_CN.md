简体中文 · [English](ai-quota-monitor.md)

# 来源、协议与数据契约

## 服务数据

最多八个账号各自使用 `~/.local/share/ai-passport-quota/profiles/<id>`（或 `AIQ_STATE_DIR`）下的隔离私有 profile，不导入已有 CLI 凭证。删除账号立即解除状态关联；Codex/Claude 会尝试退出授权并保留其 profile 文件。DeepSeek 删除本地密钥，服务侧撤销须另行操作。

| 服务 | 来源 | 含义 |
| --- | --- | --- |
| Codex | 官方 app-server 登录与 `account/rateLimits/read` | Codex 用量；将 300/10080 分钟窗口映射到五小时/七日额度 |
| Claude | 官方订阅登录及正常响应后的 statusline 回调 | 回调绑定到已验证隔离账号；缓存回调保留来源时间 |
| DeepSeek | 私有 API key 调用 `GET https://api.deepseek.com/user/balance` | 可用余额；无验证邮箱、消费历史、请求次数或累计 token 总数 |

额度/余额刷新不发送模型提示。保留 `observed_at`；读取缓存或快照 revision 变化不代表新服务观察。五小时/七日窗口独立处理：缺失窗口保持未知并在两端隐藏，有效 0% 仍显示。重置时间到期后该窗口等待新来源数据，不能自动恢复为 100%。缺失余额不是零。旧数据、离线和过期状态须明确标识。

Codex 从官方选中的限额 bucket 读取 Credits，从账户级 `rateLimitResetCredits.availableCount` 读取储备重置次数。可选快照字段为 `credits: {has_credits, unlimited, balance}` 和 `banked_reset: {available_count}`。Credit 标志为布尔值，balance 为不含控制字符、最多 32 个 UTF-8 字节的来源字符串或 null。不得推断货币单位、换算或合成余额。重置次数为非负安全整数，仅大于零时显示。Credits 仅在 `has_credits` 或 `unlimited` 为 true 时显示；不可用扩展项独立于窗口隐藏。Claude statusline 没有对应扩展项，不得伪造。

DeepSeek 密钥存于仅限当前用户的 `profiles/<id>/deepseek/api-key.json`，不得进入公开状态、设备快照或 USB。金额保留有符号十进制字符串，CNY/USD 独立。当前视图仅选择 CNY `total_balance`，包含赠款和充值，不展开其组成、不换汇、不回退 USD。正常刷新失败保留带状态的缓存，更换密钥则在验证前清除旧钱包。本地标签不是授权邮箱。单请求模型用量不代表账户级平台历史。

来源参考：[Codex 授权](https://learn.chatgpt.com/codex/auth)、[app-server](https://learn.chatgpt.com/codex/app-server)、[Claude 授权](https://code.claude.com/docs/en/authentication)、[statusline](https://code.claude.com/docs/en/statusline)、[DeepSeek 余额](https://api-docs.deepseek.com/api/get-user-balance/)。

## 信任与传输

设置 API 的 4317 端口只监听本机。私有地址的 4318 HTTPS 监听器在配对时启动，或恢复先前已授权配置。配对提供随机 bearer token、带 IP subject alternative name 的 ECDSA 证书及可信时间。固件固定该证书并验证主机；设备 HTTP 不跟随重定向。停止同步撤销令牌，地址变化须重新配对。

Wi-Fi 凭证从浏览器内存直接通过 Web Serial 发送，不经过电脑端 API。固件仅在物理 120 秒窗口内接受配置，已配置设备启动时关闭该窗口。USB 帧以 `@AIQ:` 开始、换行结束，上限 4096 字节。版本 1 的 `configure` 包含八位十六进制 request ID、Wi-Fi 字段、私有 HTTPS base URL、令牌、证书和初始时间。`result` 须匹配 ID，不能回显凭证。USB 打开后立即接收，区分超时/无输入、传输中断、响应不匹配和明确拒绝。

## 设备 API

所有设备请求使用 `Authorization: Bearer <pair-token>`。

| 方法/路径 | 契约 |
| --- | --- |
| `GET /v1/snapshot` | 版本 1、服务时间、revision、设置和已授权账号；最多八个账号、8192 字节 |
| `POST /v1/refresh` | 合并来源刷新请求；接受请求不证明已有新观察 |
| `PATCH /v1/settings` | `refresh_seconds`：60/300/900/1800；布尔 `auto_refresh`；可选 `screen_timeout_seconds`：0/30/60/120/300/600 |

省略息屏时间保留已有设置；旧电脑状态默认 120 秒。旧快照/ACK 缺少该字段时固件保留已存息屏值。仅改息屏时间不重置电脑额度计时器。设置以服务端为准，设备确认后再持久化。

DeepSeek 载荷使用 `provider: "deepseek"`、空 `email`、`plan: "API"`、最多 32 个 UTF-8 字节的 `label`、null 额度窗口及可空 `balance: {is_available, balance_infos}`。最多两项不重复的 CNY/USD 条目包含字符串 `total_balance`、`granted_balance`、`topped_up_balance`，每项最多 20 个十进制字符。旧固件拒绝 DeepSeek，添加前须更新。可执行结构见 `companion/shared/contract.mjs`、`companion/server/protocol.mjs` 和 `main/quota_logic.h`。

## 刷新与屏幕生命周期

电脑端维持独立刷新计划。设备息屏暂停快照/来源/设置 HTTP 及 Wi-Fi 重试/配置，随后停止 Wi-Fi。已获准的有界请求可在工作任务停无线电前完成；显示 generation 防止旧结果发布或继续触发操作。应用、网络和串口任务息屏时等待事件，不再轮询。LCD 暂停刷新，关闭背光并进入 Sleep In。CPU DFS 在息屏时允许 40 MHz，唤醒持有配置最高频率锁。保留 LVGL 5 ms tick 与 ADC 功能键轮询，不使用 MCU light/deep sleep。

唤醒先从 RAM 缓存恢复显示，再重连 Wi-Fi，排入一次静默缓存 GET，即使关闭自动刷新也执行；先等待已获准的有界请求结束，离线则等待连接恢复。该读取不启动服务刷新、不显示进度、不推迟原截止时间。读取后，待执行手动刷新或已启用且到期自动刷新才发送 POST 并显示进度。下一来源间隔从完成时计算。请求准入前取消时保留到期/手动工作；普通失败维持有界节奏。

第一个唤醒功能键手势全部被消耗。配对抑制自动息屏，关闭后重置空闲时间。冷启动时间保持未知，直到本次启动配对或固定证书快照校准。设备时间使用 UTC+8。电量读数缓存三十秒，SOC 填充不推断充电。

## 持久化与显示数据

设备 NVS 保存配对和脱敏快照，快照写入最多每十五分钟一次。保留旧配对/额度缓存布局。设备 Credits 和储备重置扩展项仅存 RAM，按账号 ID 匹配，冷启动后直到来源同步前不显示，不改变 NVS 结构。`screen_to` 为独立息屏键。DeepSeek 的 `balance_cache` 是 CRC 保护的旁路缓存，绑定快照 revision、配置身份与保存时间。不匹配/损坏数据应拒绝，不能为掩盖初始化错误擦除无关 NVS。

设备固定文本使用 `assets/fonts/` 的两种子集字体。任意非 ASCII 账号身份字符回退为 `?`；精确显示需 ASCII 标签或明确扩展覆盖。真机显示、USB 配对、离线恢复、扩展时序和真实来源行为需要[验收检查](../development/README.zh_CN.md#验收与报告)，现有证据记于[变更日志](../CHANGELOG.zh_CN.md)。
