[简体中文](portable-connectivity.zh_CN.md) · English

# Portable connectivity proposal

Research status: proposed, not implemented or tested on the board. Current firmware still uses the manually started desktop companion. The target is device-owned accounts, settings and refresh scheduling, with direct Internet access and phone-assisted setup.

## Recommended baseline

Use **device HTTPS + a local phone setup page + ordinary Wi-Fi or a 2.4 GHz phone hotspot**. This needs no always-on computer or hosted account service and avoids maintaining three mobile apps in the first stage.

1. A physical setup action opens a bounded window and a temporary WPA2-protected device access point with a unique password. Show its Wi-Fi QR and manual SSID/password fallback.
2. After joining, open the device's local page, for example `http://192.168.4.1`, using a second URL QR or the displayed address. Captive-portal opening is a convenience, not a requirement. One scan cannot be assumed to join Wi-Fi and open a page on every phone.
3. Submit target Wi-Fi details directly to the device. Bind changes to the setup window/session; reject cross-origin submissions and never put provider credentials in QR codes, URLs or logs. Close the setup AP after completion or timeout. Account authorization is a separate provider-specific step after Internet access works.
4. The device retains provider credentials separately from display cache and fetches quotas over verified HTTPS. The phone is unnecessary during normal Wi-Fi operation.

Espressif supports both SoftAP/HTTP and BLE/GATT provisioning; SoftAP has lower additional memory requirements. The local browser UI and QR flow remain project work, not a ready-made universal scanner behavior. See [IDF 5.5.3 provisioning](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/provisioning/provisioning.html).

## Provider feasibility

| Provider | Direct quota query | Authorization boundary |
| --- | --- | --- |
| DeepSeek | Documented `GET /user/balance` with an API key; retain CNY `total_balance` as a decimal string. | Enter the user's key during local setup. Spending history and portal request/token totals remain unsupported. [Balance API](https://api-docs.deepseek.com/api/get-user-balance/) |
| Codex / ChatGPT subscription | Official Codex implementation has direct HTTPS quota and reset-credit queries; reuse the existing windows, Credits and banked-reset model. This is Codex quota, not all ChatGPT message limits. | A valid token is not yet proof of an independent device login flow. Codex device-code login is beta and requires account/workspace enablement; the documented open-source Sign in with ChatGPT flow uses a loopback callback. Its tokens must not be assumed to work with Codex's quota endpoints. Verify issuance, identity and renewal first. [Codex auth](https://learn.chatgpt.com/docs/auth), [OSS registration/sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in) |
| Claude subscription | Public clients implement direct quota queries, but no supported public personal Pro/Max quota contract was established. The current product reads official statusline observations. | Anthropic restricts third-party Claude.ai sign-in and collection/storage of subscription credentials. Do not promise a supported standalone integration or substitute API Console usage for Pro/Max quota. Keep this provider pending an acceptable authorization route. [Credential rules](https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use), [statusline](https://code.claude.com/docs/en/statusline) |

Codex's [official client implementation](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/backend-client/src/client/rate_limit_resets.rs#L69) queries `https://chatgpt.com/backend-api/wham/usage` and `/wham/rate-limit-reset-credits` using a bearer token and account ID. These HTTP paths are implementation evidence, not a stable public third-party contract. Its [device-code implementation](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs#L62) lets a phone approve a code while the device obtains tokens; public source does not establish permission to reuse Codex's OAuth client ID in a third-party device. A proof of concept must resolve that boundary. Claude's [public client query](https://github.com/steipete/CodexBar/blob/c967d07817362c3ceb1c55fc2185ed2ef5494f5c/Sources/CodexBarCore/Providers/Claude/ClaudeOAuth/ClaudeOAuthUsageFetcher.swift#L60-L129) likewise establishes an implementation path, not provider approval.

Account passwords should remain in providers' own login pages. Do not copy an existing desktop refresh-token chain and let both devices rotate it. Missing windows, authoritative timestamps, exact balance strings and optional extras keep their existing meaning. Do not send model prompts to obtain quota.

## Phone and network choices

| Path | iPhone | Android | HarmonyOS | Decision |
| --- | --- | --- | --- | --- |
| Local setup webpage after joining device AP | Browser-based | Browser-based | Browser-based | Baseline; test joining/scanning and network switching on real phones. |
| 2.4 GHz phone hotspot | Supported; normally cellular upstream | Supported; WLAN sharing depends on model | Supported; WLAN sharing depends on model | Preferred travel fallback. Confirm hotspot band and actual device Internet access. |
| Ordinary webpage using BLE | Safari/WebKit does not implement Web Bluetooth | Supported by compatible Chrome, with HTTPS and user interaction | Browser support not established | Cannot be the universal setup or network-relay path. |
| Native BLE relay application | CoreBluetooth | BLE/GATT | BLE/GATT | Technically plausible, with separate platform integration and background constraints; not validated. |

Sources: [Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth), [WebKit excluded APIs](https://webkit.org/tracking-prevention/), [Apple hotspot compatibility](https://support.apple.com/en-ca/guide/security/secfd166f620/web), [iPhone hotspot upstream](https://support.apple.com/en-nz/guide/iphone/iph45447ca6/ios), [Pixel hotspot](https://support.google.com/pixelphone/answer/2812516), [Huawei hotspot](https://consumer.huawei.com/cn/support/content/zh-cn15801195/), [HarmonyOS BLE API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-bluetooth-ble).

**BLE provisioning is not Bluetooth Internet sharing.** ESP32-C3 has no Bluetooth Classic, so a system Classic PAN connection is unavailable. A BLE relay needs a custom application protocol. See [C3 Bluetooth support](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/ble/overview.html).

If credentials must stay private to the device, the proposed relay is a GATT byte stream to a phone TCP connection, with TLS and HTTP terminated on the device. A simpler phone HTTP relay sees bearer credentials. The opaque tunnel is an engineering proposal, not an ESP-IDF turnkey feature: verify TLS transport integration, buffering and disconnect handling before committing to it. A normal webpage provides no general TCP socket API. Phone proximity and app background execution remain required; continuous locked-phone service is not guaranteed. See [iOS background BLE](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html) and [Android background BLE](https://developer.android.com/develop/connectivity/bluetooth/ble/background).

Enterprise Wi-Fi is optional advanced setup, not inherently impossible: C3 supports WPA2/WPA3 Enterprise and EAP-TLS, PEAP and TTLS. The actual network must accept the device and its identity/certificates; scanning a QR does not transfer a phone's managed enterprise credentials. Add only the required EAP method after obtaining a concrete network configuration. See [IDF 5.5.3 Wi-Fi security](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi-security.html).

## Implementation handoff

- First prove independent Codex login/renewal and quota reads, and DeepSeek balance reads without a desktop collector. Provider permission and token compatibility must be established before promising the portable feature.
- Reuse `quota_logic`, current presentation and sleep/wake gates. Add small provider adapters to the firmware service; do not build a generic plugin framework or a mobile/cloud collector first. Direct mode owns refresh deadlines on the device; wake shows cache immediately and only refreshes source when manual/due. Screen off stops networking, including a future BLE relay.
- Add bounded phone setup and saved network selection. Keep legacy pairing/cache readable; use separate versioned provider storage. Measure token/CA storage and free heap with UI plus HTTPS, and choose protected credential storage before production. Do not enable irreversible security fuses as part of research.
- Accept direct mode only after real-phone setup, computer-off refresh, token expiry/rotation, offline recovery and sleep/wake checks pass. Add enterprise EAP or a native BLE tunnel only when the baseline cannot meet an actual network need; measure simultaneous TLS/BLE memory and locked-phone behavior then.

This proposal changes credential ownership from the desktop to the device. Existing [application contracts](../applications/ai-quota-monitor.md) continue to describe the implemented product until migration is completed.
