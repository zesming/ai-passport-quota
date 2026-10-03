<p align="right"><a href="ai-quota-monitor.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# AI subscription quota monitor

This application replaces the demo UI with a portrait quota dashboard for the 240 × 320 AI Passport. A local computer companion manages up to eight separate Codex/Claude subscription profiles and DeepSeek API accounts. The device receives only display identity, quotas or balances, settings and observation times.

## Computer companion

The repository `companion/` directory contains the Node.js service and React settings page. Use Node.js 22 or newer, run `npm install`, `npm run build`, then `npm start` there. Open `http://127.0.0.1:4317/`. Keep this service and the computer awake while the device is syncing. Install the official provider clients and OpenSSL before connecting accounts or pairing.

Add an account from Account management, complete the official authorization, and wait for its verified email. Each account uses an independent profile under `~/.local/share/ai-passport-quota/profiles/<id>`. Existing Codex/Claude profiles are not imported. Optional executable overrides are `AIQ_CODEX_BIN` and `AIQ_CLAUDE_BIN`; `AIQ_STATE_DIR` changes the local storage location. Files are private to the OS user. Removing an account detaches it immediately and attempts official logout; profile files are retained.

Codex uses the official app-server device-code login and `account/rateLimits/read`. Displayed quotas cover Codex usage, not all ordinary ChatGPT message limits. Windows are mapped by their duration; a missing 5-hour or weekly window remains unknown.

Claude uses official subscription login and a status-line callback. After login, copy the session launch command from the page and use that isolated Claude profile normally. Its first normal model response may supply quota data. The helper strips ambient provider credentials and binds the callback to the verified account identity. This monitor never sends model prompts to obtain quotas. Repeated callbacks and refresh timers do not manufacture a new observation time.

DeepSeek uses a user-supplied API key and only `GET https://api.deepseek.com/user/balance`. Add or replace the key from the local page; it is stored separately at `profiles/<id>/deepseek/api-key.json` with owner-only file/directory permissions, never in the public account state or device payload. The API supplies no email: an optional local label identifies the account. Amounts remain decimal strings and CNY/USD wallets stay separate. The UI displays only CNY `total_balance`, the available total including grants and top-ups; those parts are not expanded. Unavailable/missing amounts are not replaced with zero, and grants have no invented expiry countdown. A failed normal refresh retains labelled cached balances; replacing a key clears the previous wallet before verification. Removal clears the local DeepSeek key; remote revocation remains in the provider portal.

As of 2026-10-02, no documented public account-wide API was found for cumulative or selected-period spending, request counts or historical token totals. The [Chat Completions response](https://api-docs.deepseek.com/api/create-chat-completion/) reports per-request token usage, not portal history across clients. Do not infer these statistics from balance changes, send model calls to obtain them, or populate them with synthetic values in production. Console statistics require a separately verified data source and are not currently implemented.

The current desktop and device views show RMB only, using Chinese labels. They select the CNY wallet explicitly, preserve its decimal strings and show unknown if CNY is missing. They never fall back to USD or convert currencies. The source/parser/cache retain the official currency entries for compatibility; a future USD view needs its own product change.

The browser retains UTF-8 labels. The device's subset font covers the fixed UI text; arbitrary non-ASCII identity characters use `?` as a display fallback. Use an ASCII label for an exact device rendering, or extend font coverage before changing this rule.

## USB pairing

Use desktop Chrome or Edge, connect a data-capable USB cable, and open the device's pairing screen. In Device configuration, choose the computer's private IPv4 address and enter Wi-Fi details. Click Connect and configure, select the serial port, and wait for the result. Wi-Fi credentials pass directly from browser memory to USB; the companion API never receives them. The device accepts configuration only during a physical 120-second pairing window. Already configured devices boot with the window closed.

The device sync endpoint is `https://<selected-private-ip>:4318`. Pairing provisions a random bearer token and an ECDSA certificate with an IP subject alternative name. The firmware verifies that certificate and host. The settings interface stays bound to loopback; the LAN listener starts only after pairing is requested, or restores previously authorized configuration. If the computer IP changes, re-pair. Stopping LAN sync revokes the pairing token.

## Display and persistence

The dashboard shows the provider logo, verified email, and remaining 5-hour and 7-day percentages. Short OK requests a refresh; up/down selects an account; long OK opens settings or returns. Settings allow account selection, refresh interval, automatic refresh, screen timeout and pairing. DeepSeek has one RMB available balance instead of quota percentage bars. Network and storage work run outside button callbacks and the LVGL lock.

Screen timeout accepts 0 (Never), 30, 60, 120, 300 or 600 seconds, default 120. At timeout the backlight becomes 0%; function-key sensing and USB pairing remain available.

Device-originated snapshot polling, provider refreshes, settings HTTP and explicit Wi-Fi retry/configuration activity pause while asleep. An already admitted Wi-Fi initialization or bounded HTTP request may finish; after a display-state transition, old HTTP results cannot publish and no follow-up operations start. Waking immediately queues one coalesced refresh/snapshot cycle, including when automatic refresh is disabled; an already admitted bounded request finishes before the worker starts that cycle; offline wake waits for connectivity. The normal polling cadence and enabled source-refresh interval resume from the completed wake cycle. The companion retains its independent desktop refresh schedule.

The whole first waking gesture is consumed, including its CLICK/DOUBLE/LONG event. Long DOWN while awake switches the screen off. The independent hardware power key has no supported software short-press signal and retains hardware long-press shutdown. Pairing suppresses automatic screen off and restarts the idle countdown when closing. A bounded one-second event wait updates idle state and the clock; battery reads are cached for thirty seconds.

The CW2017/BSP exposes SOC and cell voltage only; no charger-status input is defined in the board pin map. Charging animation requires a documented, MCU-readable CHG/STAT signal and its polarity/full/fault semantics. USB attachment, increasing SOC or voltage must not be treated as confirmed active charging. Use the physical green indicator described in the [official charging guide](https://ai-passport.folotoy.cn/en/guides/getting-started/).

Wi-Fi icons show actual connection, not fabricated RSSI. The clock shows `--:--` after cold boot until a current-boot pairing or pinned HTTPS snapshot calibrates time. Screen rendering, wake behavior, runtime heap and battery use require device validation.

Unknown data is shown as a dash. When a reset time passes, the window stays unknown until new source data arrives; it is never assumed to be 100%. Old data and offline/expired states remain labelled. Service settings are canonical and persisted on the device only after acknowledgment. The device stores pairing details and a sanitized quota cache in NVS; cached snapshots are written at most every fifteen minutes. Screen timeout uses a separate `screen_to` NVS key; legacy pairing and quota-cache layouts remain unchanged. DeepSeek balances use a separate CRC-protected `balance_cache` sidecar matched to the quota cache revision, configuration identity and saved time.

## Validation

Run the companion's `npm test` and `npm run build`. Firmware delivery requires the repository's complete `./tools/validate.sh` gate with ESP-IDF 5.5.3 activated. On macOS, this workspace uses the documented temporary compiler wrapper for Darwin's linker and a verified actionlint binary; the firmware configuration is still the repository defaults. Device checks must cover Chinese glyph rendering, USB pairing, Wi-Fi reconnect, certificate rejection, buttons, reset windows and reboot persistence. For screen-aware sync, also verify silence of device HTTP/retry activity while asleep, immediate wake synchronization with automatic refresh disabled, offline wake/recovery, rapid sleep/wake during an active request, and resumption of the selected interval. Charging animation remains unavailable without a real hardware signal. Successful compilation does not prove these checks.

## Protocol

Device requests carry `Authorization: Bearer <pair-token>` and do not follow redirects. `GET /v1/snapshot` returns schema version 1, server time, revision, settings and authenticated accounts only. `POST /v1/refresh` coalesces refresh requests. `PATCH /v1/settings` accepts intervals 60, 300, 900 or 1800 seconds and a Boolean automatic-refresh flag. Optional `screen_timeout_seconds` accepts the six display timeout values; omission preserves the existing setting. Legacy state files default to 120, and a new firmware retains its saved timeout when old snapshots/ACKs omit the field. Changing only the timeout does not reset the companion quota timer. Passive snapshot reads do not display source refresh progress.

DeepSeek accounts have `provider: "deepseek"`, empty `email`, `plan: "API"`, a local `label` (up to 32 UTF-8 bytes), null quota windows and nullable `balance: {is_available, balance_infos}`. Each of at most two unique CNY/USD source entries contains string `total_balance`, `granted_balance`, `topped_up_balance` (up to 20 decimal characters, signed values supported). The current view displays only CNY; amounts are never combined between currencies. Older firmware rejects DeepSeek, so update it before adding that provider. Maximum snapshot size is 8192 bytes and account count is eight.

USB frames begin with `@AIQ:` and end with a newline, with at most 4096 bytes. A version-1 `configure` frame contains an eight-digit hexadecimal request ID, Wi-Fi fields, private HTTPS base URL, pair token, certificate and initial trusted time. `result` acknowledgments match the request ID and never echo credentials.

Source contracts: [Codex authentication](https://learn.chatgpt.com/codex/auth), [Codex app-server](https://learn.chatgpt.com/codex/app-server), [Claude status line](https://code.claude.com/docs/en/statusline), [Claude authentication](https://code.claude.com/docs/en/authentication), [DeepSeek balance API](https://api-docs.deepseek.com/api/get-user-balance/).
