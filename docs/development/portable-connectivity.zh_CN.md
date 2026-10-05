简体中文 · [English](portable-connectivity.md)

# 便携连接与交互设计

本文说明当前实验版便携实现的设计。主机测试和合成浏览器预览不能证明真实手机兼容性、服务商授权成功、开发板 TLS 内存余量或凭证物理保护。完成这些验收后，才能把 DIRECT 作为正式功能。

## 模式与数据归属

| 模式 | 凭证与额度来源 | 设置方式 |
| --- | --- | --- |
| `DIRECT` | 设备保存独立签发的 Codex 凭证或 DeepSeek API 密钥，自行通过校验证书的 HTTPS 查询。正常使用不需要电脑。 | 新设备默认此模式；本地手机页管理最多 3 个网络、8 个账户。 |
| `COMPANION` | 现有电脑采集器持有服务商凭证，通过已认证连接提供快照。 | 已有配置保持此兼容默认值；物理按键开启的 USB 配对窗口仍为 120 秒。 |

手机页明确提供模式选择。切换保留两种模式各自的配置和缓存，不导入电脑令牌，也不会自动完成新账户授权。不要把电脑的刷新令牌链复制到设备。Claude 保持手动启动的电脑 companion 集成；本版本不提供独立 Claude 订阅授权或直连额度采集。

## 手机设置与网络生命周期

基础路径为 2.4 GHz 个人 Wi-Fi 或兼容的手机热点。物理设置操作打开 **600 秒**的临时 WPA2 热点。每次生成新的 SSID、16 字符密码和独立的 43 字符设置 secret。窗口内暂停额度 HTTPS；实现断开 STA，不依赖手机同时维持自身热点和设备 AP 连接。

1. PHONE 第一步在设备屏幕显示 Wi-Fi 二维码、SSID 和完整密码。不支持 Wi-Fi 二维码的手机可以手动加入。
2. 第二步显示 `http://192.168.4.1/#s=<临时secret>` 的网址二维码和普通地址提示。fragment 承载设置授权。只输入普通地址能加载网页，但不能授权修改；加入热点后仍需扫描第二个二维码。不依赖 captive portal 自动弹页。
3. 原生本地页分为账户、网络、设置三个标签，输入字体为 16 像素，触控控件至少高 44 像素。不依赖 CDN、React、外部字体或托管后端。网络连接、时间准备和操作进度与账户验证状态分别展示。
4. 保存网络提交候选配置。结束设置、重新连接、窗口到期或开始 Codex 授权后关闭 AP，再连接和验证候选网络，成功后提交保存。失败保留之前的配置。已有网络可点“使用”；同名 SSID 自动更新，3 个位置已满时必须明确选择替换位置。

个人网络密码接受 8–63 字节或 64 字符十六进制 PSK；无密码网络需在表单中明确选择。每个手机命令都提交 UTC 秒数，为 TLS 初始化时间；之后可由 SNTP 或服务端时间更新。时钟问题显示“时间待同步”，不能冒充服务商验证成功。

关闭操作先留出确认响应返回时间，再关闭 AP。页面停止轮询，提示手机恢复互联网连接并到设备查看结果。“已接收”不等于联网或验证成功。单独关闭浏览器页不会取消设备操作。窗口到期禁用修改，需要物理操作重新打开并扫描新二维码；不会自动延长设置窗口。

## 服务商交互

**Codex：**可填写别名，先提交 `codex_queue`，再提交 `codex_launch`。开始操作关闭 AP 后才访问服务商，并开启独立、最长 **15 分钟**的授权窗口。设备连接已保存的 Wi-Fi/手机热点，取得 device code，显示固定官方验证网址二维码和完整验证码。手机恢复有网连接后打开官方页面、输入验证码；不承诺扫码自动填码。设备自行轮询和交换凭证，不使用浏览器回调，也不要求继续连接本地页。屏幕区分连接中、取码中、等待批准、交换中、成功、失败、取消和到期。活动授权可长按 OK 明确取消；已收到凭证的交换必须先完成整包持久化，再确定终态。

适配器实验性复现官方 Codex 客户端行为。device-code 是否可用取决于账户或工作区设置；OAuth 客户端标识和额度接口并非本产品已注册或稳定的第三方契约。独立签发、续期和实际额度读取仍需验收。参考 [Codex 认证](https://learn.chatgpt.com/docs/auth)和[官方 device-code 实现](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs#L62)。

**DeepSeek：**必填本机别名，密钥输入默认隐藏。“保存到设备”产生真实的待验证账户；只有设置结束、STA 可用后才开始服务商验证。CNY `total_balance` 保持原始十进制字符串，不虚构邮箱。替换密钥先验证和成功保存候选，再替换旧密钥与余额；留空只改别名。候选无效时提示错误并保留旧账户。[官方余额接口](https://api-docs.deepseek.com/api/get-user-balance/)不提供订阅额度或网页消费历史。

**账户反馈：**保留缓存观测值并显示时间，区分离线、待验证、需重新授权、时钟不可用和 HTTP 429 退避。缺失窗口保持不可用，重置时间已过的窗口等待新来源数据，不声称额度已恢复。移除账户使用内联确认，提供“取消”，完成后选择剩余账户。已有密钥和令牌不回传网页。

## 设备导航、刷新与睡眠

保留 240 × 320 深色设备界面和 216 × 8 额度条。设置菜单六行顺序为：账户管理、刷新间隔、立即刷新、自动息屏、网络与配网、电脑配对。NETWORK 提供手机配网和重新连接。PHONE 用 Up/Down 切换两步，短按 OK 前进或重新打开已过期窗口，长按 OK 关闭设置并返回。两个二维码采用整数模块缩放，在 168 像素白色区域内保留四模块静区。

首页 Up/Down 切账户，短按 OK 刷新全部，长按 OK 进入设置。子菜单短按 OK 确认，长按 OK 返回。AUTH 活动中长按 OK 取消，终态时返回。长按 Down 全局请求息屏。唤醒的第一个完整按键手势会被消费，不会同时切账户、刷新或进入设置。

DIRECT 只有一个全局刷新周期：手动或 1/5/15/30 分钟。手机“立即刷新全部账户”与首页短按 OK 使用同一动作。睡眠停止 Wi-Fi 和 AP，不准入新的服务商 HTTP。活动授权暂停网络，但单调时钟期限继续；到期前唤醒继续，到期后显示过期。已收到的令牌轮换仍需完成持久化。唤醒立即展示缓存，只有手动请求或已到刷新时间才查询来源。设置和授权在各自有界窗口内临时保持屏幕亮起，不修改用户保存的息屏值（从不/30/60/120/300/600 秒）。

## 本地 API 与安全契约

`quota_portable_service` 串行管理生命周期，`quota_portal` 管理 HTTP 和解析，`quota_direct` 访问服务商，`quota_store` 管理版本化私有存储，`quota_ui`/`quota_logic` 管理显示和导航。嵌入的 `main/portable_setup.html` 与手动启动的 `tools/preview_portable.mjs` 使用同一个页面；预览账户和场景路由仅存在于 localhost fixture。

- `GET /api/state` 返回公开模式、设置剩余时间、网络/时钟/设置、账户观测及授权状态、最近四个有界任务。任务包括请求编号、操作、queued/running/succeeded/failed 和脱敏错误码。
- `POST /api/command` 接受扁平 JSON：`v:1`、8 字符小写十六进制 `request_id`、`op`、可选 Unix 秒数 `phone_utc` 和操作字段。HTTP 202 返回 accepted/请求编号；有界重复请求匹配避免重复副作用。拒绝冲突编号、重复/未知字段、无效 UTF-8/类型/范围、嵌入 NUL 和超过 2048 字节的请求。
- 操作包括 `network_save`（SSID/密码/无密码标记或已保存位置）、`deepseek_save`（别名/密钥/可选账户 ID）、`codex_queue`（可选别名/账户 ID）、`codex_launch`、`account_remove`、`settings_save`、`mode_select`、`setup_close`、`refresh`、`reconnect`。未实现的扫描、EAP 或 BLE 控件不显示。
- 所有敏感请求要求 `X-AIQ-Setup`。页面从 fragment 读取 secret 到内存，用 `history.replaceState` 清除网址，不写浏览器存储。校验精确 Host、AP 本地 socket 和 AP 子网对端。修改要求严格同源 Origin；GET 可不带 Origin，但提供时必须同源。拒绝跨域请求和预检，不开放 CORS。
- 临时 SSID/密码/secret 只供物理屏幕，不进入公开 JSON。服务商凭证不进入二维码、网址、页面状态、日志或快照。凭证整包与缓存身份/代次分离，删除墓碑阻止旧缓存复活。TLS 使用服务商证书包校验和固定来源；Flash/NVS 的物理保护尚未验收。

## 企业 Wi-Fi 与 BLE 的后续路线

| 路线 | 已核实的平台事实 | 项目边界 |
| --- | --- | --- |
| 手机热点 | iPhone 支持兼容个人热点；Android/Huawei 的 WLAN 共享因型号而异。 | 使用有实际互联网连接的 2.4 GHz 热点。不假设所有手机都能共享企业 WLAN，也不假设加入设备 AP 时仍维持自身热点。 |
| 企业 EAP | C3/ESP-IDF 支持 WPA2/WPA3 Enterprise 和基于证书/身份的 EAP 方法。 | 本版没有企业设置 UI。先确定具体 EAP 方法，验证 CA、身份、客户端证书、设备准入和存储，再扩展。扫码不能导出手机的受管企业凭证。 |
| 原生 BLE 隧道 | C3 支持 BLE，不支持 Bluetooth Classic PAN。iOS、Android、HarmonyOS 有原生 BLE/GATT API。 | 后续可设计 GATT 字节流 → 手机原生 TCP 连接器 → 互联网。TLS/HTTP 终止在设备，使手机传送不透明 TLS；HTTP 代理会看到 bearer 凭证。这是未验证的传输设计，不是 BLE 自动共享网络。 |
| 网页 BLE | 兼容的 Android Chrome 支持安全上下文 Web Bluetooth；Safari/WebKit 不支持。Harmony 浏览器支持尚未确定。 | 普通网页不能提供跨平台 BLE 网络中继，也无通用 TCP socket。原生应用需要分别集成平台，处理有界缓冲、断连和后台限制；不保证锁屏后持续运行。 |

主要参考：[ESP-IDF Wi-Fi 安全](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi-security.html)、[C3 BLE 支持](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/ble/overview.html)、[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)、[WebKit API 策略](https://webkit.org/tracking-prevention/)、[HarmonyOS BLE](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-bluetooth-ble)、[Apple 热点兼容性](https://support.apple.com/en-ca/guide/security/secfd166f620/web)。

验收需覆盖真实 iPhone/Android/Harmony 设置与网络切换、关闭电脑后的来源查询、Codex 令牌轮换和持久化失败、候选回滚、超时/取消/睡眠/唤醒竞态、离线/429 恢复，以及 TLS/UI 堆内存余量。本文不声称所有手机型号已测试。EAP、原生 BLE 应用和正式物理保护仍在本实验基础版本范围之外。
