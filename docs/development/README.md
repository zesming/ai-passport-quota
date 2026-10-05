[简体中文](README.zh_CN.md) · English

# Development, validation and flashing

## Structure

| Area | Entry |
| --- | --- |
| Firmware input and display lifecycle | `main/main.c` |
| Pure state, parsing and timing | `main/quota_logic.c`, `main/quota_logic.h` |
| Network worker and legacy USB/companion | `main/quota_service.c` |
| Account catalog, controller, auth/query, storage and setup page | `main/quota_catalog.c`, `main/quota_portable_service.c`, `main/quota_direct*.c`, `main/quota_store.c`, `main/quota_portal.c`, `main/portable_setup.html` |
| Dashboard and display assets | `main/quota_ui.c`, `main/quota_brand_assets.c`, `assets/` |
| Board drivers | `components/bsp/include/`, `components/bsp/src/` |
| Desktop UI and USB | `companion/src/` |
| Official providers and local service | `companion/server/` |
| Shared desktop settings | `companion/shared/contract.mjs` |
| Host checks and firmware packaging | `tests/`, `companion/test/`, `tools/` |

Keep state/protocol calculations testable without ESP-IDF/LVGL. Firmware button callbacks enqueue events; application and network workers handle slow work. Access LVGL only while holding its BSP lock outside the LVGL task. Read [application contracts](../applications/ai-quota-monitor.md) before changing protocol, provider data, refresh or persistence; read the [hardware reference](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) before changing drivers or configuration.

## Build and check

The companion uses Node.js 22+, npm, OpenSSL and the official provider clients. Firmware uses ESP-IDF **5.5.3** for ESP32-C3. Activate a suitable existing installation, or install this version using [Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/index.html). Keep other toolchains and user configuration intact.

```bash
# Repository root; companion behavior/frontend checks
(cd companion && npm ci && npm test && npm run build)

# Activate ESP-IDF 5.5.3 first
source <esp-idf-v5.5.3>/export.sh
idf.py --version
./tools/validate.sh --static    # repository/workflow checks and firmware host tests
./tools/validate.sh --firmware  # isolated firmware build and image/bundle checks
./tools/validate.sh             # both firmware gates
```

Run focused checks while editing. Use the complete gate for firmware delivery and the companion checks for desktop behavior changes. CI calls the shared gate; do not maintain competing build sequences. A documentation-only change needs document checks, not unrelated device flashing.

The tracked defaults are `sdkconfig.defaults`, `partitions.csv` and `dependencies.lock`. LVGL is pinned to 9.5.0; its unused examples and demos are disabled with `CONFIG_LV_BUILD_EXAMPLES=n` and `CONFIG_LV_BUILD_DEMOS=n`. Review lock changes with their component manifests. The firmware gate generates an isolated `sdkconfig` and build in a temporary directory; ignored local settings do not enter that artifact. Resolve any requested local variant explicitly. Optional `IDF_CCACHE_ENABLE=1` reuses compiler work when ccache is available.

A successful gate writes `build/FoloToy-AI-Passport-full.bin` and a verified matching bundle at `build/firmware/<full-image-sha256>/`. The bundle contains the merged image, application ELF/MAP/image, bootloader, partition table, `flash_args` and a size/hash manifest. Verify it with:

```bash
python3 tools/archive_firmware.py verify <bundle-directory>
```

Use the matching ELF when decoding a crash. A later rebuild may have a different identity. Failed runs can leave previous outputs intact; report the exact successful bundle path and image hash. Generated firmware/debug bundles stay outside commits and are not uploaded automatically. Additional custom partition payloads are not retained separately in this bundle.

For the embedded device setup page, `node tools/preview_portable.mjs` serves the exact production HTML with synthetic fixtures; open the URL printed at startup. `http://127.0.0.1:4328/__preview/manual` tests manual connection with a fixture key of 43 consecutive `s` characters. This is a manual development preview, not a data collector. See [portable design](portable-connectivity.md) before changing device-owned authorization.

For frontend iteration, run `AIQ_DEV_ORIGIN=http://127.0.0.1:5173 npm start` in `companion/`, then `npm run dev` in another terminal and open that exact origin. For documentation screenshots, `npm run preview:readme` serves isolated synthetic accounts at `http://127.0.0.1:4327/`.

## Flashing and NVS

The default 8 MB layout is NVS at `0x9000`/`0x6000`, PHY data at `0xF000`/`0x1000`, factory app at `0x10000`/`0x7B0000`, and portable NVS at `0x7C0000`/`0x40000`; the partition table is normally at `0x8000`. The verifier checks configured offsets, bounds, non-overlap, partition MD5 and image correspondence. A valid custom layout is allowed; use the actual artifact's recorded offsets.

Before writing, identify the intended device and exact verified artifact, confirm partition compatibility and state the data impact. Obtain applicable authorization for that device/artifact/write scope. Prior approval for another build does not authorize a new artifact; rebuilding changes the proposed deliverable. Reading back the original firmware is not required.

A merged `full.bin` written at `0x0` includes padded gaps and can reset NVS/PHY data. Use it for blank devices or an intentional complete refresh. To preserve settings, write only the verified component images at their recorded offsets after confirming a compatible partition table and that none of those writes overlaps saved data. Save any required data using an application-supported method. Do not substitute the app-only image at `0x0`, and do not erase the whole chip unless explicitly authorized. Artifact verification alone does not guarantee user-data preservation.

## Acceptance and reporting

Keep the native USB serial connection open throughout authorization and avoid modem-line changes. Reconnecting a monitor can reset the C3 and invalidate that acceptance run.

Report `Build`, `Host tests`, `Device tests` and `Unverified` separately, with the checks actually performed. Browser screenshots and host simulations do not prove board rendering, physical USB pairing or provider authorization. Relevant checks include independent/missing windows, real 0%, expired-window waiting, conditional Codex extras and restoration of matching source-bound cached extras after reboot. Missing extras must stay absent until observed. Device acceptance also covers fonts/buttons, pairing, certificate rejection, Wi-Fi stop/reconnect, LCD Sleep In/out, silent cached wake, preserved refresh deadlines and reboot persistence. Compare measured awake/asleep current under stated USB/battery conditions; configuration and host tests alone establish no current reduction. Real DeepSeek credentials and extended/offline timing still need their own acceptance evidence; consult the [changelog](../CHANGELOG.md) for dated results rather than transferring prior acceptance to a new build.
