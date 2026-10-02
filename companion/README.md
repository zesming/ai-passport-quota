[简体中文](README.zh_CN.md) · English

# AI Passport local quota companion

A local account/settings application for the AI Passport quota-monitor firmware. Add separate Codex and Claude subscription profiles, view source-provided remaining 5-hour/weekly quota, configure refresh timing, and provision the device over USB. No demo accounts or simulated quotas are used.

Use Node.js 22+, then `npm install`, `npm run build`, `npm start`. Open http://127.0.0.1:4317/ . The service needs the official Codex and Claude clients. Current paths are detected automatically; `AIQ_CODEX_BIN` and `AIQ_CLAUDE_BIN` override them. Browser device pairing requires desktop Chrome or Edge with Web Serial support.

Complete official authorization after adding an account. Codex reports Codex usage windows, not every ChatGPT message limit. For Claude, copy its session launch command and use that account normally; its status-line feed supplies quota after a normal response. Automatic refresh does not send billed model prompts or make old data fresh.

Account profiles and credentials remain under `~/.local/share/ai-passport-quota` with private permissions. Wi-Fi input goes directly from browser memory to USB. The settings interface listens on loopback only. HTTPS device syncing opens only on the selected private address after pairing, or restores previously authorized configuration. Keep this computer and the service running while syncing. Stop syncing revokes the device token; re-pair to resume.

`npm test` validates protocol normalization, account isolation/lifecycle, stale source timestamps, local request checks and pinned HTTPS. `npm run build` compiles the frontend. Real account authorization, browser interaction and hardware require user verification; tests use synthetic providers and temporary directories.

Full bilingual operation and protocol guide: [AI quota monitor](../docs/applications/ai-quota-monitor.md).

USB pairing starts receiving immediately after opening the device, waits briefly for startup, and matches the acknowledgment to the configuration request. If pairing fails, the page distinguishes no USB input, interrupted communication, an unmatched response, and explicit device rejection. Refresh the page after an application update; re-enter Wi-Fi details and reopen the physical pairing window before retrying. Only the ESP32-C3 native USB Serial/JTAG device is offered by the port picker. Other serial tools and pairing tabs must release the device first.

On macOS, double-click `start-dashboard.command` to start the prepared application again. Keep its terminal window open while syncing. The page URL is the same; no credentials are embedded in the launcher.
