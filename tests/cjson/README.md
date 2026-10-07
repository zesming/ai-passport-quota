# 主机 JSON 测试依赖

`cJSON.c` 和 `cJSON.h` 原样复制自 ESP-IDF v5.5.3 的 `components/json/cJSON`，IDF 固定提交为 `2c211b236707889e8400c4dc5644dd5c4ee071e0`。两份文件保留原 MIT 许可证声明。这是固件实际使用的解析器，不是测试桩；主机检查编译它来验证 USB 帧和设置命令解析，不依赖安装 ESP-IDF。正式固件仍链接 IDF 内置 `json` 组件。调整固件的 IDF 版本时应同步这些文件。

上游来源：[cJSON](https://github.com/DaveGamble/cJSON)。
