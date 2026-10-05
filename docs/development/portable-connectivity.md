[简体中文](portable-connectivity.zh_CN.md) · English

# Device setup and connectivity

Passport stores accounts and updates them itself. Phones and computers use the same embedded settings page; they are setup clients, not runtime account authorities. Claude and existing desktop profiles remain optional per-account collector sources. There is no global mode switch. See [application contracts](../applications/ai-quota-monitor.md) for binding, transport and persistence rules.

## Open settings

A physical **Network/setup → Device settings** action opens a temporary WPA2 hotspot for ten minutes. Each window generates a new password and independent 43-character setup key. New provider/collector HTTPS pauses while setup is requested or open; received credentials still finish saving. The baseline is personal 2.4 GHz Wi-Fi or a compatible Internet-capable phone hotspot.

| Device step | Phone or computer action |
| --- | --- |
| 1. Hotspot | Join with the Wi-Fi QR or displayed SSID/full password; remain connected despite the no-Internet notice. |
| 2. Page | Scan the URL QR after joining. Its fragment carries temporary authorization. |
| 3. Manual entry | Type the displayed address and complete setup key into a browser. The bare page prompts for the key. |

Up/Down cycles the three screens; short OK advances or reopens an expired window; long OK closes and returns. QRs retain integer module scaling and a four-module quiet zone. The local page has Accounts, Network and Settings tabs, 16-pixel inputs and controls at least 44 pixels high. No CDN, hosted backend, browser storage or Web Bluetooth is required. The setup key stays in memory; fields/URL are cleared after use. Expiry disables changes, and reopening requires a physical action/fresh key.

## Accounts and network

Device-sourced Codex/DeepSeek and collector-sourced accounts share one list. Each row distinguishes source, readiness, error/backoff and observation age. Native controls do not depend on collector availability. Claude states that a manually running collector is required and offers no native authorization. Removing a collector row removes it from Passport, not the provider/computer. Source replacement and rebind require explicit inline confirmation; email is not an identity match.

At most eight accounts are active. Historical pending rows retain credentials and can be activated, deactivated or swapped with a named active row without deletion. Verified collector discovery requires explicit import; unknown rows never join automatically. Full descriptor/native-slot capacity reports a clear error. Existing native accounts can reauthorize using their own slot even when all slots are occupied.

Saving Wi-Fi accepts a candidate, not verified connectivity. End setup, reconnect, timeout or Codex launch closes the AP before station connection. The candidate has a 25-second validation deadline; only connection and verified storage success replace the saved profile. Failure keeps the prior settings. Three saved networks are supported; an exact historical fourth profile remains pending and can explicitly swap with a named saved profile. Ambiguous same-SSID/different-password profiles require an explicit replacement index.

Codex uses queue then launch: wait for confirmed preparation, close the AP, connect to saved Wi-Fi, show the official authorization QR/full code, and run a bounded fifteen-minute device-code flow. Restore Internet access on the phone/computer and approve on the official site. No browser callback or continued AP connection is required. Device states distinguish connecting, requesting code, waiting, exchanging, saving and terminal result. This experimental client-compatible integration needs real provider/device acceptance.

DeepSeek requires an alias and masked key. Verification starts after setup closes; a received key is not authenticated success. Candidate validation lasts at most sixty seconds after closing. Replacing a key preserves the old committed key/balance on failure; a blank key only changes the alias. Only CNY total balance is displayed. API limitations are documented in the [provider contract](../applications/ai-quota-monitor.md).

Jobs remain queued/running until actual completion; a closing-command ACK allows time to return before AP shutdown. Closing the browser does not cancel work. A request-ID cancellation can stop unsent candidates; received credentials cannot be discarded and must finish saving. Unconfirmed storage gates further network/mutations until recovery.

## Device controls and refresh

Keep the 240 × 320 dark display and 216 × 8 quota tracks. HOME Up/Down switches accounts, short OK refreshes all and long OK opens settings. Submenus use short OK to confirm and long OK to return. AUTH long OK cancels unsent work or returns from terminal state. Long Down sleeps; the first complete wake gesture only wakes. Optional Computer pairing remains a physical 120-second USB action, with no permanent reader or autostart.

One device cadence controls manual or 1/5/15/30-minute refresh. Screen-off closes AP/Wi-Fi and admits no new HTTP; current bounded requests may settle. Authorization pauses but its deadline continues. Received-token persistence and deadlines continue without network. Wake presents cache, reconnects and resumes due/manual work. Collector cache reads are silent and do not move the provider deadline. Setup/login holds the display awake only within its window, preserving the saved never/30/60/120/300/600-second sleep setting.

## Platform boundary and acceptance

| Route | Current project boundary |
| --- | --- |
| Phone hotspot | Use 2.4 GHz with actual Internet. Do not assume every phone relays managed enterprise WLAN or can join Passport while hosting its own hotspot. |
| Enterprise EAP | ESP-IDF/C3 supports enterprise methods; this project has no certificate/identity provisioning UI. Managed phone credentials cannot simply be exported by a QR. |
| Native BLE relay | C3 supports BLE, not Classic PAN. A future native phone tunnel needs explicit GATT buffering, disconnect/background handling and end-to-end device TLS. |
| Browser BLE | An ordinary page is not a universal iOS/Android/Harmony Internet relay. Native platform integration remains separate work. |

Primary references: [ESP-IDF Wi-Fi security](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi-security.html), [C3 BLE](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/ble/overview.html), [Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth), [WebKit policy](https://webkit.org/tracking-prevention/), [HarmonyOS BLE](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-bluetooth-ble), [Apple hotspot](https://support.apple.com/en-ca/guide/security/secfd166f620/web).

Host/browser evidence does not establish phone interoperability, real authorization, TLS memory margin or physical protection. Acceptance covers phone and computer setup, computer-off native reads, mixed/offline sources, storage crash cuts, capacity/migration, cancel/setup/sleep races, reboot persistence and measured TLS/UI margins. Report tested phone platforms and current measurements separately; dated results belong in the [changelog](../CHANGELOG.md).
