[简体中文](AGENTS.zh_CN.md) · English

# Companion constraints

Read the root [instructions](../AGENTS.md) and [application contracts](../docs/applications/ai-quota-monitor.md).

- Keep the settings API on loopback, private profiles isolated, and secrets out of public state, snapshots, USB responses and logs. Do not import existing provider credentials.
- Use official Codex app-server, Claude authentication/statusline and DeepSeek balance sources. Preserve source timestamps, unknown windows, decimal strings and separate currencies; the current view selects CNY only. A DeepSeek label is not a verified email; recovery uses balance retry/key replacement.
- Wi-Fi input must never reach the API. Preserve USB request-ID acknowledgment matching and bounded transport failure handling.
- Keep the device preview at 240 × 320; its quota track is 216 × 8. Percentage text and bar fill have independent low/critical styling; the number container remains transparent. Production previews do not invent board telemetry.
- Use synthetic accounts and temporary state for shareable captures (`npm run preview:readme`). Run `npm test` and `npm run build` for behavior changes; check changed UI in a browser. Distinguish host/browser results from real account and device acceptance.
