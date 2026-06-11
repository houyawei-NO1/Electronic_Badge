# 低功耗模式 (Light Sleep) 设计文档

## 版本历史

| 日期 | 版本 | 变更 |
|------|------|------|
| 2026-06-10 | v2.0 | 从 Deep Sleep 重构为 Light Sleep |
| 2026-06-10 | v1.0 | 初始 Deep Sleep 实现 |

## 硬件限制

RST 引脚 (GPIO5) 与背光电源共用。Deep sleep 会拉低 GPIO5，导致：
- GC9A01 控制器复位
- 唤醒后需完整重新初始化显示
- 屏幕闪烁/黑屏问题

## 解决方案：Light Sleep

| 特性 | Deep Sleep | Light Sleep |
|------|-----------|-------------|
| CPU 状态 | 完全关闭 | 暂停，保留寄存器 |
| RAM | 仅 RTC 内存保留 | 全部保留 |
| GPIO 状态 | 丢失 | 保持 |
| 背光 | 熄灭 | 保持开启 |
| 屏幕内容 | 丢失 | 保持可见 |
| 唤醒延迟 | ~100ms | ~1ms |
| 功耗 | ~5uA | ~0.5mA |

## 电源管理配置

```c
esp_pm_config_t pm_config = {
    .max_freq_mhz = 160,      // 最高 CPU 频率
    .min_freq_mhz = 40,       // 最低 CPU 频率 (light sleep 时)
    .light_sleep_enable = true,
};
esp_pm_configure(&pm_config);
```

## 唤醒源

### Timer Wakeup (每分钟刷新)
```c
esp_sleep_enable_timer_wakeup(60 * 1000000ULL);  // 60秒
```

### GPIO Wakeup (按键唤醒)
```c
esp_deep_sleep_enable_gpio_wakeup(
    (1ULL << GPIO_BTN_WAKE) | (1ULL << GPIO_BTN_REFRESH),
    ESP_GPIO_WAKEUP_GPIO_LOW);
```

## 运行流程

```
启动
  │
  ▼
显示模式 (60秒)
  │   ├── 显示主界面（时间、天气、图标）
  │   ├── WAKE 长按 → 进入低功耗模式
  │   └── REFRESH 短按 → 连WiFi更新天气 → 断开WiFi
  │
  ▼
低功耗模式 (light sleep 循环)
      │
      ├── 显示屏保 (当前时间)
      ├── 断开 WiFi
      ├── light sleep 60秒
      │       └── 唤醒后检查分钟变化 → 刷新屏保时间
      │
      └── 按键唤醒 → 返回显示模式
```

## 文件变更

| 文件 | 变更 |
|------|------|
| `main/include/power.h` | 移除 RTC API，添加 light sleep 接口 |
| `main/src/power.c` | 实现 light sleep + 电源管理配置 |
| `main/include/config.h` | 添加 `LIGHT_SLEEP_INTERVAL_US` (1分钟) |
| `main/main.c` | 完全重写：移除 RTC 依赖，RAM 状态保持 |
| `main/include/display.h` | 添加 `display_is_initialized()` |
| `main/src/display.c` | 实现 `display_is_initialized()` |

## 已知限制

### USB 直连板子日志断开
ESP32-C3 USB Serial/JTAG 控制器在 CPU 进入 light sleep 时会暂停工作。

| 板子类型 | 日志行为 | 原因 |
|---------|---------|------|
| 带串口芯片 (CH340/CP2102) | 正常打印 | 串口芯片独立供电，不受 CPU 睡眠影响 |
| USB 直连 (Super Mini) | light sleep 期间断开 | USB 控制器随 CPU 暂停 |

**这不是 bug，是硬件限制。**
- 功能完全正常（时间刷新、按键唤醒）
- 按键唤醒后日志自动恢复
- 如需持续日志，请使用带串口芯片的板子

### 字库限制
当前 lv_font_simsun_16_cjk 字库主要包含日文常用汉字，缺少大量简体中文天气相关字符。
如需完整中文天气显示，需要：
1. 使用更大的中文字库（如完整的思源黑体）
2. 或仅使用字库支持的字符组合天气名称

## 日志示例

```
I (94789) 主程序: 显示超时，进入低功耗模式
I (94789) 主程序: === 低功耗模式 ===
I (94805) 主程序: 低功耗模式: 断开WiFi
I (94901) power: 进入低功耗模式 (light sleep), 唤醒间隔 60000000 us
I (154901) power: 从低功耗模式唤醒, 原因: timer
I (154901) 主程序: 屏保刷新: 18:34
I (154901) power: 进入低功耗模式 (light sleep), 唤醒间隔 60000000 us
```
