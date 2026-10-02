[简体中文](AGENTS.zh_CN.md) · English

# Companion development

This is the production local companion, not the earlier design prototype.
Start with the root README and the firmware application/protocol guide.

- Keep the settings API on loopback and credentials in isolated user-private profiles. Never import the user's existing provider credentials or print raw auth/USB payloads.
- Official Codex app-server and Claude authentication/statusline are the quota sources. Do not send model prompts for quota polling, manufacture timestamps, or treat missing/expired windows as full quota.
- Wi-Fi input goes from browser memory directly to Web Serial; the local API must never receive it. Preserve request-ID acknowledgment matching and safe failure messages.
- The device preview is 240 by 320 pixels. Its native bar is 216 by 8 pixels. Low/critical styling colors number text and bar fill separately; the full-width number container must stay transparent.
- Run the local app when verification requires it. Use example accounts for shareable captures (`npm run preview:readme`); do not publish personal emails, quota data or device configuration.
- Run `npm test` and `npm run build` for behavior changes. Keep one owner per test/build. Check changed UI in an available browser; distinguish browser simulations from board acceptance.
- `.openai/hosting.json`, `worker/index.js`, `scripts/prepare-sites-build.mjs` and `tests/sites-worker.test.mjs` are retained build scaffolding. The real credential-backed app runs through `server/index.mjs` on the computer; static Sites hosting does not supply that backend. No hosting publication is implied.

The browser USB transport fix passed host stream tests but still needs a real
pairing retry. Follow the root README acceptance order and preserve these limits
when handing work to another agent.
