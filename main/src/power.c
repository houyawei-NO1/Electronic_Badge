/**
 * @file power.c
 * @brief Power management implementation — Low Power Mode (software polling)
 *
 * Uses software polling (vTaskDelay loop) instead of light sleep to:
 *   - Preserve RAM contents (no RTC memory needed)
 *   - Preserve GPIO states (backlight stays on)
 *   - Preserve screen content (no re-initialization needed)
 *   - Keep USB Serial/JTAG functional for debugging
 *
 * Wake-up sources:
 *   - Timer: every minute to refresh the time display
 *   - GPIO:  button press to enter active display mode
 *
 * Design decision:
 *   ESP32-C3's USB Serial/JTAG does NOT survive light sleep, and
 *   usb_serial_jtag_is_connected() is unreliable on C3. GPIO wakeup
 *   from light sleep may also not work correctly. Therefore we use
 *   software polling (50ms GPIO check loop) instead of esp_light_sleep_start().
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
     * These pins are polled every 50ms in the low-power loop. */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << GPIO_BTN_WAKE) | (1ULL << GPIO_BTN_REFRESH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_conf);

    /* LCD pins: keep as-is (state is preserved during software polling).
     * No need to pull-down RST/backlight because GPIO output levels
     * are maintained while the CPU is running. */

    /* NOTE: esp_pm_configure(.light_sleep_enable = true) is NOT called
     * here. In the current software polling implementation, tickless idle
     * light sleep is not used. power_enable_light_sleep() is kept for
     * potential future use if light sleep support is added.
     *
     * Why not enable globally? Calling it here would enable tickless idle
     * light sleep globally, which means every vTaskDelay() in display_init()
     * / enter_display_mode() / enter_update_mode() would cause the system
     * to enter light sleep, stalling UART log output and making the boot
     * sequence appear to hang. */
    ESP_LOGI(TAG, "电源管理初始化完成 (按键GPIO已配置)");
}

void power_enable_light_sleep(void)
{
    /* Enable tickless idle light sleep.
     *
     * NOTE: Currently NOT called in the software polling implementation.
     * Kept for potential future use if light sleep support is added.
     *
     * ESP32-C3 requires this before esp_light_sleep_start() works,
     * and it also enables automatic light sleep during FreeRTOS idle
     * periods.
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
     * NOTE: Currently NOT called in the software polling implementation.
     * Kept for potential future use if light sleep support is added.
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
    /* Configure wakeup sources for potential future light sleep use.
     *
     * NOTE: Currently NOT called in the software polling implementation.
     * Kept for potential future use if light sleep support is added.
     *
     * Timer wakeup for periodic time refresh */
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
     * Software polling mode
     * ============================================================
     *
     * Uses vTaskDelay(50ms) loop to poll GPIO buttons.
     * This keeps USB Serial/JTAG alive for debugging.
     *
     * Real light sleep (esp_light_sleep_start) is NOT used because:
     *   1. ESP32-C3 USB Serial/JTAG does NOT survive light sleep
     *   2. usb_serial_jtag_is_connected() is unreliable on C3
     *   3. GPIO wakeup from light sleep may not work correctly
     *
     * For true battery power saving, consider deep sleep instead.
     */
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

esp_sleep_wakeup_cause_t power_get_wakeup_cause(void)
{
    return esp_sleep_get_wakeup_cause();
}
