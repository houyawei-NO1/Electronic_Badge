/**
 * @file power.c
 * @brief Power management implementation — Light Sleep (Low Power Mode)
 *
 * Uses ESP32-C3 light sleep for power saving during screensaver:
 *   - CPU stops, RAM is retained
 *   - GPIO states preserved (backlight stays on)
 *   - Screen content remains visible
 *   - Wake up every minute to refresh time, or on button press
 *
 * [GPIO WAKEUP REQUIREMENT]
 * GPIO wakeup from light sleep requires:
 *   1. gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL) per pin
 *   2. esp_sleep_enable_gpio_wakeup() globally
 *   3. Pins must NOT have GPIO_INTR_DISABLE (set to GPIO_INTR_LOW_LEVEL)
 *   4. Pins must have internal pull-up enabled (active low buttons)
 *
 * [USB Serial/JTAG NOTE]
 * ESP32-C3 USB Serial/JTAG does NOT survive light sleep.
 * UART logs stop during sleep; this is acceptable for battery operation.
 * For debugging, use external USB-UART bridge on GPIO20/21 instead.
 */
#include <string.h>
#include "power.h"
#include "config.h"
#include "button.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

void power_init(void)
{
    /* Configure button pins as inputs with pull-up for light sleep GPIO wakeup.
     * GPIO_INTR_LOW_LEVEL is required for light sleep wakeup (not ANYEDGE). */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << GPIO_BTN_WAKE) | (1ULL << GPIO_BTN_REFRESH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL  /* Required for light sleep GPIO wakeup */
    };
    gpio_config(&btn_conf);

    /* LCD pins: keep as-is in light sleep (state is preserved).
     * No need to pull-down RST/backlight because light sleep
     * preserves GPIO output levels. */

    ESP_LOGI(TAG, "电源管理初始化完成 (按键GPIO已配置)");
}

void power_enable_light_sleep(void)
{
    /* Enable tickless idle light sleep.
     * ESP32-C3 requires this before esp_light_sleep_start() works,
     * and it also enables automatic light sleep during FreeRTOS idle
     * periods — which saves power during the screensaver phase.
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
    gpio_wakeup_disable(GPIO_BTN_WAKE);
    gpio_wakeup_disable(GPIO_BTN_REFRESH);
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
    /* ============================================================
     * Light Sleep path (battery power saving)
     * ============================================================
     *
     * GPIO wakeup from light sleep requires GPIO_INTR_LOW_LEVEL.
     * But button.c uses GPIO_INTR_ANYEDGE for normal operation.
     * We switch interrupt type before sleep and restore after wakeup. */

    /* Switch to LOW_LEVEL interrupt for light sleep GPIO wakeup */
    button_set_sleep_interrupt_type();
    vTaskDelay(pdMS_TO_TICKS(10));  // Allow GPIO config to settle

    power_configure_wakeup(wakeup_interval_us);
    power_enable_light_sleep();
    ESP_LOGI(TAG, "进入低功耗模式 (light sleep), 唤醒间隔 %llu us", wakeup_interval_us);
    fflush(stdout);

    esp_err_t ret = esp_light_sleep_start();

    /* Restore ANYEDGE interrupt for normal button operation */
    button_restore_interrupt_type();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "light sleep 失败: %d, 回退到软件轮询", ret);
        power_disable_light_sleep();
        /* Fallback: software polling for the requested interval */
        uint64_t elapsed = 0;
        while (elapsed < wakeup_interval_us) {
            vTaskDelay(pdMS_TO_TICKS(50));
            elapsed += 50000;
            if (!gpio_get_level(GPIO_BTN_WAKE) || !gpio_get_level(GPIO_BTN_REFRESH)) {
                return false;  // GPIO wakeup (button pressed)
            }
        }
        return true;  // Timer wakeup (poll timeout elapsed)
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool timer_wakeup = (cause == ESP_SLEEP_WAKEUP_TIMER);
    ESP_LOGI(TAG, "从低功耗模式唤醒, 原因: %s", timer_wakeup ? "timer" : "gpio");
    power_disable_light_sleep();
    return timer_wakeup;
}

esp_sleep_wakeup_cause_t power_get_wakeup_cause(void)
{
    return esp_sleep_get_wakeup_cause();
}
