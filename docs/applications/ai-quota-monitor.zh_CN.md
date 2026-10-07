简体中文 · [English](ai-quota-monitor.md)

# 服务来源、协议与数据契约

## 归属与来源

Passport 拥有一份账户目录、Wi-Fi 列表、选择状态和刷新/息屏设置。最多八个启用账户，另外八个历史描述项可保留待启用。每行用逻辑 ID、服务来源与代数绑定设备凭据槽或电脑端点代数/远端 ID。启停保留凭据，更换来源须明确授权，不按邮箱合并。手机和电脑使用同一[设备设置页](../development/portable-connectivity.zh_CN.md)。

| 来源 | 服务 | 契约 |
| --- | --- | --- |
| 设备 | Codex | 实验性官方客户端设备码流程，HTTPS 查询额度及可选重置详情 |
| 设备 | DeepSeek | 官方 `GET https://api.deepseek.com/user/balance`，验证后替换密钥 |
| 可选电脑采集器 | Codex | 官方 app-server 登录及 `account/rateLimits/read` |
| 可选电脑采集器 | Claude | 隔离的官方订阅登录，正常使用时的 statusline 回调 |
| 可选电脑采集器 | DeepSeek | 隔离私有密钥调用官方余额 API |

刷新不发送模型提示。保留来源 `observed_at`；读取缓存、修订号变化不代表新观察。缺失 5h/周窗口隐藏，真实 0% 可见。重置时间已过则等待来源数据，不自动恢复 100%。倒计时为 `🔄 xd xh`，到期时间为 `xd xh 到期`；不足一小时的正数为 `<1h`。

Codex 设备授权复现[官方客户端实现](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs)，使用可配置 `QUOTA_DIRECT_CODEX_CLIENT_ID` 及固定 `auth.openai.com`/`chatgpt.com` 来源。额度端点属于客户端实现细节，并非稳定第三方 API；本工程未注册 OAuth 集成。直连只映射恰好 18000/604800 秒的主窗口。剩余额度、可用重置次数来自 usage；可选详情失败只让到期未知，保留新额度。剩余额度保留来源字符串，不推测币种。仅在全部可用详情已知时展示最近到期；正数次数仍以来源为准。Claude 无对应扩展字段。

DeepSeek 展示原始十进制人民币 `total_balance`，包括赠送/充值但不展开、不换算、不回退美元。别名不是认证邮箱。API 不提供消费历史、请求总数或累计 Tokens。设备更换密钥先验证候选，失败保留旧密钥与带标记的余额。电脑密钥存仅所有者可访问的隔离档案，不进入设备配置或快照。

主要参考：[Codex 授权](https://learn.chatgpt.com/codex/auth)、[app-server](https://learn.chatgpt.com/codex/app-server)、[Claude 授权](https://code.claude.com/docs/en/authentication)、[statusline](https://code.claude.com/docs/en/statusline)、[DeepSeek 余额](https://api-docs.deepseek.com/api/get-user-balance/)。

## 传输与命令

可选采集器的本机设置端口为 **4317**，内网固定证书 HTTPS 为 **4318**。设备请求使用配对 Bearer、主机名验证且不跟随重定向。`GET /v1/snapshot` 最多八账户/8192 字节，`POST /v1/refresh` 接收合并的来源刷新。电脑设置不决定 Passport 设置。快照仅更新当前目录绑定；未知行须明确导入。采集器失败只影响其账户。

USB 保留物理 120 秒配对窗口及有界 `@AIQ:` 换行帧。v1 `configure` 含 request ID、Wi-Fi、端点/令牌/证书/时间；响应须匹配 ID 且不回显秘密。固件将完整电脑端点及代数存统一 model，更换端点后旧绑定显示 `source_changed`；确认新电脑并核实远端 ID/服务后才重绑。端点保存 ACK 不代表候选 Wi-Fi 验证成功。Wi-Fi 从浏览器内存直接传 USB，不经过电脑 API。USB 无常驻读取任务。

USB v2 复用物理窗口和单一执行者。`session_open` 将首次请求编号绑定到新生成的 32 位十六进制 `session_id`；相同编号重试不延长窗口。`state_get` 返回统一的脱敏状态；`command` 包含原有扁平 v1 命令，内外请求编号必须一致。`collector_configure` 只提交私有固定证书端点，复用四个任务回执与验证保存，不把证书放入通用队列，也不复制电脑服务凭据。无效或过期会话不返回当前会话编号。入站帧仍为 4096 字节，命令 2048 字节，状态 16384 字节加封装；浏览器接收行上限为 32768 字节。

空闲 USB 窗口保留设备 Wi-Fi 和直接服务请求。进入准备、半帧暂存阻止新请求；暂存资料在 TLS 前清理释放，半帧三秒过期。网页只允许一个尚未确认的修改，最多在同一窗口内按原始内容、编号和时间重试一次，不跨重开窗口或重启自动重放。状态查询不淘汰任务回执。网页响应预算为九十秒，覆盖多阶段执行等待，不宣称 DNS 硬期限。USB 状态只额外返回授权状态、固定官方授权地址、用户验证码和脱敏错误；热点状态保持原有边界。两种连接使用同一份外部页面模块，脚本只从本来源加载。

设备服务验证证书包及固定来源。响应最多 32 KiB，完整头部最多 16 KiB。异步传输使用 15 秒进展预算、单次等待最多一秒；不宣称所有 DNS/头部情况下的硬总时限。令牌 POST 在 TLS 握手后、发送头部前预留响应；准入失败不得发送授权。响应开始后释放唯一 POST body，释放外层响应再顺序解析 JWT 元信息，不宣称独立 JWT 签名验证。

AP 设置 API 只接受 AP 接口/子网、精确 Host 和会话头 `X-AIQ-Setup`。修改须同源 Origin，GET 若带 Origin 也须匹配；无 CORS/跨域预检。`GET /` 不含私有状态；`/api/state` 返回脱敏账户/历史/发现项/网络/设置/任务；`/api/command` 接收扁平 v1 JSON、八位小写十六进制 request ID 和可选 UTC。最多 2048 字节，拒绝重复/未知字段、非法类型及内嵌 NUL，相同 ID 去重。202 仅代表入队。

命令包括 network_save/network_activate、settings_save、codex_queue/codex_launch、deepseek_save、account_remove/account_deactivate/account_activate、external_import、setup_close、refresh/reconnect、operation_cancel。account_activate 可通过 replace_active_id 原子交换启停；旧电脑绑定须确认重绑。mode_select 已废弃、不支持。命令内部捕获配置代数。公开状态、QR、日志不含服务令牌、PKCE、Wi-Fi 密码或电脑私有凭据；临时热点/密码/设置密钥只在实体设置屏显示。

## 持久化与恢复

保留分区与 v1 记录字节布局。portable NVS 仍为 **0x7c0000 / 256 KiB**。model_v2 是一份 CRC blob，含统一配置、最多十六个紧凑描述项、完整电脑端点及一个 intent。单调序号与精确读回决定已应用/未应用/未知，不假设 nvs_commit 提供跨键事务。未知写入保留确切候选，网络与修改等待存储恢复。类型化读取区分缺失、损坏、I/O、内存、忙；仅真正缺失/有效墓碑为空槽。

新增或替换设备授权先存 intent，再存原 v1 凭据整包，最后提交 model 绑定并清 intent。重启只无网络完成精确有效目标，或在精确前态匹配时清理中断 intent；冲突保留归属，不重放一次性交换。删除设备账户先移除描述项并保存删除 intent，再写精确下一代墓碑、清 intent。代数不回绕。已有设备账户即使满槽也能复用自身槽重新授权。

普通 Codex 续期不改绑定代数：请求前持久化 refresh_inflight，验证后整包替换并清标记。准入前取消可清标记，完整 429 可清标记并退避；已准入的不明超时、重定向、错误、异常成功要求重授权。重启不重放带标记令牌。收到的轮换保留同一个独占凭据缓冲，直到纯存储重试成功，不因息屏/设置/取消丢弃。

observations_v2 每十五分钟最多缓存八个启用账户观察值，包括可选扩展，绑定逻辑 ID、服务、行代数及精确设备/电脑元组；不能提供身份、别名、选择或设置。加载只合并观察字段。v1 缓存仅作迁移输入。迁移保留两组账户、ID 冲突和原启用选择；四个历史网络保留三个常用加一个可见待启用，明确验证交换，不丢密码。

CRC/NVS 不提供保密性。Flash 未加密，无 secure-boot、eFuse 或物理读取防护声明。秘密不进 Git；网络/存储/USB 单一负责人，临时工作区按需释放。公共视图锁只覆盖有界复制，不包围 HTTP/NVS/LVGL/JSON 分配。

## 刷新、显示与验证

息屏关闭 AP/Wi-Fi、停止新 HTTP，存储重试与操作时限继续。热点请求/开放窗口及 USB 进入准备、收发暂存立即阻止新阶段；空闲 USB 窗口允许设备联网。亮屏先显示缓存、恢复 Wi-Fi，静默读电脑缓存且不推迟统一来源刷新点。手动或到点执行一轮，遵守账户退避，结束后起算下轮。缓存读取不伪造新观察。保存时间仅初始化时钟，设备 TLS 等待本次启动设置校时或 SNTP；显示 UTC+8。

首次完整唤醒操作只亮屏。面板 Sleep In、背光关闭，任务等待事件/截止时间。息屏 CPU DFS 可降至 40 MHz，未启用 MCU light/deep sleep；LVGL tick/ADC 按键轮询保留。电池 SOC 缓存三十秒，不推断充电。固定中文用子集字体，不支持的账户字符回退 `?`。

按[验收指南](../development/README.zh_CN.md#验收与报告)分别报告构建、主机/网页、真机/服务及未验证证据。有日期的结果写[变更日志](../CHANGELOG.zh_CN.md)。
