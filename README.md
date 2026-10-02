[简体中文](README.zh_CN.md) · English

# AI Passport Quota

An AI subscription quota dashboard for the FoloToy AI Passport (ESP32-C3, 240 x 320 screen), with a local desktop companion for account login and device settings. Supports up to eight independent ChatGPT/Codex and Claude subscription profiles, provider logos, verified emails, remaining 5-hour/weekly quotas, refresh timing and offline cache.

## Screenshots

These are captures of the running companion with isolated **documentation example accounts**. Emails and quotas are synthetic; no personal account data is included. The left panel is the web device preview, not a photograph of the physical screen.

![Account dashboard and device preview](docs/screenshots/dashboard.jpg)

![Refresh settings](docs/screenshots/settings.jpg)

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
4. Set automatic refresh and an interval of 1, 5, 15 or 30 minutes.

## Connect the device

Flash a verified build first, then connect USB and open the physical pairing window. For an already configured device, long-press OK, navigate to pairing with up/down, and confirm with OK. The window lasts 120 seconds.

In the web device configuration panel, choose the computer's private IPv4 address and enter **2.4 GHz Wi-Fi** details. Click Connect and configure, select the ESP32-C3 USB Serial/JTAG device, and wait for confirmation. Wi-Fi details travel directly from browser memory to USB. After pairing, the device connects to the selected computer's pinned HTTPS endpoint on port **4318**; port **4317** remains local-only. Re-pair if the computer's IP changes. Stopping sync revokes the token.

On the device: up/down selects an account, short OK refreshes or confirms, and long OK opens settings or returns. Two idle minutes dim the backlight; a button wakes it. The active pairing window stays bright.

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

The firmware gate produces `build/FoloToy-AI-Passport-full.bin` and a matching debug bundle under `build/firmware/<sha256>/`. Verify the bundle with `python3 tools/archive_firmware.py verify <bundle-directory>`. Flash the verified merged image at `0x0` only after approval for that device and data impact; a complete merged write can reset stored settings. Build files are not committed. See [environment setup](docs/development/engineering/environment-setup.md) and [build/flash policy](docs/development/engineering/build-and-test.md).

For frontend development, keep the companion API running, then use a separate terminal:

```bash
cd companion
AIQ_DEV_ORIGIN=http://127.0.0.1:5173 npm start
# separate terminal, same directory
npm run dev
```

Open http://127.0.0.1:5173/ for development, matching the allowed origin above.

For reproducible documentation captures without accessing real profiles: `cd companion && npm run preview:readme`, then open http://127.0.0.1:4327/ . This example service uses temporary settings and synthetic accounts; it is separate from production.

## Continue development

Start with [AGENTS.md](AGENTS.md), this README and the [application/protocol guide](docs/applications/ai-quota-monitor.md). Use this repository as the source of truth; do not continue editing the earlier separate working directories.

| Area | Files |
| --- | --- |
| Firmware UI, state and input | `main/main.c`, `main/quota_ui.c`, `main/quota_logic.c` |
| Wi-Fi, pinned HTTPS, USB and NVS | `main/quota_service.c` |
| Display/board drivers | `components/bsp/` |
| React dashboard and USB transport | `companion/src/App.jsx`, `companion/src/serial.mjs`, `companion/src/styles.css` |
| Official login and quota collection | `companion/server/accounts.mjs`, `clients.mjs`, `claude-feed.mjs`, `claude-session.mjs` |
| Local API, pairing and storage | `companion/server/index.mjs`, `pairing.mjs`, `protocol.mjs`, `storage.mjs` |
| Host tests | `tests/test_quota_logic.c`, `tests/test_quota_fonts.py`, `companion/test/*.test.mjs` |

Latest validation on **2026-10-02**: 28 companion tests and its production build passed; the firmware complete gate and font coverage passed. The exact firmware was flashed and booted without observed crashes. The user confirmed clear Chinese on the initial pairing page and the two-minute window expiry. One real Codex profile supplied weekly quota; its source omitted the 5-hour window.

Hardware observations used the pre-consolidation full image `690229c2dca7ec2ee0a794d1c4f8625468e4feedab54507b7c3cae0448b1af7c`, with ELF SHA-256 `15a33663935734a7df19fd553db87e461f53d1a22e2984973342ca8df3bc0858`. Rebuilding this checkout creates a new artifact identity; its new build has not been flashed. Always match the actual image's ELF before decoding a crash.

**Next acceptance task:** reproduce browser USB pairing on the actual board. A reported 15-second timeout prompted eager receive, a bounded startup wait, a 4096-byte stream buffer and specific errors; the host checks pass, but the real retry has not been confirmed. Safe short and production-sized invalid frames returned matching acknowledgments over native serial. Opening a serial monitor can reset the board, so avoid competing tools and distinguish a reset from pairing expiry.

The web low/critical quota preview also had a full-width colored number background; it is fixed and browser-checked. Keep the number background transparent and fill only the remaining proportion. Do not present stream simulations or web previews as physical acceptance.

After successful pairing, verify Wi-Fi/HTTPS sync, Claude authorization and real quota, all screens/buttons/logos, settings round-trip, offline/reboot cache, dim/wake and runtime heap during TLS. Keep unknown/expired windows unknown, preserve source timestamps, and never invent fresh quotas.

Profiles and credentials live outside the repository in `~/.local/share/ai-passport-quota/` (or `AIQ_STATE_DIR`). Never commit auth files, Wi-Fi details, tokens, private keys, device identifiers or raw logs. UI changes should be captured with example accounts. Firmware code outside the LVGL task must use the BSP lock; networking/storage must not block button callbacks. Run the relevant tests and the full firmware gate for firmware delivery; flashing requires separate authorization.

## Origin and license

Firmware retains the MIT-licensed [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) history, based on commit `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`. The local `upstream` remote points there; `origin` is this project. See [LICENSE](LICENSE), [font and firmware assets](assets/README.md), and [companion assets](companion/ASSETS.md) for retained licenses and attribution.
