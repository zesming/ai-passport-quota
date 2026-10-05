简体中文 · [English](portable-connectivity.md)

# 便携联网方案

状态：调研方案，尚未实现或真机验证。当前固件仍使用手动启动的电脑管理服务。目标是设备保存账号、设置和刷新计划，独立联网更新，手机只辅助配置。

## 推荐基础方案

采用 **设备 HTTPS 直连 + 手机本地配网页 + 普通 Wi-Fi／2.4 GHz 手机热点**。首阶段无需常驻电脑、托管账号服务或三个手机应用。

1. 通过实体按键开启有时限的配置窗口，启动使用独立密码的临时 WPA2 热点。屏幕显示加入热点的 Wi-Fi 二维码，并提供 SSID／密码作为备用入口。
2. 手机加入后，用第二个网址二维码或屏幕地址打开设备本地网页，例如 `http://192.168.4.1`。自动弹出配网页只是便利功能；不能保证所有手机一次扫码就同时入网并打开网页。
3. 目标 Wi-Fi 信息直接提交给设备，修改绑定配置窗口／会话，拒绝跨站提交。服务商凭证不进入二维码、网址或日志。配置完成或超时后关闭热点；联网成功后再分别完成账号授权。
4. 服务商凭证与展示缓存分开保存，设备通过验证证书的 HTTPS 查询额度。普通 Wi-Fi 下，后续刷新不需要手机在场。

乐鑫支持 SoftAP／HTTP 和 BLE／GATT 配网，SoftAP 的额外内存需求较低。本地网页及扫码流程仍需开发，不是现成的跨平台扫码行为。参见 [IDF 5.5.3 配网文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/provisioning/provisioning.html)。

## 账号直连可行性

| 平台 | 直接查询 | 鉴权边界 |
| --- | --- | --- |
| DeepSeek | 正式 `GET /user/balance` 接口；人民币 `total_balance` 保留原始十进制字符串。 | 在本地配置页输入用户 API Key。仍不支持官网消费历史、请求次数及累计 Tokens。[余额 API](https://api-docs.deepseek.com/api/get-user-balance/) |
| Codex／ChatGPT 订阅 | 官方 Codex 实现有直接 HTTPS 查询额度和重置额度的路径，可复用现有窗口、剩余额度与可用重置模型。它展示 Codex 用量，不是全部 ChatGPT 消息额度。 | 持有令牌不等于已解决设备独立登录。Codex 设备码登录属于 beta，需账号／工作区启用；公开的 Sign in with ChatGPT 开源流程使用本机回调，不能假设该流程的令牌可读取 Codex 额度接口。先验证签发、身份和续期。[Codex 鉴权](https://learn.chatgpt.com/docs/auth)、[开源注册与登录](https://developers.openai.com/siwc/token-sharing-open-source/sign-in) |
| Claude 订阅 | 公开客户端有直接查询实现，但尚未确认受支持的个人 Pro／Max 额度 API。当前项目读取官方状态栏采集结果。 | 官方限制第三方 Claude.ai 登录及订阅凭证收集／保存。不能承诺受支持的独立接入，也不能用 API Console 用量替代订阅额度；先保留为待确认项。[凭证规则](https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use)、[状态栏文档](https://code.claude.com/docs/en/statusline) |

Codex 的[官方客户端实现](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/backend-client/src/client/rate_limit_resets.rs#L69)使用令牌和账户 ID 查询 `https://chatgpt.com/backend-api/wham/usage` 及 `/wham/rate-limit-reset-credits`。这些 HTTP 路径是实现依据，不是稳定公开的第三方接口契约。[设备码实现](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs#L62)允许手机批准登录码、设备自行取得令牌，但源码公开不等于允许第三方设备复用 Codex 的 OAuth 客户端 ID，验证前需解决这一边界。Claude 的[公开客户端查询](https://github.com/steipete/CodexBar/blob/c967d07817362c3ceb1c55fc2185ed2ef5494f5c/Sources/CodexBarCore/Providers/Claude/ClaudeOAuth/ClaudeOAuthUsageFetcher.swift#L60-L129)同样只证明技术路径，不能替代平台许可。

账号密码留在服务商自己的登录页。不要复制桌面刷新令牌链后让双方各自轮换。缺失窗口、权威时间、余额原始字符串及可选信息沿用现有语义，不发送模型请求来获得额度。

## 手机与网络选择

| 路径 | iPhone | Android | 鸿蒙 | 选择 |
| --- | --- | --- | --- | --- |
| 加入设备热点后打开本地网页 | 浏览器可用 | 浏览器可用 | 浏览器可用 | 基础方案；实测扫码／入网及网络切换。 |
| 2.4 GHz 手机热点 | 支持，通常共享蜂窝网络 | 支持，能否转发 WLAN 看机型 | 支持，能否转发 WLAN 看机型 | 外出优先；确认频段和设备实际可访问互联网。 |
| 普通网页操作 BLE | Safari／WebKit 未实现 Web Bluetooth | 兼容 Chrome 支持，需 HTTPS 和用户操作 | 浏览器支持未确认 | 不能作为统一配网或中转入口。 |
| 原生 BLE 中转应用 | CoreBluetooth | BLE／GATT | BLE／GATT | 技术上有路径，但需平台适配与后台处理，未验证。 |

依据：[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)、[WebKit 未实现接口](https://webkit.org/tracking-prevention/)、[苹果热点兼容模式](https://support.apple.com/en-ca/guide/security/secfd166f620/web)、[iPhone 热点上游](https://support.apple.com/en-nz/guide/iphone/iph45447ca6/ios)、[Pixel 热点](https://support.google.com/pixelphone/answer/2812516)、[华为热点](https://consumer.huawei.com/cn/support/content/zh-cn15801195/)、[鸿蒙 BLE API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-bluetooth-ble)。

**蓝牙配网不等于蓝牙共享上网。** ESP32-C3 不支持经典蓝牙，不能直接使用系统的经典蓝牙 PAN 网络共享。BLE 中转需要自定义应用协议。参见 [C3 蓝牙支持](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/ble/overview.html)。

若要求凭证只在设备内解密，建议验证 GATT 字节流到手机 TCP 连接的中转，TLS／HTTP 在设备内处理；简单的手机 HTTP 代发会看到鉴权令牌。前者是工程方案，不是 ESP-IDF 开箱即用的功能，需先验证 TLS 接入、缓冲与断连处理。普通网页没有通用 TCP 接口。手机必须在附近，应用后台运行也有限制，不能保证手机锁屏后永久中转。参见 [iOS 后台 BLE](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html)、[Android 后台 BLE](https://developer.android.com/develop/connectivity/bluetooth/ble/background)。

企业 Wi-Fi 可作为高级配置，并非硬件一定不支持：C3 支持 WPA2／WPA3 Enterprise 与 EAP-TLS、PEAP、TTLS。实际网络必须允许该设备及其身份／证书接入；扫码不会自动转移手机里的企业托管凭证。获得具体网络配置后，只实现需要的 EAP 方式。参见 [IDF 5.5.3 Wi-Fi 安全](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi-security.html)。

## 开发交接

- 先验证不依赖电脑采集器的 Codex 独立登录／续期／额度查询，以及 DeepSeek 余额查询。确认平台授权路径及令牌兼容性后，才能承诺便携功能。
- 复用 `quota_logic`、现有展示及息屏／唤醒逻辑，在固件服务内增加小型平台适配；不先做通用插件框架或手机／云端采集器。直连模式由设备掌管刷新计划，唤醒先显示缓存，手动或到点时才查来源。息屏停止网络活动，包括未来的 BLE 中转。
- 增加有界手机配网及已保存网络选择。保留旧配对／缓存可读性，新增独立、带版本的凭证存储。测量令牌／CA 存储及 UI + HTTPS 的可用内存，正式使用前确定凭证保护方式；调研时不写入不可逆安全熔丝。
- 真机通过手机配置、关闭电脑后的刷新、令牌过期／轮换、离线恢复、息屏／唤醒后，才验收直连模式。基础方案确实无法满足某个网络需求时，再增加企业 EAP 或原生 BLE 中转，并实测 TLS／BLE 并行内存和手机锁屏行为。

此方案把凭证归属从电脑转移至设备。迁移完成前，[应用契约](../applications/ai-quota-monitor.zh_CN.md)仍描述当前已实现的产品。
