[简体中文](ai-quota-monitor.zh_CN.md) · English

# Provider, protocol and data contracts

## Ownership and sources

Passport owns one account catalog, Wi-Fi list, selection and refresh/sleep settings. At most eight accounts are active; eight additional historical descriptors can remain pending. Each row binds a logical ID/provider/source generation to either a device credential slot or a collector endpoint epoch/remote ID. Activity changes preserve credentials; source replacement requires explicit authorization and never merges by email. Phone and computer use the same [device setup page](../development/portable-connectivity.md).

| Source | Provider | Contract |
| --- | --- | --- |
| Device | Codex | Experimental official-client device-code flow; verified HTTPS usage and optional reset details |
| Device | DeepSeek | Documented `GET https://api.deepseek.com/user/balance`; key validation before replacement |
| Optional collector | Codex | Official app-server login and `account/rateLimits/read` |
| Optional collector | Claude | Isolated official subscription login/statusline callback during normal usage |
| Optional collector | DeepSeek | Documented balance API using an isolated private key |

No refresh sends a model prompt. Preserve provider `observed_at`; a cache read/revision change is not a new observation. Missing 5h/7-day windows are hidden; real 0% is visible. A passed reset deadline waits for source data instead of restoring 100%. Display reset countdown as `🔄 xd xh`, reset expiry as `xd xh` plus the localized expiry label, and sub-hour positive durations as `<1h`.

Codex device flow reproduces [official client code](https://github.com/openai/codex/blob/7f892275e31002f0422477c6219189284560e689/codex-rs/login/src/device_code_auth.rs), using configurable `QUOTA_DIRECT_CODEX_CLIENT_ID` and fixed `auth.openai.com`/`chatgpt.com` origins. Usage endpoints are client implementation details, not a stable public third-party API; this project is not a registered OAuth integration. Native windows map only exact 18000/604800-second durations. Credits/count come from usage; optional reset-detail failure leaves expiry unknown without discarding fresh usage. Display credits under the localized remaining-credits label, preserving the source string without inferred currency. The earliest banked-reset expiry is shown only when all available detail rows are known; positive reset counts remain authoritative. Claude supplies no equivalent extras.

DeepSeek displays original decimal CNY `total_balance`, including grants/top-ups without expanding them or converting/falling back to USD. A local alias is not an authenticated email. The API supplies no spending history, request total or cumulative token total. Device key replacement validates a candidate before committing; failure retains the old key and labelled balance. Desktop keys stay in owner-only isolated profiles and never enter device provisioning/snapshots.

Primary references: [Codex auth](https://learn.chatgpt.com/codex/auth), [app-server](https://learn.chatgpt.com/codex/app-server), [Claude auth](https://code.claude.com/docs/en/authentication), [statusline](https://code.claude.com/docs/en/statusline), [DeepSeek balance](https://api-docs.deepseek.com/api/get-user-balance/).

## Transport and commands

The optional collector has loopback settings on **4317** and pinned private-address HTTPS on **4318**. Device requests use a pairing bearer token, hostname verification and no redirects. `GET /v1/snapshot` returns at most eight accounts/8192 bytes; `POST /v1/refresh` accepts coalesced source work. Collector settings are not authoritative for Passport. Incoming rows update only current catalog bindings; unknown rows require explicit import. Collector failure affects its rows only.

USB configuration uses the existing physical 120-second pairing window and bounded `@AIQ:` newline frames. Version-1 `configure` includes request ID, Wi-Fi, endpoint/token/certificate/time; response IDs must match and never echo secrets. Modern firmware saves the complete collector endpoint and epoch in the unified model. Changing the endpoint marks old bindings `source_changed`; rebind requires verified remote ID/provider and explicit confirmation. A saved endpoint ACK does not prove candidate Wi-Fi validation. Wi-Fi travels from browser memory directly to USB, never through the computer API. USB has no permanent reader/task.

USB v2 uses the same physical window and owner. `session_open` binds one opener request ID to a fresh 32-character hex `session_id`; only that opener can retry without extending the fixed window. `state_get` returns common sanitized state; `command` contains the flat v1 body with matching inner/outer request IDs. `collector_configure` accepts only a private pinned endpoint, using the same four-job receipts and verified model writes; it does not queue certificates or copy desktop provider credentials. Invalid/expired sessions never return the current session ID. Inbound frames remain 4096 bytes, commands 2048, state 16384 plus framing; the browser bounds received lines at 32768 bytes.

Idle USB preserves STA and native HTTP. Entry/partial-frame scratch gates new phases; scratch is wiped/freed before TLS. Partial frames expire after three seconds. The browser permits one unconfirmed mutation, retains exact bytes/ID/UTC for at most one identical retry in the same window, and never automatically replays across reopen/reboot. State reads cannot evict job receipts. A 90-second browser response budget accommodates multi-phase owner turns without claiming a hard DNS deadline. USB state exposes only login state, the fixed official verification URL/user-facing code and sanitized errors. AP state keeps its original disclosure boundary. Both transports serve the same external page modules with self-only scripts.

Device providers use certificate-bundle verification and fixed origins. Responses are bounded to 32 KiB, completed headers to 16 KiB. The asynchronous transport uses a 15-second progress budget and at most one-second waits; this is not a measured total deadline for all DNS/header behavior. Reserve a token POST response after TLS handshake and before sending headers; admission failure cannot send the grant. Release the sole-owned POST body when response starts, then parse JWT metadata sequentially after releasing the envelope/body. Metadata parsing is not independent JWT signature verification.

The AP setup API accepts only the AP interface/subnet, exact Host and session header `X-AIQ-Setup`. Mutations require exact same-origin Origin; any supplied GET Origin must also match. No CORS or cross-origin preflight. `GET /` contains no private state; `/api/state` returns sanitized accounts/history/discovery/network/settings/jobs; `/api/command` accepts flat v1 JSON with an eight-character lowercase-hex request ID and optional UTC time. Limit bodies to 2048 bytes, reject duplicate/unknown fields/types/embedded NULs, and deduplicate matching IDs. A 202 means queued, not completed.

Commands cover network_save/network_activate, settings_save, codex_queue/codex_launch, deepseek_save, account_remove/account_deactivate/account_activate, external_import, setup_close, refresh/reconnect and operation_cancel. `account_activate` may atomically swap `replace_active_id`; stale collector rows require confirmed rebind. `mode_select` is obsolete and unsupported. Commands capture configuration generation internally. Public state/QR/logs never expose provider tokens, PKCE values, Wi-Fi passwords or private endpoint credentials. Only physical setup screens show the temporary AP/password/session key.

## Persistence and recovery

Preserve existing partitions and v1 record bytes. The portable NVS partition remains **0x7c0000 / 256 KiB**. `model_v2` is one CRC-checked blob containing unified configuration, at most sixteen compact descriptors, the complete legacy endpoint and one intent. A monotonic sequence and exact readback determine applied/not-applied/unknown; do not assume multi-key transactions from `nvs_commit`. Unknown writes retain their exact candidate and gate all network/mutations behind storage recovery. Typed reads distinguish missing, invalid, I/O, memory and busy; only genuine missing/valid tombstones are free slots.

Adding or replacing native authorization saves an intent before admission, then saves the unchanged v1 credential bundle, then commits the model binding/clears intent. Reboot either finishes an exact validated target tuple without HTTP, or clears an interrupted intent only when the exact prior state matches. Conflicts preserve ownership. Never replay a one-time exchange. Native removal removes the descriptor with a deletion intent, writes the exact next-generation tombstone, then clears intent. Generations never wrap. Existing native reauthorization reuses its slot even at full capacity.

Normal Codex renewal retains its binding generation: persist `refresh_inflight` before sending, then replace the entire bundle/clear marker after validation. Pre-admission cancellation can safely clear the marker; a complete 429 can clear it and back off. Ambiguous admitted timeout/redirect/error/malformed success requires reauthorization. Boot never replays a marked token. A received rotation keeps the same exclusive credential lease until storage-only retries succeed, regardless of display/setup/cancel requests.

`observations_v2` stores at most eight active observations at a fifteen-minute cadence, including optional extras. It binds logical ID/provider/row generation and exact native or legacy tuple; it cannot supply identity, aliases, selection or settings. Load merges only observation fields into current rows. V1 caches remain read-only migration inputs. Migration preserves both account cohorts, ID collisions and the previously active selection. Four unique historical networks retain three core profiles plus one visible pending profile; explicit validated activation swaps a named network rather than dropping credentials.

CRC/NVS checks do not provide confidentiality. Flash is unencrypted; no secure-boot, eFuse or physical extraction protection is claimed. Secrets stay outside Git; network/storage/USB have one owner and short-lived scratch. Common-view locks cover bounded copies only, never HTTP/NVS/LVGL/JSON allocation.

## Refresh, display and validation

Screen-off stops AP/Wi-Fi and new HTTP, while storage retries and operation deadlines continue. AP requests/open windows and USB entry/I/O scratch gate new phases immediately; idle USB windows allow device networking. Wake displays cache, reconnects and silently reads collector cache without postponing the shared provider deadline. Manual or due refresh runs one cycle, honors account backoff, and starts the next interval after completion. Collector cache reads cannot fabricate observation freshness. Stored time seeds the clock; native TLS still waits for current-boot setup time or SNTP synchronization. Display time is UTC+8.

The entire first wake gesture is consumed. The panel enters Sleep In with backlight off; workers wait for events/deadlines. CPU DFS permits 40 MHz while asleep; MCU light/deep sleep is not enabled. LVGL tick/ADC-key polling remain. Battery SOC is cached for thirty seconds and does not infer charging. Fixed Chinese text uses subset fonts; arbitrary unsupported identity characters fall back to `?`.

Use [acceptance checks](../development/README.md#acceptance-and-reporting) and report build, host/browser, real-device/provider and unverified evidence separately. Dated results belong in the [changelog](../CHANGELOG.md).
