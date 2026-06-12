/**
 * @file power.c
 * @brief Power management implementation - Light Sleep (Low Power Mode)
 *
 * Uses ESP32-C3 light sleep instead of deep sleep to preserve:
 *   - RAM contents (no RTC memory needed)
 *   - GPIO states (backlight stays on)
 *   - Screen content (no re-initialization needed)
 *
 * Wake-up sources:
 *   - Timer: every minute to refresh the time display
 *   - GPIO:  button press to enter active display mode
 *
 * Implementation note:
 *   We use esp_light_sleep_start() with proper GPIO wakeup configuration.
 *   For Light-sleep, GPIO wakeup must use gpio_wakeup_enable() + 
 *   esp_sleep_enable_gpio_wakeup(), NOT esp_deep_sleep_enable_gpio_wakeup().
 */
#include <string.h>
#include "power.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

void power_init(void)
{
    /* Configure button pins as inputs with pull-up.
     * In light sleep these pins retain their state. */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << GPIO_BTN_WAKE) | (1ULL << GPIO_BTN_REFRESH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_conf);

    /* LCD pins: keep as-is in light sleep (state is preserved).
     * No need to pull-down RST/backlight because light sleep
     * preserves GPIO output levels. */

    /* NOTE: esp_pm_configure(.light_sleep_enable = true) is NOT called
     * here. It is deferred to power_enable_light_sleep() which is called
     * from enter_low_power_mode() — the screensaver loop.
     *
     * Why? Calling it here would enable tickless idle light sleep globally,
     * which means every vTaskDelay() in display_init() / enter_display_mode()
     * / enter_update_mode() would cause the system to enter light sleep,
     * stalling UART log output and making the boot sequence appear to hang.
     * We only want automatic light sleep during the low-power screensaver
     * phase, not during normal display/update phases. */
    ESP_LOGI(TAG, "电源管理初始化完成 (按键GPIO已配置, 自动light sleep已推迟)");
}

void power_enable_light_sleep(void)
{
    /* Enable tickless idle light sleep.
     * ESP32-C3 requires this before esp_light_sleep_start() works,
     * and it also enables automatic light sleep during FreeRTOS idle
     * periods — which is exactly what we want during the low-power
     * screensaver loop.
     *
     * After this call:
     *   - idle task will automatically enter light sleep (saves power)
     *   - UART log output will stall during sleep (acceptable in screensaver)
     *   - esp_light_sleep_start() will work for explicit long-duration sleep */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "电源管理配置失败: %d", ret);
    } else {
        ESP_LOGI(TAG, "自动light sleep已启用 (tickless idle)");
    }
}

void power_disable_light_sleep(void)
{
    /* Disable automatic tickless idle light sleep — restore full CPU speed.
     *
     * After this call:
     *   - idle task will NOT enter light sleep (CPU stays at max frequency)
     *   - UART logs emit normally (no stalls during vTaskDelay)
     *   - esp_light_sleep_start() will still work if re-enabled later */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 160,
        .light_sleep_enable = false,
    };
    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "禁用light sleep失败: %d", ret);
    } else {
        ESP_LOGI(TAG, "自动light sleep已禁用 (CPU全速)");
    }
}

void power_configure_wakeup(uint64_t timer_interval_us)
{
    /* Timer wakeup for periodic time refresh */
    if (timer_interval_us > 0) {
        esp_err_t ret = esp_sleep_enable_timer_wakeup(timer_interval_us);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "timer wakeup 配置失败: %d", ret);
        }
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }

    /* GPIO wakeup for button press (active low)
     * For Light-sleep, use gpio_wakeup_enable() per-pin,
     * then esp_sleep_enable_gpio_wakeup() to enable globally.
     * DO NOT use esp_deep_sleep_enable_gpio_wakeup() - it's for Deep-sleep only! */
    esp_err_t ret = gpio_wakeup_enable(GPIO_BTN_WAKE, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d wakeup enable 失败: %d", GPIO_BTN_WAKE, ret);
    }
    ret = gpio_wakeup_enable(GPIO_BTN_REFRESH, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d wakeup enable 失败: %d", GPIO_BTN_REFRESH, ret);
    }
    ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO wakeup 全局启用失败: %d", ret);
    }
}

bool power_enter_low_power_mode(uint64_t wakeup_interval_us)
{
    power_configure_wakeup(wakeup_interval_us);

    ESP_LOGI(TAG, "进入低功耗模式 (light sleep), 唤醒间隔 %llu us",
             wakeup_interval_us);
    ESP_LOGW(TAG, "注意: USB直连板子在light sleep期间会暂停日志输出");
    ESP_LOGW(TAG, "功能正常，按键唤醒后可恢复日志");

    /* Flush log output before sleeping */
    fflush(stdout);

    /* Enter light sleep. CPU halts here until wakeup.
     * Properly configured GPIO wakeup will wake the system
     * when button is pressed (active low). */
    esp_err_t ret = esp_light_sleep_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "light sleep 失败: %d", ret);
        return false;
    }

    /* Wakeup source determination */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool timer_wakeup = (cause == ESP_SLEEP_WAKEUP_TIMER);

    ESP_LOGI(TAG, "从低功耗模式唤醒, 原因: %s",
             timer_wakeup ? "timer" : "gpio");

    return timer_wakeup;
}

esp_sleep_wakeup_cause_t power_get_wakeup_cause(void)
{
    return esp_sleep_get_wakeup_cause();
}
