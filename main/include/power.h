/**
 * @file power.h
 * @brief Power management module — Low Power Mode (software polling)
 *
 * [HARDWARE LIMITATION]
 * The RST pin (GPIO5) is shared with the backlight power control on this
 * module. When the ESP32-C3 enters deep sleep, GPIO5 is pulled down by
 * power_init(), which turns off the backlight but also resets the GC9A01
 * controller. On wakeup the panel must be fully re-initialized.
 *
 * [DESIGN DECISION — Software Polling instead of Light Sleep]
 * ESP32-C3's built-in USB Serial/JTAG does NOT survive light sleep.
 * Additionally, usb_serial_jtag_is_connected() is unreliable on C3,
 * and GPIO wakeup from light sleep may not work correctly.
 * Therefore we use software polling (vTaskDelay loop) instead:
 * - CPU polls GPIO buttons every 50ms
 * - RAM is retained, GPIO states preserved, screen content visible
 * - WiFi stays off to save power
 * - Wake up every minute to refresh the time display
 * - USB Serial/JTAG remains functional for debugging
 */
#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_sleep.h"

/**
 * @brief Initialize power management (GPIO only, no auto light sleep)
 *
 * This only configures button GPIO pins. Auto light sleep (tickless idle)
 * is NOT enabled here — call power_enable_light_sleep() before the first
 * esp_light_sleep_start(). This keeps UART logs and CPU at full speed
 * during normal display / update phases.
 *
 * NOTE: In the current software polling implementation, power_enable_light_sleep()
 * and esp_light_sleep_start() are not used. This function is kept for
 * potential future use if light sleep support is added.
 */
void power_init(void);

/**
 * @brief Enable automatic tickless idle light sleep.
 *
 * NOTE: Currently NOT used in the software polling implementation.
 * Kept for potential future use if light sleep support is added.
 *
 * Call this once before entering the low-power screensaver loop.
 * After this call, FreeRTOS idle task will automatically enter light sleep
 * whenever the system is idle — which saves power and is the desired
 * behavior during the screensaver (big-clock) phase.
 *
 * Must NOT be called during normal (display / update) modes because
 * it causes UART log output to stall and reduces responsiveness.
 */
void power_enable_light_sleep(void);

/**
 * @brief Disable automatic tickless idle light sleep.
 *
 * NOTE: Currently NOT used in the software polling implementation.
 * Kept for potential future use if light sleep support is added.
 *
 * Call this when waking up from low-power mode and resuming normal
 * display/update operations. Restores CPU to full speed so UART logs
 * emit normally and UI updates feel responsive.
 */
void power_disable_light_sleep(void);

/**
 * @brief Enter low-power mode (software polling).
 *
 * Uses vTaskDelay(50ms) loop to poll GPIO buttons.
 * The function blocks until one of the following occurs:
 *   - Timer expires (wakeup_interval_us) → returns true
 *   - GPIO button is pressed → returns false
 *
 * In low-power mode:
 *   - RAM is retained (no RTC memory needed)
 *   - GPIO states are preserved (backlight stays on)
 *   - Screen content remains visible
 *   - WiFi/BT must be stopped by caller before entering
 *   - USB Serial/JTAG remains functional
 *
 * NOTE: This does NOT use esp_light_sleep_start() due to ESP32-C3
 * USB Serial/JTAG limitations. See file header for details.
 *
 * @param wakeup_interval_us Timer wakeup interval in microseconds
 * @return true if woken by timer (timeout), false if woken by GPIO (button)
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
