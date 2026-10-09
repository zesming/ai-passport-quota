// components/bsp/include/bsp_power.h
// 息屏浅睡眠（POLL 唤醒）。亮屏时应用持有 ESP_PM_CPU_FREQ_MAX 锁，所以不会进入浅睡眠；
// 息屏顺序（应用任务里）：面板 Sleep In → bsp_power_enter_screen_off() → 释放锁；
// 唤醒顺序：先获取锁 → bsp_power_exit_screen_off() → 面板 Sleep Out。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>

// 采样到按键后在 esp_timer 任务里调用，只能入队或做同等级的有界操作。返回 false 表示没能
// 通知（例如队列满），BSP 会在下一个采样周期重试。
typedef bool (*bsp_power_wake_cb_t)(void *user);
void bsp_power_set_wake_callback(bsp_power_wake_cb_t cb, void *user);

// 进入息屏：停按键轮询定时器、暂停 LVGL 定时器并停 tick、配置背光/LCD CS 睡眠电平，
// 启动 50 ms 单次 esp_timer 采样按键 ADC。任何一步失败都回滚到亮屏态并返回错误。幂等。
// 唤醒检测要求先看到一次松开：长按下键息屏时键还按着，不会立刻被当成唤醒。
esp_err_t bsp_power_enter_screen_off(void);

// 退出息屏：停采样定时器、恢复背光 PWM 与睡眠引脚、重启 LVGL tick 并恢复定时器、
// 恢复按键轮询。按键仍按着时，唤醒那一次按键的事件由 bsp_power_wake_gesture_drop() 丢弃。
// 失败可重试，各步骤幂等。
esp_err_t bsp_power_exit_screen_off(void);

bool bsp_power_screen_off(void);

// 按键回调里先调用：true 表示这是唤醒手势的事件，必须丢弃。PRESS 一直丢弃；
// CLICK/LONG（松开或长按）丢弃后手势结束；超过 BSP_BTN_WAKE_GESTURE_MS 不再丢弃。
bool bsp_power_wake_gesture_drop(bsp_btn_t button, bsp_btn_ev_t event);
