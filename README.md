[简体中文](README.zh_CN.md) · English

# AI Passport Quota

A quota dashboard for the FoloToy AI Passport: ESP32-C3, 240 × 320 display, 8 MB Flash, no PSRAM. It shows Codex/Claude usage windows, reset countdowns, available banked resets/credits when provided, and DeepSeek CNY balance. Missing windows are hidden; actual 0% remains visible.

## Set up the device

Passport owns its accounts, Wi-Fi and refresh settings. Phones use the device hotspot; computers can open the same settings page over USB; no computer service is required for device-managed accounts. Up to eight active accounts and three personal 2.4 GHz networks are supported.

1. Install firmware using the [developer guide](docs/development/README.md).
2. Long-press OK → **Device settings → Hotspot**. Join the temporary hotspot using the first QR or the displayed network name/password.
3. Short-press OK to show the page QR. Scan it after joining, or use the manual-entry step to enter the complete address and temporary setup key in a computer browser.
4. Save Internet-capable Wi-Fi or a compatible phone hotspot. Add Codex or a DeepSeek API key, then finish setup so Passport can connect and validate.
5. Codex authorization closes the device hotspot. Restore Internet access on the phone/computer, open the official authorization page shown on Passport, and enter its code. Passport saves its independently issued credentials.

The local page has Accounts, Network and Settings tabs and no hosted backend. A bare address loads a setup-key prompt. Hotspot setup lasts ten minutes; USB setup lasts two minutes. Reopening requires a physical action. API keys/tokens are never returned to the page. Device credentials are stored in a dedicated NVS partition; this build does not encrypt Flash against physical access.

Codex uses an experimental reproduction of the official client's device-code flow, not a registered third-party OAuth integration or stable public quota API. It displays **Codex usage**, not all ChatGPT messaging limits. DeepSeek displays total available CNY balance; its balance API does not expose spending history, requests or token totals. Claude subscription collection still requires the optional computer collector.

## Use

Up/Down switches accounts, short OK refreshes/confirms, and long OK opens settings/returns. Long Down turns the display off; the first complete function-key gesture only wakes it. The hardware power key retains long-press shutdown.

Refresh intervals are 1/5/15/30 minutes; auto-sleep options are never/30 seconds/1/2/5/10 minutes. Screen-off stops Wi-Fi and new network requests; received credentials still finish saving. Wake shows cached values, restores Wi-Fi and refreshes when manually requested or due. Network/clock readiness and account errors are reported separately.

Network information shows connection status and saved Wi-Fi names. Device settings offers Hotspot and USB for the same account, network and settings controls.

The status bar shows Wi-Fi, synchronized UTC+8 time and a green battery fill proportional to charge. Green is styling, not verified charging detection. Enterprise certificate Wi-Fi and Bluetooth Internet relay are not implemented; use personal Wi-Fi or a compatible hotspot.

## Computer setup over USB

Start the local page with the commands below, then open **http://127.0.0.1:4317/** in desktop Chrome/Edge. Choose USB device settings, connect the USB port and wait for Passport to start. On Passport, long-press OK → **Device settings → USB**, then connect the settings window in the same browser tab.

Use the Accounts, Network and Settings tabs. Codex starts a new device authorization: approve the displayed code on the official page. DeepSeek sends its API key directly to Passport and verifies it over the saved Wi-Fi. The computer keeps its Internet connection; Passport still needs Internet-capable Wi-Fi. Existing computer OAuth credentials are not copied. Closing or expiry ends setup access, while accepted work continues.

## Optional computer collector

Computer-sourced accounts share the same device list and settings. An unavailable collector affects only those accounts. Existing accounts are preserved during migration; history beyond the eight-active limit can be deactivated/activated without deleting credentials. Codex/DeepSeek can be explicitly reauthorized on Passport to change their source; desktop tokens are not imported or automatically merged by email.

Requires Node.js 22+, npm and OpenSSL; Codex/Claude also require their official clients. Start manually:

```bash
git clone https://github.com/zesming/ai-passport-quota.git
cd ai-passport-quota/companion
npm ci
npm run build
npm start
```

Open **http://127.0.0.1:4317/**. The optional compatibility collector uses isolated private profiles. Claude's official statusline supplies observations during normal usage; refreshing sends no paid model prompt. In USB device settings, explicitly pair this computer collector, wait for verified discovery, then import the required account. Importing a Claude row keeps the computer as its source; it does not transfer provider credentials. The device uses pinned HTTPS on port **4318**. No autostart is installed. Private computer data lives in `~/.local/share/ai-passport-quota/` or `AIQ_STATE_DIR`.

## Screenshots and development

Screenshots use isolated synthetic accounts; browser previews do not establish hardware telemetry.

![Device setup](docs/screenshots/phone-setup.jpg)
![ChatGPT Pro](docs/screenshots/quota-pro.jpg)
![Claude](docs/screenshots/claude.jpg)
![DeepSeek](docs/screenshots/deepseek.jpg)

Read [AGENTS](AGENTS.md), the [developer guide](docs/development/README.md) and [application contracts](docs/applications/ai-quota-monitor.md) to continue development. Validation history belongs in the [changelog](docs/CHANGELOG.md).

Based on the MIT-licensed [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport), commit `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`. See [LICENSE](LICENSE) and [asset licenses](assets/README.md).
