[简体中文](AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) · English

# AI Passport hardware reference

The target is an ESP32-C3 with 8 MB Flash, no PSRAM and a 240 × 320 ST7789P3 RGB565 display. It supports 2.4 GHz Wi-Fi and Bluetooth LE; this application uses Wi-Fi and native USB Serial/JTAG. The board includes an ES8311 microphone/speaker path, but Quota does not initialize or provide audio. The independent hardware power key is separate from the three software function keys.

Published product specifications are 60 × 95 × 8.5 mm, 50 g, a nominal 520 mAh battery and 5 V USB Type-C input. The passive NTAG213 NFC tag is separate from the firmware BSP. These specifications do not establish measured runtime or battery life.

Firmware mappings are authoritative in [`bsp_pins.h`](../../components/bsp/include/bsp_pins.h); behavior follows the BSP headers/implementation and actual measurements. Do not derive additional wiring, polarity or charging signals from a generic ESP32-C3 board.

## Pins and shared resources

| Resource | Mapping | Constraint |
| --- | --- | --- |
| LCD SPI2 | MOSI 9, SCLK 8, CS 1, DC 20 | Reset is not MCU-connected; mode 0, inversion enabled, configured clock 80 MHz |
| Backlight | GPIO21, LEDC 5 kHz/10-bit | UART0's default TX conflicts; use native USB console |
| Function keys | GPIO0, ADC1 channel 0 | UP/DOWN/OK share an external resistor ladder; do not create another ADC1 unit |
| Shared I2C0 | SDA 10, SCL 7 | Reuse BSP-owned bus; CW2017 address `0x63`, ES8311 address `0x18` (7-bit) |
| Physical codec I2S0 wiring | MCLK 6, BCLK 5, WS 3, MCU DOUT 2, DIN 4 | No MCU-controlled amplifier-enable pin; not an active Quota API |
| Native USB | GPIO18/19 | Reserved for USB Serial/JTAG; a data-capable cable is required |

Button voltage windows are UP 0–150 mV, DOWN 150–447 mV and OK 447–1900 mV; released is approximately 3300 mV. The configured click window is 180 ms and long-press threshold 500 ms. `bsp_button_read_mv()` returns `-1` on read failure. Resistor/board changes need actual voltage measurements before updating the header. The power key has no supported software short-press signal; retain its hardware shutdown behavior.

## BSP ownership and initialization

`main/main.c` initializes shared I2C, display/LVGL, backlight, optional battery, quota service/UI and buttons, then starts application/network work. Display/LVGL failure stops startup. Missing battery data degrades to an unavailable indicator. Button callbacks run in the shared `esp_timer` task and only enqueue events.

- `bsp_display_init()` must succeed before `bsp_lvgl_init()`. Serialize first initialization from one owner. Successful initialization is idempotent; partial display/button failures release their acquired resources. LVGL display/callback failure allows retry on the initialized port; a partial port initialization failure requires reboot because cleanup has no public completion handshake.
- Outside the LVGL task, hold `bsp_lvgl_lock()` for object access and unlock only after a successful lock. Keep network/NVS work outside this lock. There is no universal BSP deinitialization API.
- Use the shared BSP I2C bus for the battery. Do not allocate a second bus on the same port or erase unrelated NVS to hide an error.
- `bsp_battery_soc()` returns 0–100 or `-1`; `bsp_battery_mv()` returns millivolts or `-1`. Accuracy depends on the battery/profile. Neither is a charging-state API. USB presence, rising voltage or SOC does not prove charging; the physical green indicator is separate.

## Display and memory

LVGL is fixed at **9.5.0**. The current port uses one 40-line internal DMA buffer (240 × 40 × 2 = 19,200 bytes), RGB565 byte swapping, portrait orientation and a 24 KB LVGL pool. The configured refresh period is 20 ms; this is not measured FPS. Rotation/mirroring belongs in the port display configuration, which can override panel settings.

The final flush masks pixels outside the 30-pixel screen radius to black; do not replace it with a full-screen intermediate ARGB layer without checking memory. Fonts and brand descriptors stay in Flash. With no PSRAM, review TLS/Wi-Fi, LVGL, DMA and task-stack use together; measure free heap, minimum heap and largest block under real load. Configured 80 MHz SPI does not prove a board's signal margin.

Screen off only reduces backlight to zero and gates application networking; function-key sensing remains active. It is not MCU deep sleep or hardware power-off. See [application lifecycle contracts](../applications/ai-quota-monitor.md#refresh-and-screen-lifecycle).

## Physical acceptance

Use the exact build and record board revision, artifact identity and observations. Relevant checks are:

- Startup without panic/watchdog/reboot loops; stable USB and authenticated synchronization.
- LCD colors, byte order, clipping/rounding, orientation, glyphs and backlight behavior.
- Key voltage margins and click/long events across battery levels; consumed wake gesture and manual screen off.
- Plausible battery SOC/voltage and graceful missing-device/I2C failure behavior; no inferred charging animation.
- USB pairing/request-ID acknowledgment, Wi-Fi offline/reconnect, invalid-certificate rejection and saved settings after reboot.
- No device HTTP/retry activity while asleep, silent wake before deadline, overdue refresh afterward, offline and active-request sleep/wake transitions.
- Runtime heap/stack stability, sustained display/network load and measured power consumption when relevant.

Host tests and browser previews cannot establish these observations. [Development and flashing](../development/README.md) defines the verified artifact/data policy; the [changelog](../CHANGELOG.md) records which physical checks were actually performed.
