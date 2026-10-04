[简体中文](ai-quota-monitor.zh_CN.md) · English

# Provider, protocol and data contracts

## Provider data

Each of at most eight accounts has an isolated private profile under `~/.local/share/ai-passport-quota/profiles/<id>` (or `AIQ_STATE_DIR`). Existing CLI credentials are not imported. Account removal detaches state immediately; Codex/Claude logout is attempted and their profile files are retained. DeepSeek removal deletes its local key; provider-side revocation is separate.

| Provider | Source | Meaning |
| --- | --- | --- |
| Codex | Official app-server login and `account/rateLimits/read` | Codex usage; map 300/10080-minute windows to five-hour/seven-day quota |
| Claude | Official subscription login and statusline callback from a normal response | Bind the callback to the verified isolated account; cached callbacks retain source time |
| DeepSeek | `GET https://api.deepseek.com/user/balance` with a private API key | Available balance; no verified email, spending history, request count or cumulative token total |

No quota/balance refresh sends a model prompt. Preserve `observed_at`; refreshing cache or changing a snapshot revision is not a new provider observation. Each five-hour/seven-day window is independent: a missing window is unknown and hidden in both views; a valid 0% remains visible. A reset deadline passing leaves that window waiting for new source data, never automatically restoring 100%. Missing balance is not zero. Label stale/offline/expired data explicitly.

Codex also reads Credits from the officially selected rate-limit bucket and account-level banked resets from `rateLimitResetCredits.availableCount`. Optional snapshot fields are `credits: {has_credits, unlimited, balance}` and `banked_reset: {available_count}`. Credit flags are Boolean; balance is a source string of at most 32 UTF-8 bytes with no control characters, or null. Do not infer a monetary unit, convert or synthesize a balance. Reset count is a nonnegative safe integer, displayed only above zero. Credits display only when `has_credits` or `unlimited` is true; unavailable extras stay hidden independently of quota windows. Claude statusline provides no corresponding extras; do not manufacture them.

DeepSeek keys live in owner-only `profiles/<id>/deepseek/api-key.json` and never enter public state, device snapshots or USB. Amounts remain signed decimal strings, with CNY/USD entries separate. The current view selects CNY `total_balance` only, including grants and top-ups; it does not expand those parts, convert currencies or fall back to USD. A normal refresh failure retains labelled cache; key replacement clears the old wallet before verification. A local label is not an authenticated email. Per-request model usage is not account-wide portal history.

Source references: [Codex auth](https://learn.chatgpt.com/codex/auth), [app-server](https://learn.chatgpt.com/codex/app-server), [Claude auth](https://code.claude.com/docs/en/authentication), [statusline](https://code.claude.com/docs/en/statusline), [DeepSeek balance](https://api-docs.deepseek.com/api/get-user-balance/).

## Trust and transport

The settings API is loopback-only on port 4317. The private-address HTTPS listener on 4318 starts for pairing or restores previously authorized configuration. Pairing supplies a random bearer token, ECDSA certificate with IP subject alternative name and trusted time. Firmware pins the certificate and verifies the host; device HTTP does not follow redirects. Stop sync revokes the token; an address change requires re-pairing.

Wi-Fi credentials travel from browser memory directly to Web Serial, never through the companion API. Firmware accepts configuration only during a physical 120-second window; configured devices boot with it closed. USB frames start with `@AIQ:`, end with a newline and are limited to 4096 bytes. Version-1 `configure` includes an eight-digit hexadecimal request ID, Wi-Fi fields, private HTTPS base URL, token, certificate and initial time. `result` matches that ID and never echoes credentials. Start reading immediately after opening USB and distinguish timeout/no input, interrupted transport, unmatched response and explicit rejection.

## Device API

All device requests use `Authorization: Bearer <pair-token>`.

| Method/path | Contract |
| --- | --- |
| `GET /v1/snapshot` | Version 1, server time, revision, settings and authenticated accounts; at most eight accounts and 8192 bytes |
| `POST /v1/refresh` | Coalesced source-refresh request; acceptance is not proof of a new observation |
| `PATCH /v1/settings` | `refresh_seconds`: 60/300/900/1800; Boolean `auto_refresh`; optional `screen_timeout_seconds`: 0/30/60/120/300/600 |

Omitting screen timeout preserves the setting; legacy desktop state defaults to 120 seconds. Firmware retains its saved timeout when older snapshots/ACKs omit it. A timeout-only change does not reset the desktop quota timer. Settings are canonical on the service and stored on the device after acknowledgment.

DeepSeek payloads use `provider: "deepseek"`, empty `email`, `plan: "API"`, a UTF-8 `label` of at most 32 bytes, null quota windows and nullable `balance: {is_available, balance_infos}`. At most two unique CNY/USD entries contain string `total_balance`, `granted_balance`, `topped_up_balance`, each at most 20 decimal characters. Older firmware rejects DeepSeek; update it before adding that provider. See `companion/shared/contract.mjs`, `companion/server/protocol.mjs` and `main/quota_logic.h` for executable schemas.

## Refresh and screen lifecycle

The desktop has its own refresh schedule. Device screen off pauses snapshot/source/settings HTTP and Wi-Fi retry/configuration, then stops Wi-Fi. An admitted bounded request may finish before the worker stops the radio; display generations prevent obsolete results publishing or starting follow-up work. Application, network and serial workers wait for events instead of polling while asleep. LCD refresh is paused and the panel enters Sleep In with backlight off. CPU DFS permits 40 MHz while asleep; waking holds the configured maximum-frequency lock. The LVGL 5 ms tick and ADC function-key polling remain active; no MCU light/deep sleep is used.

Wake restores the display from RAM cache before reconnecting Wi-Fi, then queues one silent cache GET even with automatic refresh disabled; an already admitted bounded request finishes first, and offline wake waits for connectivity. This cache read does not start a provider refresh, show progress or postpone its deadline. After it, a pending manual refresh or enabled overdue automatic refresh uses POST and shows progress. The next source interval starts after completion. Requests canceled before admission retain their due/manual work; ordinary failures keep the bounded cadence.

The entire first waking function-key gesture is consumed. Pairing suppresses automatic screen off and resets idle time when closing. Cold-boot time stays unknown until current-boot pairing or a pinned snapshot calibrates it. Device time uses UTC+8. Battery reads are cached for thirty seconds; SOC fill does not infer charging.

## Persistent and display data

Device NVS retains pairing and a sanitized snapshot, written at most every fifteen minutes. Preserve the legacy pairing/quota-cache layouts. Device Credits and banked-reset extras are RAM-only, matched by account ID and absent after cold boot until source synchronization; no NVS structure is changed. `screen_to` is a separate timeout key. DeepSeek uses a CRC-protected `balance_cache` sidecar bound to snapshot revision, configuration identity and save time. Reject mismatched/corrupt data; do not erase unrelated NVS to hide initialization errors.

Fixed device text uses the two subset fonts in `assets/fonts/`. Arbitrary non-ASCII account identity characters fall back to `?`; exact rendering requires an ASCII label or deliberately expanded coverage. Hardware rendering, USB pairing, offline recovery, extended timing and real provider behavior need the [acceptance checks](../development/README.md#acceptance-and-reporting); current evidence is recorded in the [changelog](../CHANGELOG.md).
