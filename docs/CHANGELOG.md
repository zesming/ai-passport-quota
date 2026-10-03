<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

> This fork records feature changes, validation and device acceptance here. The root README describes current functionality, usage and constraints. Inherited upstream entries are preserved below; `Unreleased` does not imply a published release.

## Unreleased

### 2026-10-03 — Device upgrade to the green battery build

After the owner's explicit flash request, installed the previously validated `ca0a27c27d49eddefeaa0efe29ce6e8d55575c04baaf93e99914033eb08c4e20` bundle on `/dev/cu.usbmodem1101`. Before writing, the actual partition table matched the bundle's SHA-256 `420931e3f5af072d899b5357c043fd9f283bf165fc9df6113b6d43d82b48a06d`. Verified component writes at `0x0` / `0x8000` / `0x10000` left NVS and PHY regions untouched; no full-chip erase or rebuild was performed. Existing cache and pairing were restored.

Build: **PASS** and Host tests: **PASS**, reusing the complete gate for that exact artifact; archive verification passed again before writing. GitHub static and firmware checks both passed for code commit `17c2ed9`.

Device tests: **PASS for component verification and bounded startup**. A 30-second capture showed application version `b39dc02-dirty`, boot ELF prefix `795791a0e` uniquely matching the retained ELF `795791a0eb12d27f486a178ce454a255034470129700a56f541c71848e3a6971`, application readiness, cache restoration and CW2017 detection. No panic, assertion, stack-canary error or watchdog was observed. The companion received authenticated requests after flashing. The serial port was released; private logs remain local.

Unverified: the physical green icon/no numeric percentage, manual sleep/wake and traffic suppression/resumption, automatic sleep and extended/offline timing. User observations are pending. Current persisted refresh and screen-off intervals are both five minutes, with automatic refresh enabled. This bundle is now the latest flashed image; earlier unflashed entries below describe their original validation stages.

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
