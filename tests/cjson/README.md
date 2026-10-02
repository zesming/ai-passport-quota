<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Host JSON test dependency

`cJSON.c` and `cJSON.h` are copied unchanged from ESP-IDF v5.5.3, `components/json/cJSON`, at the pinned IDF commit `2c211b236707889e8400c4dc5644dd5c4ee071e0`. Both carry the original MIT license notice. This is the real JSON parser used by the firmware, not a test stub. The host gate compiles it to test quota/provision parsing without requiring ESP-IDF installation. Firmware continues to link ESP-IDF's built-in `json` component. Keep these two files aligned if the firmware's IDF version changes.

Upstream source: [cJSON](https://github.com/DaveGamble/cJSON).
