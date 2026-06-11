/**
 * @file power.h
 * @brief Power management module - Light Sleep / Low Power Mode
 *
 * [HARDWARE LIMITATION]
 * The RST pin (GPIO5) is shared with the backlight power control on this
 * module. When the ESP32-C3 enters deep sleep, GPIO5 is pulled down by
 * power_init(), which turns off the backlight but also resets the GC9A01
 * controller. On wakeup the panel must be fully re-initialized.
 *
 * To avoid this, we use LIGHT SLEEP instead of DEEP SLEEP:
 * - CPU stops, RAM is retained
 * - GPIO states are preserved (backlight stays on)
 * - Screen content remains visible
 * - Wake up every minute to refresh the time display
 * - WiFi stays off to save power
 */
#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_sleep.h"

/**
 * @brief Initialize power management
 */
void power_init(void);

/**
 * @brief Enter low-power mode (light sleep).
 *
 * The CPU halts until one of the following occurs:
 *   - Timer expires (wakeup_interval_us)
 *   - GPIO button is pressed
 *
 * In light sleep:
 *   - RAM is retained (no RTC memory needed)
 *   - GPIO states are preserved (backlight stays on)
 *   - Screen content remains visible
 *   - WiFi/BT must be stopped by caller before entering sleep
 *
 * @param wakeup_interval_us Timer wakeup interval in microseconds
 * @return true if woken by timer, false if woken by GPIO
 */
bool power_enter_low_power_mode(uint64_t wakeup_interval_us);

/**
 * @brief Get the wakeup cause after light sleep
 * @return esp_sleep_wakeup_cause_t Wakeup cause
 */
esp_sleep_wakeup_cause_t power_get_wakeup_cause(void);

/**
 * @brief Configure GPIO wakeup sources
 * @param timer_interval_us Timer wakeup interval (0 to disable)
 */
void power_configure_wakeup(uint64_t timer_interval_us);

/**
 * @brief Check if wakeup cause was from timer (one-minute refresh)
 * @return true if timer wakeup, false otherwise
 */
static inline bool power_is_timer_wakeup(void)
{
    return (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
}

/**
 * @brief Check if wakeup cause was from GPIO (button press)
 * @return true if GPIO wakeup, false otherwise
 */
static inline bool power_is_gpio_wakeup(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause == ESP_SLEEP_WAKEUP_GPIO ||
            cause == ESP_SLEEP_WAKEUP_EXT0 ||
            cause == ESP_SLEEP_WAKEUP_EXT1);
}

#endif // POWER_H
