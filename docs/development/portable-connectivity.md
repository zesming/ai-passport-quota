[简体中文](portable-connectivity.zh_CN.md) · English

# Portable connectivity and interaction design

This is the current design for the experimental portable implementation. Host tests and synthetic browser previews do not establish real-phone interoperability, successful provider authorization, board TLS memory margins or physical credential protection. Those acceptance checks remain required before treating DIRECT as production-ready.

## Modes and ownership

| Mode | Credentials and quota source | Setup |
| --- | --- | --- |
| `DIRECT` | Device stores independently issued Codex credentials or a DeepSeek API key and makes verified HTTPS requests. No computer is required during normal operation. | New devices default here; local phone setup manages up to 3 saved networks and 8 accounts. |
| `COMPANION` | Existing computer collector owns provider credentials and supplies authenticated snapshots. | Existing configurations keep this compatibility default. The physical USB pairing window remains 120 seconds. |

The phone mode selector is explicit. Switching modes preserves their separate configurations and caches; it does not import desktop tokens or authorize new accounts. Do not copy a desktop refresh-token chain into the device. Claude remains a manually started computer-companion integration; this version offers no independent Claude subscription authorization or direct quota collection.

## Phone setup and network lifecycle

The baseline is 2.4 GHz personal Wi-Fi or a compatible phone hotspot. A physical setup action opens a temporary WPA2 AP for **600 seconds**. Each window generates a fresh SSID, 16-character password and independent 43-character setup secret. Normal quota HTTPS pauses during this window; the implementation disconnects STA rather than relying on simultaneous phone-hotspot and device-AP connectivity.

1. PHONE step 1 shows the Wi-Fi QR, SSID and complete password on the physical screen. Phones without Wi-Fi QR support can join manually.
2. PHONE step 2 shows `http://192.168.4.1/#s=<temporary-secret>` as a URL QR and the plain address as a hint. The fragment carries setup authorization. Typing the plain address alone loads the page but cannot authorize changes; scan the second QR after joining. Captive-portal opening is not required.
3. The native local page has Accounts, Network and Settings tabs, 16-pixel inputs and controls at least 44 pixels high. It has no CDN, React runtime, external font or hosted backend. It displays network connection, clock readiness and operation progress separately from provider readiness.
4. Saving a network queues a candidate. Closing setup, reconnecting, timing out or launching Codex closes the AP; the device then connects to the candidate, validates it and commits success. Failure preserves the prior saved configuration. Select an existing network with “Use”; adding/updating a matching SSID is automatic, while a full 3-slot list requires explicit replacement.

Personal WPA credentials accept 8–63 bytes or a 64-character hexadecimal PSK; open networks require explicit confirmation in the form. The phone submits its UTC time with every command for TLS bootstrap; SNTP/provider time can subsequently update the clock. A clock problem must appear as “Time pending,” never as an authenticated provider success.

The AP closes after an accepted closing command has had time to return its acknowledgment. The page stops polling and instructs the user to restore Internet access and read results on the device. An acknowledgment means accepted, not connected or validated. Closing a browser tab alone does not cancel a device operation. Expiry disables page mutations; reopening requires a physical action and fresh QR. There is no automatic setup-window extension.

## Provider interaction

**Codex:** choose an optional alias, submit `codex_queue`, then `codex_launch`. Launch closes the AP before provider HTTPS and starts an independent, bounded **15-minute** authorization window. The device connects to saved Wi-Fi/hotspot, requests a device code and displays the fixed official verification URL QR plus the full user code. The phone returns to an Internet connection, opens the official page and enters the code; scanning is not promised to fill it automatically. The device polls and exchanges credentials without a browser callback or continued local-page connection. Screen states distinguish connecting, requesting code, waiting, exchanging, success, failure, cancellation and expiry. Long OK explicitly cancels an active login; a completed exchange that has already received credentials must finish persisting its bundle before terminal state is resolved.

The adapter reproduces official Codex client behavior experimentally. Device-code availability depends on account/workspace settings; the OAuth client identifier and quota endpoints are not a registered or stable third-party product contract. Real independent issuance, renewal and quota reads remain acceptance gates. See [Codex authentication](https://learn.chatgpt.com/docs/auth) and the [official device-code implementation](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs#L62).

**DeepSeek:** require a local alias and masked API-key input. “Save to device” creates a truthful pending account; provider verification starts only after setup ends and STA is ready. Display CNY `total_balance` as its original decimal string and do not invent an email. Replacing a key retains the old committed key and balance until the candidate verifies and saves successfully. A blank replacement key changes only the alias. Invalid candidates produce an error and preserve the old account. The [documented balance API](https://api-docs.deepseek.com/api/get-user-balance/) does not provide subscription quota or portal spending history.

**Account feedback:** keep cached observations visible with their age and distinguish offline, pending verification, reauthorization, clock unavailable and HTTP 429/backoff. Missing windows remain unavailable; elapsed reset timestamps wait for new source data rather than claiming renewed quota. Account removal requires an inline confirmation with Cancel and returns to the remaining selection. Existing keys/tokens are never returned to the page.

## Device navigation, refresh and sleep

Preserve the 240 × 320 dark device presentation and 216 × 8 quota tracks. Settings has six rows in this order: Accounts, Refresh interval, Refresh now, Auto sleep, Network/setup, Computer pairing. NETWORK offers Phone setup and Reconnect. PHONE toggles its two steps with Up/Down; short OK advances or reopens an expired window, and long OK closes setup and returns. Both QRs use integer module scaling and a four-module quiet zone inside a 168-pixel white area.

On HOME, Up/Down switches accounts, short OK refreshes all, and long OK opens settings. Submenus use short OK to confirm and long OK to return. AUTH uses long OK to cancel while active and return once terminal. Long Down requests sleep globally. The first complete wake gesture is consumed and cannot also select, refresh or open settings.

DIRECT has one global cadence: manual or 1/5/15/30 minutes. Phone “Refresh all” and HOME short OK use the same refresh action. Sleep stops Wi-Fi and AP and admits no new provider HTTP. An active authorization is paused, but its monotonic deadline keeps running; waking before expiry resumes, while an elapsed deadline yields expired. Already received credential rotations still need durable completion. Wake presents cached values immediately and only queries the source when manually requested or due. Setup/login temporarily hold the display awake within their bounded windows, without changing the user's stored sleep setting (never/30/60/120/300/600 seconds).

## Local API and security contract

Implementation ownership is split between `quota_portable_service` (serialized lifecycle), `quota_portal` (HTTP/parser), `quota_direct` (provider transport), `quota_store` (versioned private storage), and `quota_ui`/`quota_logic` (rendering/navigation). The embedded `main/portable_setup.html` and manually started `tools/preview_portable.mjs` use the same page; preview accounts and scenario routes exist only in the localhost fixture.

- `GET /api/state` returns public mode, setup time remaining, network/clock/settings, account observations/auth status and the last four bounded jobs. Jobs expose request ID, operation, queued/running/succeeded/failed and a sanitized error code.
- `POST /api/command` accepts a flat JSON object: `v:1`, 8-character lowercase hexadecimal `request_id`, `op`, optional Unix-seconds `phone_utc` and operation fields. Return HTTP 202 with accepted/request ID; bounded duplicate-request matching prevents repeated side effects. Reject conflicting IDs, duplicate/unknown fields, invalid UTF-8/types/ranges, embedded NULs and bodies over 2048 bytes.
- Operations: `network_save` (SSID/password/open-network or saved index), `deepseek_save` (alias/key/optional account ID), `codex_queue` (optional alias/account ID), `codex_launch`, `account_remove`, `settings_save`, `mode_select`, `setup_close`, `refresh`, `reconnect`. No scan/EAP/BLE control is shown unless implemented.
- All sensitive requests require `X-AIQ-Setup`; the page reads its secret from the fragment into memory, clears the URL with `history.replaceState`, and uses no browser storage. Validate exact Host, AP-local socket and AP-subnet peer. Mutations require exact same-origin Origin; GET may omit Origin, but any supplied Origin must match. Reject cross-origin requests/preflight; do not expose CORS.
- Setup SSID/password/secret stay in the physical view, not public JSON. Provider credentials stay out of QR, URL, page state, logs and snapshots. Credential bundles and cache identities/generations are separate; removal tombstones prevent old cache resurrection. TLS verifies the provider certificate bundle and fixed origins; physical Flash/NVS protection is not yet accepted.

## Future enterprise Wi-Fi and BLE route

| Route | Verified platform facts | Project boundary |
| --- | --- | --- |
| Hotspot fallback | iPhone supports compatible personal hotspots; Android/Huawei WLAN sharing varies by model. | Choose a 2.4 GHz hotspot with actual Internet access. Do not assume every phone can relay an enterprise WLAN or stay on its own hotspot while joining the device AP. |
| Enterprise EAP | C3/ESP-IDF supports WPA2/WPA3 Enterprise and certificate/identity-based EAP methods. | No enterprise setup UI is implemented. Choose a concrete EAP method and validate CA/identity/client certificates, device admission and storage before adding it. A phone scan cannot export managed enterprise credentials. |
| Native BLE tunnel | C3 supports BLE, not Bluetooth Classic PAN. iOS, Android and HarmonyOS have native BLE/GATT APIs. | Future custom GATT byte stream → native phone TCP connector → Internet. Keep TLS/HTTP on the device so the phone transports opaque TLS; an HTTP proxy would see bearer tokens. This is an unproved transport design, not automatic BLE Internet sharing. |
| Browser BLE | Compatible Android Chrome supports secure-context Web Bluetooth; Safari/WebKit does not. Harmony browser support is unestablished. | An ordinary page is not a universal BLE relay and has no general TCP socket API. Native apps require separate platform integration, bounded buffers and disconnect/background handling; locked-phone operation is not guaranteed. |

Primary references: [ESP-IDF Wi-Fi security](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi-security.html), [C3 BLE support](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/ble/overview.html), [Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth), [WebKit API policy](https://webkit.org/tracking-prevention/), [HarmonyOS BLE](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-bluetooth-ble), [Apple hotspot compatibility](https://support.apple.com/en-ca/guide/security/secfd166f620/web).

Acceptance must include real iPhone/Android/Harmony setup and network cutover, computer-off provider reads, Codex token rotation/persistence failures, candidate rollback, timeout/cancel/sleep/wake races, offline/429 recovery, and measured TLS/UI heap margins. No claim is made that all phone models have been tested. EAP, native BLE apps and production physical protection remain outside this experimental baseline.
