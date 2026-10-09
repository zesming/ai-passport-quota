# AI Passport 硬件参考

目标为 ESP32-C3、8 MB Flash、无 PSRAM，屏幕为 240 × 320 ST7789P3 RGB565。芯片支持 2.4 GHz Wi-Fi 和 Bluetooth LE；当前应用使用 Wi-Fi 与原生 USB Serial/JTAG。板卡带 ES8311 麦克风/扬声器通路，但 Quota 不初始化或提供音频功能。独立硬件电源键与三个软件功能键分开。

公开产品规格为 60 × 95 × 8.5 mm、50 g、标称 520 mAh 电池和 5 V USB Type-C 输入。无源 NTAG213 NFC 标签独立于固件 BSP。这些规格不能证明实测运行表现或续航。

固件映射以 [`bsp_pins.h`](../../components/bsp/include/bsp_pins.h) 为准；行为以 BSP 头文件/实现和实际测量为准。不得从通用 ESP32-C3 开发板推断额外接线、极性或充电信号。

## 引脚与共享资源

| 资源 | 映射 | 约束 |
| --- | --- | --- |
| LCD SPI2 | MOSI 9、SCLK 8、CS 1、DC 20 | 复位不接 MCU；mode 0，启用反色，配置时钟 80 MHz |
| 背光 | GPIO21，LEDC 5 kHz/10-bit | 与 UART0 默认 TX 冲突；使用原生 USB 控制台 |
| 功能键 | GPIO0，ADC1 channel 0 | UP/DOWN/OK 共用外部分压电阻；不能另建 ADC1 unit |
| 共享 I2C0 | SDA 10、SCL 7 | 复用 BSP 总线；CW2017 地址 `0x63`、ES8311 地址 `0x18`（7 位） |
| 物理 codec I2S0 接线 | MCLK 6、BCLK 5、WS 3、MCU DOUT 2、DIN 4 | 无 MCU 控制的功放使能脚；不是活动 Quota API |
| 原生 USB | GPIO18/19 | 保留给 USB Serial/JTAG，须用可传输数据的线 |

按键电压窗口：UP 0–150 mV、DOWN 150–447 mV、OK 447–1900 mV，松开约 3300 mV。短按在去抖松手时响应；长按阈值 500 ms，长按松手不再触发短按。创建后清零固定版本按键组件的重复等待，要求至少两个去抖周期。`bsp_button_read_mv()` 读取失败返回 `-1`。电阻/板卡变化须测量真实电压后再更新头文件。电源键没有支持的软件短按信号，保留硬件关机行为。

## BSP 所有权与初始化

`main/main.c` 初始化共享 I2C、显示/LVGL、背光、可选电量计、额度服务/UI 和按键，随后启动应用/网络工作。显示/LVGL 失败停止启动。电量不可用时显示未知标记。按键回调运行于共享 `esp_timer` 任务，仅投递事件。

- `bsp_display_init()` 成功后才能调用 `bsp_lvgl_init()`。首次初始化由单一负责人串行执行。成功初始化可重复；显示/按键部分失败释放本次资源。LVGL display/回调失败可在已有 port 上重试；port 部分初始化失败需重启，因为异步清理无公开完成握手。
- 非 LVGL 任务访问对象前持有 `bsp_lvgl_lock()`，仅成功加锁后解锁。网络/NVS 操作在锁外执行。不存在统一 BSP 反初始化 API。
- 电量计复用 BSP I2C 总线，不在相同端口另建总线，不为掩盖错误擦除无关 NVS。
- `bsp_battery_soc()` 返回 0–100 或 `-1`，`bsp_battery_mv()` 返回毫伏或 `-1`。准确度依赖电池/profile，两者都不是充电状态 API。USB 连接、电压或 SOC 上升不能证明正在充电；物理绿色指示灯独立。

## 显示与内存

LVGL 固定为 **9.5.0**。当前 port 使用单个 20 行内部 DMA 缓冲（240 × 20 × 2 = 9,600 字节）、RGB565 字节交换、竖屏方向和 24 KB LVGL 池。刷新配置周期 20 ms，不代表实测 FPS。旋转/镜像在 port 显示配置处理，它可能覆盖面板设置。

最终 flush 将 30 像素圆角外区域置黑；未经内存检查，不改成全屏中间 ARGB 图层。字体和品牌描述符留在 Flash。无 PSRAM，须同时审查 TLS/Wi-Fi、LVGL、DMA 和任务栈开销，并在真实负载下测量空闲堆、最小堆和最大块。80 MHz SPI 配置不能证明板卡信号裕量。

息屏停止 Wi-Fi，将背光降为零，向 LCD 发送 Sleep In 并暂停刷新定时器。随后 `bsp_power_enter_screen_off()` 依次：停 5 ms 按键轮询（`iot_button_stop`）；在 LVGL 锁内暂停所有运行中的定时器（最多记录 16 个）、`lvgl_port_stop()` 停 tick、再 `lv_timer_enable(true)` 让 port 任务按 `task_max_sleep_ms` 阻塞；背光 GPIO21 `ledc_stop(idle 0)` 并设睡眠下拉，LCD CS GPIO1 设睡眠上拉（`CONFIG_PM_SLP_DISABLE_GPIO` 会让睡眠中的引脚浮空）；启动 50 ms 一次性 `esp_timer` 采样按键 ADC（低于 `BSP_BTN_PRESSED_MAX_MV` 视为按下；长按下键息屏时先要看到一次松开）。最后释放 `quota_awake` 锁，自动浅睡眠（`CONFIG_FREERTOS_USE_TICKLESS_IDLE`）才可能发生。唤醒按相反顺序：先取锁，再 `bsp_power_exit_screen_off()`（停采样、恢复引脚与 PWM、LVGL tick 与定时器、按键轮询；按键仍按着时先布置唤醒手势丢弃），再面板 Sleep Out，最后重连。`BSP_BTN_WAKE_MODE` 在编译期选择 POLL（默认）或预留的 GPIO0 低电平唤醒（P5b，未实现；OK 键最高约 595 mV，与 C3 VIL≈825 mV 余量约 180 mV，须实测）。“松开”一律指 ADC ≥1900 mV。唤醒那次按键的事件由 BSP 丢弃，直到状态机报告松开，或 ADC 持续松开 20 ms（覆盖停表时被打断的长按和短于去抖的轻点，这两种情形状态机不会报松开）。按键初始化失败时息屏降级为只关屏并保持亮屏锁。USB-Serial-JTAG 在浅睡眠中断电，息屏时插 USB 可能无法枚举，须先按键亮屏；`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` 保证主机已连接时不睡。电流降低和唤醒时序须真机测量。见[应用生命周期契约](../applications/ai-quota-monitor.md#刷新显示与验证)。

## 真机验收

使用确切构建，记录板卡版本、产物身份和观察结果。相关检查：

- 启动无 panic/watchdog/重启循环，USB 与认证同步稳定。
- LCD 色彩、字节序、裁切/圆角、方向、字形及背光。
- 不同电量下按键电压裕量和短按/长按事件，唤醒手势消耗与手动息屏。
- 电量/电压合理，缺失器件或 I2C 故障时正常降级，不推断充电动画。
- USB 设置/request ID 确认、Wi-Fi 离线/重连、错误证书拒绝和重启设置保留。
- 息屏时 Wi-Fi 停止、LCD 睡眠，无设备 HTTP/重试或工作任务轮询；功能键可靠唤醒，重连前恢复 LCD，截止前静默唤醒，到期后刷新，以及离线/活动请求中的睡眠唤醒转换。
- 运行时堆/栈稳定性、持续显示/网络负载；涉及功耗时实际测量。

主机测试和浏览器预览不能证明这些观察。[开发与刷写](../development/README.md)规定产物和数据策略，[变更日志](../CHANGELOG.md)记录实际执行的真机检查。
