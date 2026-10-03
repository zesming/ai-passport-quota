[简体中文](README.zh_CN.md) · English

# AI Passport Quota

An AI subscription quota dashboard for the FoloToy AI Passport (ESP32-C3, 240 x 320 screen), with a local desktop companion for account login and device settings. Supports up to eight independent ChatGPT/Codex, Claude and DeepSeek API accounts, provider logos, subscription quotas or API balances, refresh timing and offline cache.

The device status bar shows Wi-Fi connection, a green battery fill without a numeric percentage, and the current time after synchronization. OpenAI uses a monochrome mark on the dark screen. Screen off is configurable. Device polling and refresh requests pause while the screen is off; waking immediately queues one synchronization, then resumes the configured refresh schedule. The companion keeps its own desktop refresh schedule. Battery fill always follows measured SOC; a diagonal mark indicates an unavailable reading. Green is the battery icon's color, not a charging-state indication. The physical green LED indicates charging.

## Screenshots

These are captures of the running companion with isolated **documentation example accounts**. Emails, quotas, balances and device indicators are synthetic; no personal account data is included. The left panel is the web device preview, not a photograph of the physical screen. Its clock uses the computer's local time; live board Wi-Fi/battery telemetry is not supplied to the companion.

![Account dashboard and device preview](docs/screenshots/dashboard.jpg)

![Refresh settings](docs/screenshots/settings.jpg)

![DeepSeek balance and device preview](docs/screenshots/deepseek.jpg)

![USB device configuration](docs/screenshots/device-setup.jpg)

## Run the companion

Requirements: Node.js 22+, npm, OpenSSL, and the official Codex/Claude CLI for the providers you want to connect. USB configuration requires desktop Chrome or Edge and a data-capable cable. Firmware uses ESP-IDF **5.5.3**, ESP32-C3, 8 MB Flash, no PSRAM.

```bash
git clone https://github.com/zesming/ai-passport-quota.git
cd ai-passport-quota/companion
npm ci
npm run build
npm start
```

Open **http://127.0.0.1:4317/**. On macOS, `companion/start-dashboard.command` starts an already built installation. Keep the computer and companion running during device synchronization. Executables are discovered automatically; optional overrides are `AIQ_CODEX_BIN`, `AIQ_CLAUDE_BIN`, and `AIQ_STATE_DIR`.

1. In account management, add Codex or Claude and complete authorization on the official provider page. Each profile is isolated; existing CLI credentials are not imported.
2. Codex quota is collected through the official app-server. It describes **Codex usage**, not every ordinary ChatGPT message limit. Missing windows display as unknown.
3. For Claude, copy the account's session launch command from the page and use that profile normally. A statusline callback after a normal model response supplies its quota. Refreshing never sends a paid model prompt.
4. Add DeepSeek with an API key from the [official portal](https://platform.deepseek.com/api_keys). The optional name is a local label; the balance API does not supply a verified email. Keys stay in owner-only local profiles and are never sent to the device. The page displays one RMB available balance (`total_balance`, including grants and top-ups) from the [official balance API](https://api-docs.deepseek.com/api/get-user-balance/), with availability and observation time. Grant/top-up details and USD are not displayed. Missing RMB data stays unknown. This check sends no model prompt.
5. Set automatic refresh at 1, 5, 15 or 30 minutes and automatic screen off at 30 seconds, 1/2/5/10 minutes, or Never (default: 2 minutes).

The DeepSeek integration displays available balance only. Account-wide cumulative/period spending, request counts and historical token totals are not supported. [Model responses](https://api-docs.deepseek.com/api/create-chat-completion/) contain usage for a single request, not the portal's last-30-day totals across other clients.

## Connect the device

Flash a verified build first, then connect USB and open the physical pairing window. For an already configured device, long-press OK, navigate to pairing with up/down, and confirm with OK. The window lasts 120 seconds.

In the web device configuration panel, choose the computer's private IPv4 address and enter **2.4 GHz Wi-Fi** details. Click Connect and configure, select the ESP32-C3 USB Serial/JTAG device, and wait for confirmation. Wi-Fi details travel directly from browser memory to USB. After pairing, the device connects to the selected computer's pinned HTTPS endpoint on port **4318**; port **4317** remains local-only. Re-pair if the computer's IP changes. Stopping sync revokes the token.

On the device: up/down selects an account, short OK refreshes or confirms, and long OK opens settings or returns. Automatic screen off uses the selected timeout. The first function-key gesture wakes only; long DOWN while awake turns the screen off. The independent hardware power key retains its official long-press shutdown behavior. Pairing suppresses automatic screen off. Screen off pauses device snapshot polling, source refresh requests, settings HTTP and explicit Wi-Fi reconnect attempts. Waking queues one synchronization immediately, waiting only for an already admitted bounded request to finish, even when automatic refresh is disabled; if offline, it waits for connectivity. Cached accounts and pairing data remain available.

If USB configuration fails, refresh the web page, reopen the physical window and re-enter Wi-Fi details. Close other serial tools or pairing tabs. The page distinguishes no input, interrupted USB communication, an unmatched acknowledgment and explicit device rejection.

## Build and verify

```bash
# Companion: from the repository root
cd companion
npm ci
npm test
npm run build
cd ..

# Firmware: activate your ESP-IDF 5.5.3 installation first
source <esp-idf-v5.5.3>/export.sh
python3 tools/install_passport_skills.py --install
./tools/validate.sh --static
./tools/validate.sh --firmware
# The complete delivery gate is ./tools/validate.sh
```

The firmware gate produces `build/FoloToy-AI-Passport-full.bin` and a matching debug bundle under `build/firmware/<sha256>/`. Verify the bundle with `python3 tools/archive_firmware.py verify <bundle-directory>`. Flash only after approval for the exact device, artifact and data impact. To retain settings, confirm partition compatibility and write the verified bootloader, partition table and application at their recorded offsets. A complete merged write at `0x0` also writes the NVS gap and can reset settings. Build files are not committed. See [environment setup](docs/development/engineering/environment-setup.md) and [build/flash policy](docs/development/engineering/build-and-test.md).

For frontend development, keep the companion API running, then use a separate terminal:

```bash
cd companion
AIQ_DEV_ORIGIN=http://127.0.0.1:5173 npm start
# separate terminal, same directory
npm run dev
```

Open http://127.0.0.1:5173/ for development, matching the allowed origin above.

For reproducible documentation captures without accessing real profiles: `cd companion && npm run preview:readme`, then open http://127.0.0.1:4327/ . This example service uses temporary settings and synthetic accounts; it is separate from production.

## Development entry points

Development starts with [AGENTS.md](AGENTS.md) and the [application/protocol guide](docs/applications/ai-quota-monitor.md). Dated changes, validation and device acceptance are recorded in the [changelog](docs/CHANGELOG.md).

| Area | Files |
| --- | --- |
| Firmware UI, state and input | `main/main.c`, `main/quota_ui.c`, `main/quota_logic.c` |
| Wi-Fi, pinned HTTPS, USB and NVS | `main/quota_service.c` |
| Display/board drivers | `components/bsp/` |
| React dashboard and USB transport | `companion/src/App.jsx`, `companion/src/serial.mjs`, `companion/src/styles.css` |
| Official login and quota collection | `companion/server/accounts.mjs`, `clients.mjs`, `claude-feed.mjs`, `claude-session.mjs`, `deepseek.mjs` |
| Local API, pairing and storage | `companion/server/index.mjs`, `pairing.mjs`, `protocol.mjs`, `storage.mjs` |
| Host tests | `tests/test_quota_logic.c`, `tests/test_quota_fonts.py`, `tests/test_quota_http_runtime.py`, `tests/test_quota_refresh_runtime.py`, `tests/test_quota_storage_runtime.py`, `companion/test/*.test.mjs` |

Profiles and credentials live outside the repository in `~/.local/share/ai-passport-quota/` (or `AIQ_STATE_DIR`). Never commit auth files, Wi-Fi details, tokens, private keys, device identifiers or raw logs. UI changes should be captured with example accounts. Firmware code outside the LVGL task must use the BSP lock; networking/storage must not block button callbacks. Run the relevant tests and the full firmware gate for firmware delivery; flashing requires separate authorization.

## Origin and license

Firmware retains the MIT-licensed [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) history, based on commit `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`. The local `upstream` remote points there; `origin` is this project. See [LICENSE](LICENSE), [font and firmware assets](assets/README.md), and [companion assets](companion/ASSETS.md) for retained licenses and attribution.
