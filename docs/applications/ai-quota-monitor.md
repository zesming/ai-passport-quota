[简体中文](ai-quota-monitor.zh_CN.md) · English

# Provider, protocol and data contracts

## Modes and ownership

| Mode | Account and refresh owner | Supported sources |
| --- | --- | --- |
| `COMPANION` | Local desktop companion; device reads its authenticated cache | Codex app-server, Claude statusline, DeepSeek balance |
| `DIRECT` | Device; phone assists setup and provider authorization | Experimental Codex device authorization, DeepSeek balance; no native Claude authorization |

Mode selection is explicit. An existing USB-paired device defaults to `COMPANION`; a device without legacy configuration defaults to `DIRECT`. Credentials and caches remain separate, and switching modes does not import CLI credentials or reuse a desktop refresh-token chain. Each mode supports at most eight accounts. Phone screens and button actions are described in [portable connectivity](../development/portable-connectivity.md).

## Provider data

In `COMPANION`, each of at most eight accounts has an isolated private profile under `~/.local/share/ai-passport-quota/profiles/<id>` (or `AIQ_STATE_DIR`). Existing CLI credentials are not imported. Account removal detaches state immediately; Codex/Claude logout is attempted and their profile files are retained. DeepSeek removal deletes its local key; provider-side revocation is separate.

| Provider | Source | Meaning |
| --- | --- | --- |
| Codex | Official app-server login and `account/rateLimits/read` | Codex usage; map 300/10080-minute windows to five-hour/seven-day quota |
| Claude | Official subscription login and statusline callback from a normal response | Bind the callback to the verified isolated account; cached callbacks retain source time |
| DeepSeek | `GET https://api.deepseek.com/user/balance` with a private API key | Available balance; no verified email, spending history, request count or cumulative token total |

In `DIRECT`, Codex reproduces the [official device-code flow](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs), then reads `https://chatgpt.com/backend-api/wham/usage` and `/wham/rate-limit-reset-credits` with an access token and `ChatGPT-Account-ID`. These paths are official-client implementation details, not a stable public third-party API. The configurable `QUOTA_DIRECT_CODEX_CLIENT_ID` defaults to the official Codex client ID `app_EMoamEEZ73f0CkXaXp7hrann`; it is not a registered OAuth client for this product. Account/workspace device-code enablement, permission to use that client and real-device compatibility remain prerequisites for accepting this experimental integration. The phone signs in on `https://auth.openai.com/codex/device` and enters the displayed code; no browser callback to the device is used. Direct DeepSeek uses the same documented balance endpoint. Claude remains available through `COMPANION` only.

No quota/balance refresh sends a model prompt. Preserve `observed_at`; refreshing cache or changing a snapshot revision is not a new provider observation. Each five-hour/seven-day window is independent: a missing window is unknown and hidden in both views; a valid 0% remains visible. Reset deadlines use a refresh icon followed by `xd xh`; banked-reset expiry uses `xd xh` followed by the localized expiry label. Positive durations below an hour display `<1h`, and day/hour values use completed hours. A deadline passing leaves that window waiting for new source data, never automatically restoring 100%. Missing balance is not zero. Label stale/offline/expired data explicitly.

In `COMPANION`, Codex reads Credits from the officially selected rate-limit bucket and account-level banked resets from `rateLimitResetCredits.availableCount`. Optional snapshot fields are `credits: {has_credits, unlimited, balance}` and `banked_reset: {available_count, next_expires_at}`. Credit flags are Boolean; balance is a source string of at most 32 UTF-8 bytes with no control characters, or null. Do not infer a monetary unit, convert or synthesize a balance. Reset count is a nonnegative safe integer, displayed only above zero. Credits display under the localized remaining-credits label only when `has_credits` or `unlimited` is true; unavailable extras stay hidden independently of quota windows. Request reset details with `excludeResetCreditDetails: false`. Set `next_expires_at` to the earliest finite expiry only when all `available_count` rows are known available Codex resets with valid or explicitly null expiry; capped, missing or invalid details leave it null. Keep the count authoritative, omit private detail rows, and show a waiting-for-update label when a cached expiry passes. In `DIRECT`, only primary windows of exactly 18000/604800 seconds map to five-hour/seven-day quota; additional limits do not replace them. Usage supplies Credits and the authoritative reset-count summary. A failed optional reset-detail read retains the fresh usage/count but leaves expiry unknown. Claude statusline provides no corresponding extras; do not manufacture them.

In `COMPANION`, DeepSeek keys live in owner-only `profiles/<id>/deepseek/api-key.json` and never enter public state, device snapshots or USB. Amounts remain signed decimal strings, with CNY/USD entries separate. The current view selects CNY `total_balance` only, including grants and top-ups; it does not expand those parts, convert currencies or fall back to USD. A normal refresh failure retains labelled cache; key replacement clears the old wallet before verification. A local label is not an authenticated email. Per-request model usage is not account-wide portal history.

In `DIRECT`, the DeepSeek key is device-owned and never returned by the setup API. A new key may be saved as pending verification; reception is not proof of successful authentication. Replacing an existing key uses a candidate, verifies it over station Wi-Fi, and commits its key, identity generation and balance only after success. Failure retains the previous credential and wallet. A label-only change does not require a new provider observation.

Source references: [Codex auth](https://learn.chatgpt.com/codex/auth), [app-server](https://learn.chatgpt.com/codex/app-server), [Claude auth](https://code.claude.com/docs/en/authentication), [statusline](https://code.claude.com/docs/en/statusline), [DeepSeek balance](https://api-docs.deepseek.com/api/get-user-balance/).

## Trust and transport

In `COMPANION`, the settings API is loopback-only on port 4317. The private-address HTTPS listener on 4318 starts for pairing or restores previously authorized configuration. Pairing supplies a random bearer token, ECDSA certificate with IP subject alternative name and trusted time. Firmware pins the certificate and verifies the host; device HTTP does not follow redirects. Stop sync revokes the token; an address change requires re-pairing.

Wi-Fi credentials travel from browser memory directly to Web Serial, never through the companion API. Firmware accepts configuration only during a physical 120-second window; configured devices boot with it closed. USB frames start with `@AIQ:`, end with a newline and are limited to 4096 bytes. Version-1 `configure` includes an eight-digit hexadecimal request ID, Wi-Fi fields, private HTTPS base URL, token, certificate and initial time. `result` matches that ID and never echoes credentials. Start reading immediately after opening USB and distinguish timeout/no input, interrupted transport, unmatched response and explicit rejection.

In `DIRECT`, provider requests use certificate-bundle verification, hostname checks and fixed HTTPS origins (`auth.openai.com`, `chatgpt.com`, `api.deepseek.com`); redirects are rejected. HTTP bodies are bounded to 32 KiB and completed headers to 16 KiB. The same-owner asynchronous transport checks a 15-second progress budget with at most one-second transport waits; this is not a measured hard total-time guarantee for every DNS/header behavior. Credentials never appear in public state, quota snapshots, logs or QR codes. JWT metadata is parsed from the verified HTTPS token response for account/user matching and expiry; this is not a claim of JWT-signature verification.

The phone setup page uses a temporary WPA2 AP during a physical 600-second window. The first QR joins Wi-Fi; the second opens `http://192.168.4.1/#s=<session-secret>`. The fragment secret moves into page memory, is removed with `replaceState`, and is not saved in browser storage. The local page uses HTTP protected by the AP session; provider traffic uses verified HTTPS. The AP closes on completion, timeout or screen off. Codex launch closes it before station-network authorization; the phone must return to Internet access to approve the official code. A pending authorization has a 15-minute deadline, bounded poll intervals and no browser callback.

## COMPANION device API

All device requests use `Authorization: Bearer <pair-token>`.

| Method/path | Contract |
| --- | --- |
| `GET /v1/snapshot` | Version 1, server time, revision, settings and authenticated accounts; at most eight accounts and 8192 bytes |
| `POST /v1/refresh` | Coalesced source-refresh request; acceptance is not proof of a new observation |
| `PATCH /v1/settings` | `refresh_seconds`: 60/300/900/1800; Boolean `auto_refresh`; optional `screen_timeout_seconds`: 0/30/60/120/300/600 |

Omitting screen timeout preserves the setting; legacy desktop state defaults to 120 seconds. Firmware retains its saved timeout when older snapshots/ACKs omit it. A timeout-only change does not reset the desktop quota timer. Settings are canonical on the service and stored on the device after acknowledgment.

DeepSeek payloads use `provider: "deepseek"`, empty `email`, `plan: "API"`, a UTF-8 `label` of at most 32 bytes, null quota windows and nullable `balance: {is_available, balance_infos}`. At most two unique CNY/USD entries contain string `total_balance`, `granted_balance`, `topped_up_balance`, each at most 20 decimal characters. Older firmware rejects DeepSeek; update it before adding that provider. See `companion/shared/contract.mjs`, `companion/server/protocol.mjs` and `main/quota_logic.h` for executable schemas.

## Local phone setup API

These routes exist only during the physical AP session and are separate from the companion's `/v1` endpoints.

| Method/path | Contract |
| --- | --- |
| `GET /` | Embedded production setup page; no credential state |
| `GET /api/state` | Sanitized mode, settings, network/account state and bounded job results; requires `X-AIQ-Setup` |
| `POST /api/command` | Bounded command with request ID; requires `X-AIQ-Setup` and same-origin `Origin`; acceptance queues work, not verified completion |

Every request must reach the AP interface from its subnet and use exactly `Host: 192.168.4.1` or `192.168.4.1:80`. An `Origin` supplied on GET must match the local origin; mutations require it. Foreign/null origins and cross-origin preflights are rejected. Commands cover saved-network selection, settings/mode changes, DeepSeek verification, Codex queue/launch, account removal and refresh/reconnect. Duplicate request IDs must carry the same payload. Public results never echo Wi-Fi passwords, provider tokens, private authorization IDs or PKCE values. See `main/quota_portal.c`, `main/quota_portable.h` and the [phone flow](../development/portable-connectivity.md).

## Refresh and screen lifecycle

In `COMPANION`, the desktop has its own refresh schedule. In both modes, device screen off pauses snapshot/source/settings HTTP and Wi-Fi retry/configuration, then stops Wi-Fi. An admitted bounded request may finish before the worker stops the radio; display generations prevent obsolete results publishing or starting follow-up work. Application, network and serial workers wait for events instead of polling while asleep. LCD refresh is paused and the panel enters Sleep In with backlight off. CPU DFS permits 40 MHz while asleep; waking holds the configured maximum-frequency lock. The LVGL 5 ms tick and ADC function-key polling remain active; no MCU light/deep sleep is used.

In `COMPANION`, wake restores the display from RAM cache before reconnecting Wi-Fi, then queues one silent cache GET even with automatic refresh disabled; an already admitted bounded request finishes first, and offline wake waits for connectivity. This cache read does not start a provider refresh, show progress or postpone its deadline. After it, a pending manual refresh or enabled overdue automatic refresh uses POST and shows progress. The next source interval starts after completion. Requests canceled before admission retain their due/manual work; ordinary failures keep the bounded cadence.

In `DIRECT`, wake first shows its own cached snapshot and reconnects; it does not issue a companion cache GET or reset a source deadline. Source reads require a pending manual request or an enabled due refresh. One network worker owns both modes, authorization, storage mutations and HTTP; direct polling uses one global cycle rather than independent per-account timers. The next interval starts when the cycle completes. Each account honors its own 429 backoff, and ordinary failures retain labelled cache. Codex refresh runs before an imminent access-token expiry or once after a 401, followed by at most one repeated quota read. DeepSeek verification and a completed login can request a source refresh independently of automatic refresh.

Already received token rotations must still commit while the display sleeps. Pending persistence retries storage only, never another token POST; cancel/removal waits for that commit. Manual sleep pauses authorization networking while its deadline continues. Setup and pending authorization suppress automatic screen off without changing the saved timeout.

The entire first waking function-key gesture is consumed. Pairing suppresses automatic screen off and resets idle time when closing. In `COMPANION`, cold-boot time stays unknown until current-boot pairing or a pinned snapshot calibrates it. In `DIRECT`, a stored time can seed the clock, but provider HTTP waits for current-boot phone time or awake SNTP synchronization; a stored timestamp alone is not current-boot calibration. An HTTPS response Date is preferred for provider observation time, with the calibrated UTC clock as fallback. Device time uses UTC+8. Battery reads are cached for thirty seconds; SOC fill does not infer charging.

## Persistent and display data

In `COMPANION`, device NVS retains pairing and a sanitized snapshot, written at most every fifteen minutes. Preserve the legacy pairing/quota-cache layouts. Device Credits and banked-reset extras are RAM-only, matched by account ID and absent after cold boot until source synchronization; the legacy NVS structures are unchanged. `screen_to` is a separate timeout key. DeepSeek uses a CRC-protected `balance_cache` sidecar bound to snapshot revision, configuration identity and save time. Reject mismatched/corrupt data; do not erase unrelated NVS to hide initialization errors.

`DIRECT` uses the separate 256 KiB `portable` NVS partition at `0x7c0000`; existing NVS/PHY offsets are preserved and the factory application partition ends before it. Versioned CRC-checked configuration, credentials and snapshot records reject invalid data. Each account stores one blob containing its logical ID, provider identity, generation, tokens/key, expiry and `refresh_inflight`. Commit this marker before a refresh POST; after a validated response, atomically replace the same blob and clear the marker. A connection failure or cancellation before HTTP headers are sent may safely clear the marker. After headers are sent, a complete HTTP 429 may clear it and back off; timeout, redirect, server error or malformed success leaves the token chain uncertain and requires reauthorization. A boot with the marker set must not replay the old token. A received replacement that fails storage remains in RAM until a storage-only retry succeeds. Provider account IDs are separate from logical display IDs.

Direct sanitized cache is separate from the companion cache, bound to provider/account ID and credential generation, and saved at most every fifteen minutes. Removed/replaced accounts cannot inherit an older generation's wallet or quota. Codex Credits/reset extras remain RAM-only in both modes and are absent after cold boot until source synchronization. The portable partition is **not encrypted**; CRC and NVS commit behavior provide corruption/commit checks, not confidentiality or protection from flash access. No secure-boot, flash-encryption or eFuse protection is claimed.

Fixed device text uses the two subset fonts in `assets/fonts/`. Arbitrary non-ASCII account identity characters fall back to `?`; exact rendering requires an ASCII label or deliberately expanded coverage. Hardware rendering, USB pairing, offline recovery, extended timing and real provider behavior need the [acceptance checks](../development/README.md#acceptance-and-reporting); current evidence is recorded in the [changelog](../CHANGELOG.md).
