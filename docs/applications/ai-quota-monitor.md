<p align="right"><a href="ai-quota-monitor.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# AI subscription quota monitor

This application replaces the demo UI with a portrait quota dashboard for the 240 × 320 AI Passport. A local computer companion manages up to eight separate Codex and Claude subscription profiles. The device receives only email, plan, quota windows, settings and observation times.

## Computer companion

The repository `companion/` directory contains the Node.js service and React settings page. Use Node.js 22 or newer, run `npm install`, `npm run build`, then `npm start` there. Open `http://127.0.0.1:4317/`. Keep this service and the computer awake while the device is syncing. Install the official provider clients and OpenSSL before connecting accounts or pairing.

Add an account from Account management, complete the official authorization, and wait for its verified email. Each account uses an independent profile under `~/.local/share/ai-passport-quota/profiles/<id>`. Existing Codex/Claude profiles are not imported. Optional executable overrides are `AIQ_CODEX_BIN` and `AIQ_CLAUDE_BIN`; `AIQ_STATE_DIR` changes the local storage location. Files are private to the OS user. Removing an account detaches it immediately and attempts official logout; profile files are retained.

Codex uses the official app-server device-code login and `account/rateLimits/read`. Displayed quotas cover Codex usage, not all ordinary ChatGPT message limits. Windows are mapped by their duration; a missing 5-hour or weekly window remains unknown.

Claude uses official subscription login and a status-line callback. After login, copy the session launch command from the page and use that isolated Claude profile normally. Its first normal model response may supply quota data. The helper strips ambient provider credentials and binds the callback to the verified account identity. This monitor never sends model prompts to obtain quotas. Repeated callbacks and refresh timers do not manufacture a new observation time.

## USB pairing

Use desktop Chrome or Edge, connect a data-capable USB cable, and open the device's pairing screen. In Device configuration, choose the computer's private IPv4 address and enter Wi-Fi details. Click Connect and configure, select the serial port, and wait for the result. Wi-Fi credentials pass directly from browser memory to USB; the companion API never receives them. The device accepts configuration only during a physical 120-second pairing window. Already configured devices boot with the window closed.

The device sync endpoint is `https://<selected-private-ip>:4318`. Pairing provisions a random bearer token and an ECDSA certificate with an IP subject alternative name. The firmware verifies that certificate and host. The settings interface stays bound to loopback; the LAN listener starts only after pairing is requested, or restores previously authorized configuration. If the computer IP changes, re-pair. Stopping LAN sync revokes the pairing token.

## Display and persistence

The dashboard shows the provider logo, verified email, and remaining 5-hour and 7-day percentages. Short OK requests a refresh; up/down selects an account; long OK opens settings or returns. Settings allow account selection, refresh interval, automatic refresh and pairing. Network and storage work run outside button callbacks and the LVGL lock.

After two minutes without button input, the backlight dims to 15%; any button restores brightness. An active pairing window keeps the display bright. Quota synchronization continues while dimmed. Brightness transitions, runtime free heap, largest free block during Wi-Fi/TLS, and battery use require device validation.

Unknown data is shown as a dash. When a reset time passes, the window stays unknown until new source data arrives; it is never assumed to be 100%. Old data and offline/expired states remain labelled. Service settings are canonical and persisted on the device only after acknowledgment. The device stores pairing details and a sanitized quota cache in NVS; cached snapshots are written at most every fifteen minutes.

## Validation

Run the companion's `npm test` and `npm run build`. Firmware delivery requires the repository's complete `./tools/validate.sh` gate with ESP-IDF 5.5.3 activated. On macOS, this workspace uses the documented temporary compiler wrapper for Darwin's linker and a verified actionlint binary; the firmware configuration is still the repository defaults. Device checks must cover Chinese glyph rendering, USB pairing, Wi-Fi reconnect, certificate rejection, buttons, reset windows and reboot persistence. Successful compilation does not prove these checks.

## Protocol

Device requests carry `Authorization: Bearer <pair-token>` and do not follow redirects. `GET /v1/snapshot` returns schema version 1, server time, revision, settings and authenticated accounts only. `POST /v1/refresh` coalesces refresh requests. `PATCH /v1/settings` accepts intervals 60, 300, 900 or 1800 seconds and a Boolean automatic-refresh flag. Maximum snapshot size is 8192 bytes and account count is eight.

USB frames begin with `@AIQ:` and end with a newline, with at most 4096 bytes. A version-1 `configure` frame contains an eight-digit hexadecimal request ID, Wi-Fi fields, private HTTPS base URL, pair token, certificate and initial trusted time. `result` acknowledgments match the request ID and never echo credentials.

Source contracts: [Codex authentication](https://learn.chatgpt.com/codex/auth), [Codex app-server](https://learn.chatgpt.com/codex/app-server), [Claude status line](https://code.claude.com/docs/en/statusline), [Claude authentication](https://code.claude.com/docs/en/authentication).
