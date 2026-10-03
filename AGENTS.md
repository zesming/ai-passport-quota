[简体中文](AGENTS.zh_CN.md) · English

# Repository instructions

This repository contains the Quota firmware in `main/`, board support in `components/bsp/`, and the production local companion in `companion/`.

- Preserve existing changes. Read relevant headers and nearby code before editing; make the smallest complete change.
- Keep reusable board drivers in the BSP, product state/UI/network work in `main`, and account credentials in isolated private profiles outside the repository. Never commit credentials, device identifiers or unsanitized logs.
- Preserve authoritative provider timestamps, missing/expired-window semantics and decimal currency strings. Quota polling must not send paid model prompts or invent observations.
- Preserve USB request-ID matching, the physical pairing window, pinned TLS and the separation between loopback settings and authenticated device endpoints. Wi-Fi input goes directly from browser memory to USB.
- Preserve silent wake-cache reads, source-refresh deadlines and screen-off network gates. Preserve existing NVS layouts and compatibility defaults.
- LVGL access outside its task requires a successful BSP lock. Button callbacks only enqueue bounded work; networking/storage must not block them. Stop producers before deleting UI objects.
- Hardware mappings follow `components/bsp/include/bsp_pins.h` and measured evidence. Do not infer wiring or charging status. Check subset-font coverage when UI text changes; compilation does not prove rendering.
- Run checks appropriate to the change; firmware delivery uses the complete gate. Report build, host tests, device tests and unverified checks separately. Keep one active owner/run per test or build target.
- Flashing needs authorization for the exact device, verified artifact and data impact. A newly built artifact needs its own applicable authorization; prior flashing approval is not blanket consent. Never add chip erase as a routine prerequisite.
- Root README contains current functionality and use only. Dated work/validation history belongs in paired changelogs. Keep maintained Markdown English by default, with an aligned `.zh_CN.md` peer and top language links.

Read only the context the task needs:

| Task | Entry |
| --- | --- |
| Use and setup | [README](README.md) |
| Architecture, checks and flashing | [Developer guide](docs/development/README.md) |
| Provider, protocol and persistent data | [Application contracts](docs/applications/ai-quota-monitor.md) |
| Pins, BSP and physical checks | [Hardware reference](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) |
| Companion change | [Companion instructions](companion/AGENTS.md) |
| Assets and font generation | [Assets](assets/README.md) |
