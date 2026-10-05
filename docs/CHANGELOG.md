<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

> This fork records feature changes, validation and device acceptance here. The root README describes current functionality, usage and constraints. Inherited upstream entries are preserved below; `Unreleased` does not imply a published release.

## Unreleased

### 2026-10-05 — Companion startup and network restoration

The reported wake/manual-refresh failures were traced to the desktop companion not running after a computer restart. Restarting it restored the paired endpoint and provider collection; the owner confirmed device refresh recovered. Added a user-level macOS login service with restart supervision, install/remove commands, preserved configuration on reinstall and bounded startup checks. A loaded-job replacement retries transient launchd unregister errors. No credentials are stored in its plist; logs remain private.

The companion retries restoration of the saved paired endpoint every 15 seconds when unavailable. It retains the saved address, token and certificate, respects explicit stop, and waits for re-pairing if the address changes or certificate is unusable. A guard inside the serialized restore operation prevents an overlapping retry from disabling authentication on an active listener.

Build: **PASS for the companion**, Vite production build. Host tests: **PASS**, 54 companion tests, repository checks and script syntax; restoration tests cover late network availability, queued overlap, temporary bind failure, unusable certificate, stop and shutdown. Independent review found no remaining material issue. The installed login service passed reinstall and SIGTERM recovery checks, retaining accounts, settings, CLI availability and pairing. Device tests: **PASS for owner-observed refresh recovery before service installation**. Unverified: an actual logout/login or reboot with the new service, and physical wake after the final supervised restart. Firmware is unchanged; no flash or rebuild was performed.

### 2026-10-04 — Compact countdown firmware installed

Installed the verified `80ce1005…` bundle after the owner's request. The device partition table matched the archive; writes at `0x0` / `0x8000` / `0x10000` passed all three component hash checks. NVS and PHY regions were untouched.

Build and Host tests: **PASS**, reusing this exact bundle's complete gate and 52 companion tests. Archive verification passed again before writing; source commit `eff037c` passed both GitHub [static checks](https://github.com/zesming/ai-passport-quota/actions/runs/37180800306) and [firmware checks](https://github.com/zesming/ai-passport-quota/actions/runs/37180800273). Device tests: **PASS for writes, startup and authenticated communication**. The 30-second capture matched version `7b76eb5-dirty` and the ELF prefix to `55520faeb409259d7d0464ee8529e4e19379e641f35ec02620e0e43e99751622`; application readiness, cache restoration and battery detection were observed without crash markers. Authenticated device requests resumed, and companion settings and account count remained unchanged. The serial port was released; raw logs remain local.

Unverified: physical refresh-icon/countdown rendering awaits owner observation, including the compact expiry row after synchronization. This bundle is now the latest installed image; earlier entries preserve their original validation stages.

### 2026-10-04 — Compact countdowns

Quota resets use a refresh icon followed by `xd xh`; available reset expiry uses the same duration followed by the localized expiry label. Durations show completed hours, with `<1h` for a positive remainder below one hour. Both subscription views share this presentation. The device's 12-pixel font falls back to the built-in refresh glyph. Credits preserve the source string; screenshots now use integer examples without imposing an integer-only contract.

Build: **PASS** — complete ESP-IDF 5.5.3 gate and verified archive; app 1,499,856 bytes. Host tests: **PASS** — firmware/runtime/font checks, 52 companion tests and Vite build. Browser checks passed for two-window Codex, Pro without a 5-hour window, Claude and text fit; synthetic screenshots were updated.

Verified bundle: `build/firmware/80ce100545ad885e002dd2c16481f38e1a225d0439b4383f75df053a110c51e3/`. Matching ELF SHA-256: `55520faeb409259d7d0464ee8529e4e19379e641f35ec02620e0e43e99751622`; app version `7b76eb5-dirty`. Device tests: **NOT RUN** — not flashed. Physical countdown and fallback-icon rendering remain unverified; the installed image remains `a3251c5c…`. NVS layouts and refresh behavior are unchanged.

### 2026-10-04 — Reset countdowns and expiry labels

Both subscription views show time remaining until each quota reset. Uncalibrated device clocks wait for synchronization; elapsed deadlines still wait for new source data. Credits use the localized label for remaining credits. Codex polling requests reset details and displays the earliest expiry only when the available rows are complete and valid. Counts remain authoritative; missing/capped details hide the date, and cached expired dates wait for an update. Private reset rows are excluded from public state; device extras remain RAM-only with unchanged NVS layouts.

Build: **PASS** — complete ESP-IDF 5.5.3 gate and verified archive; app 1,500,944 bytes. Host tests: **PASS** — firmware/runtime/font checks and 52 companion tests; Vite build passed. Independent review identified and verified the cold-boot clock fix. Browser checks passed for Pro, two-window Codex, Claude, expired/unknown dates, short countdowns and 390-pixel layout; synthetic screenshots were updated. The updated live companion returned an earliest expiry from the real account while preserving accounts, settings and pairing.

Verified bundle: `build/firmware/2696d04b1e14ec43ff9a7941b4ac765e36db8757fcd5ecb12d1a4f824a4d563e/`. Matching ELF SHA-256: `a6235641c1cfc421f038cbf365837a9a7289633d819475339170987b92cc07be`; app version `a6b43a2-dirty`. Device tests: **NOT RUN** — this bundle has not been flashed; the latest installed image remains `a3251c5c…`. Unverified: physical countdown/expiry rendering and cold-boot clock behavior. Raw account data and logs remain local.

### 2026-10-04 — Device upgrade and acceptance

Installed the verified `a3251c5c…` bundle described below after the owner's request. The device partition table matched the archive; all three component writes at `0x0` / `0x8000` / `0x10000` passed hash verification. NVS and PHY regions were untouched. The companion was restarted with the updated collector, preserving accounts, settings and pairing.

Build and Host tests: **PASS**, reusing this exact bundle's complete gate; archive verification passed again before writing. Device tests: **PASS for component writes, startup and owner-observed behavior**. The boot version `d18d939-dirty` and ELF prefix `30f7bf01a` match the retained artifact; the cache and battery gauge initialized without an observed crash during the 30-second startup capture. Real Codex Credits and available reset counts were returned and displayed. The owner confirmed screen off, cached wake without account switching and restored Wi-Fi connectivity; a bounded observation also captured reconnection and authenticated device requests.

Initial extras appeared after the companion restart and subsequent cache synchronization. Extras remain in RAM across screen off; cold boots obtain them again from the companion. Unverified: measured sleep current or battery-life improvement, extended/offline recovery and active-request sleep transitions. Raw logs remain local.

### 2026-10-04 — Codex extras, independent windows and screen-off power controls

Subscription windows display independently in both views: missing windows are hidden, real 0% remains visible, and expired windows wait for new source data. Codex reads Credits from the selected official bucket and account-level banked-reset availability; show Credits only when available/unlimited and reset counts only above zero. Preserve source balance strings without assigning currency. Claude has no equivalent statusline extras. Device extras stay in RAM and remain hidden after cold boot until synchronization; NVS layouts are unchanged.

With the owner's authorization to turn Wi-Fi off during screen off, the device stops Wi-Fi, sends LCD Sleep In, pauses display refresh and application/network/serial polling, and permits CPU DFS down to 40 MHz. Awake operation holds the maximum-frequency lock. ADC function-key sensing and the LVGL 5 ms tick stay active; no MCU light/deep sleep is used. Wake restores cached display before reconnecting and silently reading the desktop cache, retaining source deadlines and reserving progress for actual source-refresh work.

Build: **PASS** — complete ESP-IDF 5.5.3 gate, component/merged images and debug archive verified; application 1,498,352 bytes. Host tests: **PASS** — firmware/BSP/runtime checks, 51 companion tests and Vite production build. Independent reviews found and resolved a layout overlap and stale Wi-Fi-event recovery races. Synthetic browser checks cover both/single/missing windows, real 0%, expired Claude data, unavailable/only Credits, conditional reset counts, account switching and proportional 216 × 8 tracks. Documentation captures were updated without private data.

Verified bundle: `build/firmware/a3251c5c35b719d4465669df606aef3f174fbbe6e49ad6e5b9ad3c942b8a3c35/`; directory name is the full-image SHA-256. Matching ELF SHA-256: `30f7bf01ab7559989809d4a48bfe67de8f9a6910f9554808aac164516ae1c82e`. App version: `d18d939-dirty`. Device tests: **NOT RUN** — no new flashing or current measurement. Unverified: real-account Codex extras, physical window/extras layout, Wi-Fi stop/reconnect, LCD sleep/wake, key wake timing, offline/active-request transitions, and measured current or battery-life improvement. Previous device acceptance does not validate these changes.

### 2026-10-03 — Project simplification

Reduced the maintained project from 355 to 135 files. Removed uncompiled hardware demos and pixel UI, unused audio/codec and Bluetooth configuration, their tests/stubs, duplicated skill/documentation scaffolding, static Sites packaging and upstream community/release/sync workflows. The active quota app, required BSP, provider assets/licenses and meaningful behavior tests remain. Removed files were retained locally in a recoverable cleanup folder.

README now covers current use and screenshots; concise agent instructions and one developer guide link to the provider/protocol, hardware and asset contracts. Firmware shares one refresh-interval validator and drops write-only UI/event fields. Companion shares browser/server settings and address rules, uses one settings-action path and one public HTTP-error map, and removes dead branches. Runtime C tests share extraction/compilation helpers; repository checks retain links, bilingual peers, Action pins, secrets and conflict markers without unused policy machinery.

Dependencies stay at their previous versions, with LVGL explicitly pinned to 9.5.0. Disabled unused LVGL examples/demos and removed the codec dependency. The final fresh application image is 1,488,912 bytes, 7,296 bytes smaller than the currently flashed image; compile steps decreased from 1,962 to 1,547. NVS formats, partition layout, quota/balance meaning, private profiles, TLS/USB protections and sleep/wake deadlines are unchanged.

Build: **PASS** — ESP-IDF 5.5.3 build, component/merged-image checks and debug archive verification. Host tests: **PASS** — retained firmware/BSP/repository/archive suites and all 47 companion tests; Vite production build passed. Independent code review found no blocking regression. Browser checks with synthetic accounts confirmed dialog reset, mouse/OK settings routes, proportional bars and CNY-only balance display.

Verified local bundle: `build/firmware/32b4b7debb6394fb34d627c73b59da117f4cac2a01e0d670fbdce4a58c6e75d0/`; full-image SHA-256 is the directory name. Matching ELF SHA-256: `2c938c0cd7cb455632f7997260efb69738065175d427099dd69cd8dd87d8b04e`. Application version: `c094fb1-dirty`. Device tests: **NOT RUN** — this bundle was not flashed. Unverified: physical regression of the simplified build and the outstanding real-provider/USB/offline checks below. The last flashed image remains `6ed4d397…`.

### 2026-10-03 — Device upgrade and silent wake acceptance

After the owner's explicit request, installed the exact verified `6ed4d397dc3a19337873d5c0bc1a7fb86dc663364cd5afad9960916bfcbda50e` bundle on `/dev/cu.usbmodem1101`. The actual partition table matched SHA-256 `420931e3f5af072d899b5357c043fd9f283bf165fc9df6113b6d43d82b48a06d`. All three component writes at `0x0` / `0x8000` / `0x10000` were hash-verified. NVS and PHY regions were untouched; no full-chip erase or rebuild was performed.

Build: **PASS** and Host tests: **PASS**, reusing the complete gate for this exact artifact; archive verification passed again before writing. Source commit `4204ee3` passed both GitHub [static checks](https://github.com/zesming/ai-passport-quota/actions/runs/37114083133) and [firmware checks](https://github.com/zesming/ai-passport-quota/actions/runs/37114083130).

Device tests: **PASS for verified writes, startup, authenticated communication and observed silent wake**. A 30-second startup capture matched version `1fb05cb-dirty` and boot ELF prefix `9656324ec` to retained ELF SHA-256 `9656324ecc89a8fe922f4214709336ef5e4937aafa47a2879a5d2e2f563c0d0d`; the application became ready, restored its quota cache and detected CW2017. No panic, assertion, stack-canary error or watchdog was observed. The companion received authenticated requests after flashing; pairing and the saved five-minute refresh/screen-off settings remained available, with automatic refresh enabled. Following the requested long-DOWN sleep, approximately 25-second wait and function-key wake test, the owner confirmed silent wake before the deadline and no account switch. The serial port was released; private logs remain local.

Unverified: extended/offline timing, overdue automatic refresh and cancellation recovery on the physical board. The new bundle is now the latest flashed image; earlier entries below describe their original validation stages.

### 2026-10-03 — Silent wake synchronization and preserved refresh deadlines

The owner confirmed manual screen off, function-key wake and wake-key suppression on the `ca0a27c…` firmware, but reported a refreshing prompt without an apparent source update. The previous wake path always sent a provider-refresh POST before reading the snapshot, regardless of the next deadline. The companion's `202` acknowledgment schedules asynchronous provider work; unchanged values or a snapshot revision alone do not prove new source data. Claude refresh continues to read its existing statusline cache and preserves the source timestamp.

Waking now immediately reads the companion cache silently, including with automatic refresh disabled. It no longer forces a source POST or resets the source deadline. Manual requests and enabled automatic refreshes that are due use the existing progress path; overdue work follows the wake GET. A source attempt canceled before HTTP admission preserves the due deadline and any consumed manual request. Genuine local/HTTP failures retain the existing cadence instead of retrying every worker loop. Screen-off gates, stale-generation rejection and offline wake recovery remain intact; no stored-data layouts or partitions changed.

Build: **PASS** — the complete `./tools/validate.sh` gate passed with ESP-IDF 5.5.3, merged-image checks and retained debug-bundle verification. Host tests: **PASS** — all repository/workflow and host checks passed. The actual refresh/snapshot functions and network-worker harness cover silent wake, repeated wakes before a deadline, overdue wake, slow cache reads, manual-request preservation, cancellation before admission, ordinary-failure cadence and reconnect/generation gates. Independent review found no remaining actionable issue after the cancellation fix. Companion source is unchanged.

Verified bundle: `build/firmware/6ed4d397dc3a19337873d5c0bc1a7fb86dc663364cd5afad9960916bfcbda50e/`; full-image SHA-256 `6ed4d397dc3a19337873d5c0bc1a7fb86dc663364cd5afad9960916bfcbda50e`; matching ELF SHA-256 `9656324ecc89a8fe922f4214709336ef5e4937aafa47a2879a5d2e2f563c0d0d`. The application is 1,496,208 bytes, merged image 1,561,744 bytes, build version `1fb05cb-dirty`. This working-tree build precedes its source commit. Firmware and private logs remain local.

Device tests: **NOT RUN for this fix** — read-only discovery found `/dev/cu.usbmodem1101`; it was not opened or reset, and no firmware was written. The latest flashed image remains `ca0a27c…`. Unverified: physical silence of the wake prompt before a source deadline, immediate cache synchronization, overdue automatic/manual refresh and canceled-request recovery on the new build.

### 2026-10-03 — Device upgrade to the green battery build

After the owner's explicit flash request, installed the previously validated `ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20` bundle on `/dev/cu.usbmodem1101`. Before writing, the actual partition table matched the bundle's SHA-256 `420931e3f5af072d899b5357c043fd9f283bf165fc9df6113b6d43d82b48a06d`. Verified component writes at `0x0` / `0x8000` / `0x10000` left NVS and PHY regions untouched; no full-chip erase or rebuild was performed. Existing cache and pairing were restored.

Build: **PASS** and Host tests: **PASS**, reusing the complete gate for that exact artifact; archive verification passed again before writing. GitHub static and firmware checks both passed for code commit `17c2ed9`.

Device tests: **PASS for component verification and bounded startup**. A 30-second capture showed application version `b39dc02-dirty`, boot ELF prefix `795791a0e` uniquely matching the retained ELF `795791a0eb12d27f486a178ce454a255034470129700a56f541c71848e3a6971`, application readiness, cache restoration and CW2017 detection. No panic, assertion, stack-canary error or watchdog was observed. The companion received authenticated requests after flashing. The serial port was released; private logs remain local.

The owner subsequently confirmed manual screen off, function-key wake and consumption of the wake key. A wake refreshing-prompt issue was reported and is addressed in the newer unflashed fix above. Unverified: the physical green icon/no numeric percentage, traffic suppression/resumption, automatic sleep and extended/offline timing. Current persisted refresh and screen-off intervals are both five minutes, with automatic refresh enabled. This bundle is the latest flashed image; earlier unflashed entries below describe their original validation stages.

### 2026-10-03 — Green proportional battery indicator

The owner clarified the requested appearance: green battery fill that stays at the actual remaining level. Removed the visible numeric percentage from the firmware and web preview, used `#34C759` for proportional fill, and aligned the icon with the clock and Wi-Fi status. An unavailable reading has a diagonal mark rather than appearing as a measured empty battery. Green is fixed styling; no charging state or looping charge animation is inferred. Updated all four documentation screenshots using isolated example accounts.

Rechecked the public factory identity-card firmware to refine the earlier BSP-only audit. The [official default manifest](https://ai-passport.folotoy.cn/assets/firmwares/ai-passport-default/manifest.json) identifies version `1.2.2`, commit `5f3f673`, hardware `v1.0.0`. Its [application image](https://ai-passport.folotoy.cn/assets/firmwares/ai-passport-default/trae_card.bin) is 2,678,880 bytes, SHA-256 `d911b86737818b624c5621d2bc7e344e3083cc01a06d6335b0153725349db555`, embedded ELF SHA-256 `e1a62f0e614256247471be9ae7a81ba58b453e2571fd4b4582491bae97f46ee0`. Read-only RISC-V disassembly maps its battery timer (`0x4200d3a4`) through SOC sampling/EMA smoothing to renderer `0x4200faee`; this path changes fill and color from battery level. Its additional ID ADC getter (`0x42017098`, registers `0x0E–0x0F`) is called during initialization for a diagnostic voltage log, not by this runtime status-bar path. No verified charging source was found in that path. These findings apply to the checked image, not all factory versions or board revisions; the owner's original factory indication has not been matched to an exact build. Research files and raw logs remain outside Git.

Build: **PASS** — complete `./tools/validate.sh` with ESP-IDF 5.5.3 and debug archive verification. Host tests: **PASS** — all repository/firmware host checks, plus 47 companion tests and its production build. Browser checks confirmed home, settings, DeepSeek and pairing previews; the synthetic 76% sample retains exactly 76% of the web fill track, green color and no visible battery number. Screenshots are web simulations, not board evidence.

The verified local bundle is `build/firmware/ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20/`, full image SHA-256 `ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20`, ELF SHA-256 `795791a0eb12d27f486a178ce454a255034470129700a56f541c71848e3a6971`. The app is 1,496,512 bytes; the merged image is 1,562,048 bytes. This working-tree build reports `b39dc02-dirty`. Firmware remains local and uncommitted.

Device tests: **NOT RUN** — no hardware/serial access or flashing, as requested. Unverified: physical rendering of the new icon, the preceding screen-aware synchronization feature, and a verified charging-state source if a future design depends on one. The last flashed image remains `c0eb0a89…`.

### 2026-10-03 — Screen-aware synchronization

Device screen-off state pauses snapshot polling, source-refresh requests, settings HTTP and explicit Wi-Fi retry/configuration work while keeping cache, pairing data and function keys available. A wake immediately queues one coalesced refresh/snapshot cycle even with automatic refresh disabled; any already admitted bounded request finishes first; an offline wake is retained until connectivity recovers. Normal polling and enabled automatic refresh resume from the wake cycle. The companion keeps its separate desktop refresh schedule. Already admitted Wi-Fi initialization may finish; HTTP work stays bounded; display transition generations prevent stale results and follow-up requests after sleep or rapid sleep/wake.

Charging-state audit: the current CW2017/BSP exposes SOC and voltage, with no defined charger-status input in the board pin map or a supplied schematic. The [official charging guide](https://ai-passport.folotoy.cn/en/guides/getting-started/) describes the physical green indicator. A real charging animation remains blocked on a documented MCU-readable CHG/STAT signal; USB attachment, SOC rise and voltage are not used as a substitute.

The user requested development and validation **without flashing**. No device connection or firmware write is part of this iteration; previous board observations do not validate this new sleep/sync behavior.

#### Validation

Build: **PASS** — the complete `./tools/validate.sh` gate passed with ESP-IDF 5.5.3, including the merged image and debug archive. Host tests: **PASS** — repository/workflow checks and all host suites passed, including the four runtime harness tests executing actual HTTP admission, snapshot publication, refresh/settings functions and the network loop. Coverage includes sleep suppression, automatic-refresh-disabled wake, offline recovery, fast disconnect/reconnect, cancelled GET preservation, stale generations, settings deferral, source timestamps, missing windows, completion-based cadence, and 135 non-ASCII glyphs. A new pre-GET reconnect regression failed before the fix and passed afterward. Independent final review found no remaining actionable issue. GitHub static and firmware checks both passed for code commit `f557281`. The local working-tree firmware was built before that code commit. Companion source was unchanged; its previous validation is recorded below.

The verified local bundle is `build/firmware/2a795a6280ca8f8b74bfa02966a03d4c8bb72e0c782f5ebdfb1789e49af261ac/`, full image SHA-256 `2a795a6280ca8f8b74bfa02966a03d4c8bb72e0c782f5ebdfb1789e49af261ac`, ELF SHA-256 `c420668eea7f7f393ea267fb20c0aa65bd687046b9f3fea4dda681d3db038f8c`. The app is 1,496,576 bytes; the merged image is 1,562,112 bytes. The working-tree build reports `3c823a3-dirty`. Firmware and raw logs remain local and uncommitted.

Device tests: **NOT RUN** — no hardware/serial access or flashing. The previous `c0eb0a89…` bundle remains the last flashed image. Unverified: physical sleep/wake traffic suppression, offline recovery and refresh timing on the new firmware, runtime memory/battery behavior, and a real hardware charging-state interface. The charging-animation requirement is not implemented pending that interface.

### 2026-10-02 — AI Passport Quota

Consolidated the firmware and local companion in the owner's private repository, added isolated documentation screenshots, and documented account setup, USB configuration and development entry points. Added screen controls and status indicators, corrected refresh-state and quota-preview rendering, and integrated DeepSeek with a single RMB available balance.

Checked the official DeepSeek API documentation: the [balance endpoint](https://api-docs.deepseek.com/api/get-user-balance/) supports available balance; no documented account-wide endpoint was found for cumulative/period spending, request counts or historical token totals. [Model response usage](https://api-docs.deepseek.com/api/create-chat-completion/) is per request. Portal statistics were deferred pending a verified data source. Removed grant/top-up details from both displays and retained source data compatibility.

#### Validation and device acceptance

Companion validation: **47/47 tests PASS**, production build PASS. Browser checks confirmed a single RMB available balance without grant/top-up details, proportional quota fills, unknown board telemetry in production, and saving the screen timeout. Documentation screenshots use isolated examples.

GitHub static and firmware checks both passed for code commit `e0b56c8`.

Latest firmware validation on **2026-10-02**: the complete gate passed, including the ESP-IDF build, merged image/debug archive verification, all host checks, refresh-state tests, NVS compatibility tests and coverage for 135 non-ASCII glyphs. The new screen-off, display-status and single-RMB-balance DeepSeek firmware was **flashed and boot-checked**; the user confirmed a normal home page, long-DOWN screen off and function-key wake. Full feature acceptance is still pending. A real DeepSeek key and response have not been tested; the balance tests use synthetic responses.

The current flashed debug bundle is `build/firmware/c0eb0a8988a819ae43a2cb4b3e388747e14e79c6a667d7916a2fb73a04123de4/`, with ELF SHA-256 `4a87f07e1535d9ea084a811aeff055dbcf8fdc4ea3e5fb4ae2cee8ff42e18a8f`. It was built from the working tree (`5ef51a1-dirty`), before the code was committed as `e0b56c8`. On 2026-10-02, the actual device partition table matched the archive, and verified bootloader/partition/application writes at `0x0` / `0x8000` / `0x10000` retained NVS and PHY regions. No full-chip erase was performed. A bounded 30-second startup capture matched the application version and boot-reported ELF prefix, observed display initialization, application readiness and restored quota cache, and showed no panic/watchdog. A subsequent authenticated HTTPS poll was received by the companion. One initial Wi-Fi connection-in-progress message preceded successful synchronization. Raw logs remain local and the serial port was released.

Earlier hardware observations confirmed readable Chinese on the initial pairing page, normal two-minute window expiry and boot without observed crashes. One real Codex profile supplied weekly quota; its source omitted the 5-hour window.

Those earlier hardware observations used the pre-consolidation full image `690229c2dca7ec2ee0a794d1c4f8625468e4feedab54507b7c3cae0448b1af7c`, with ELF SHA-256 `15a33663935734a7df19fd553db87e461f53d1a22e2984973342ca8df3bc0858`. Current upgrade observations refer to the newer bundle above. Always match the actual image's ELF before decoding a crash.

**Remaining USB acceptance task:** reproduce browser USB pairing on the actual board. A reported 15-second timeout prompted eager receive, a bounded startup wait, a 4096-byte stream buffer and specific errors; the host checks pass, but the real retry has not been confirmed. This upgrade restored existing pairing and does not prove that browser re-pairing works. Safe short and production-sized invalid frames returned matching acknowledgments over native serial. Opening a serial monitor can reset the board, so avoid competing tools and distinguish a reset from pairing expiry.

This iteration adds configurable screen off, wake-gesture suppression, real board Wi-Fi/battery indicators, a clock calibrated during the current boot, display-only plan capitalization, monochrome OpenAI marks, and DeepSeek balances. Passive cache polling no longer displays a source-refresh busy message. DeepSeek needs this new firmware; older firmware rejects that provider.

The web low/critical quota preview also had a full-width colored number background; it is fixed and browser-checked. Keep the number background transparent and fill only the remaining proportion. Do not present stream simulations or web previews as physical acceptance.

The upgrade confirmed initial Wi-Fi/HTTPS communication, restored quota cache, a normal home page, manual screen off and function-key wake. Continue with browser re-pairing, Claude authorization and real quota, all other screens/buttons/logos, settings round-trip, extended offline/reboot cache behavior, timed automatic screen off, DeepSeek balances and runtime heap during TLS. Keep unknown/expired windows unknown, preserve source timestamps, and never invent fresh quotas.

### Inherited upstream entries

- Added the supplied 80-byte CW2017 profile for the specified 520 mAh cell, including content/update-flag checks, verified writes, the required restart sequence, and bounded SOC-readiness polling.

- Expanded the environment bootstrap document: added Espressif's Git service mirror (`git.espressif.com.cn`) as the preferred mainland-China route for ESP-IDF v5.5.3 and its submodules, documented submodule long-wait/timeout handling, in-place repair, and the pinned-commit shallow fetch for large submodules such as `esp32-wifi-lib`, warned about stale per-repository Jihulab `insteadOf` residue, and added the official offline release archive as a last-resort fallback (learned from `esp-mosaico/esp-mosaico-vibe`).

- Reorganized the documentation by function area with a dual entry point: the root `AGENTS.md` is now a thin router (hard constraints + task routing only) and the detailed AI workflow lives in `docs/development/ai-guide.md`; `agent-guide.md` was folded in. `docs/development/` gained a second level (`engineering/`, `ci/`, `release/`), and the `plays/` application archive and `experiences/` moved into a `docs/reference/` area with a dedicated README. Removed `docs/software-design/` (empty scaffold); folded the three `assets/{fonts,images,music}/README` leaves into the `assets/` README; flattened the six `project-completion` sub-documents into a single file; and unified each directory to a single README, eliminating every `INDEX` file and a duplicated experience index. All cross-references and bibliographic links were updated; no content was dropped.

- Removed the obsolete app/test partition at `0x700000` and its related
  bootloader, validation, and documentation requirements. The fixed protected
  `cardid` partition and its CI checks remain unchanged.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
