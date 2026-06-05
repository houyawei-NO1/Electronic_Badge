/**
 * @file power.h
 * @brief Power management module - Deep Sleep control
 */
#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include "esp_sleep.h"

/**
 * @brief Initialize power management
 */
void power_init(void);

/**
 * @brief Enter deep sleep mode
 * @param wakeup_interval_us Sleep duration in microseconds
 */
void power_enter_deep_sleep(uint64_t wakeup_interval_us);

/**
 * @brief Configure wakeup sources before sleeping
 * @param timer_interval_us Timer wakeup interval (0 to disable)
 */
void power_configure_wakeup(uint64_t timer_interval_us);

/**
 * @brief Get the wakeup cause
 * @return esp_sleep_wakeup_cause_t Wakeup cause
 */
esp_sleep_wakeup_cause_t power_get_wakeup_cause(void);

/**
 * @brief Save data to RTC memory before sleep
 * @param data Pointer to data structure
 * @param size Size of data in bytes
 */
void power_save_rtc_data(const void* data, size_t size);

/**
 * @brief Load data from RTC memory after wakeup
 * @param data Pointer to data structure
 * @param size Size of data in bytes
 * @return true if data is valid, false if data was invalid (initialized to zero)
 */
bool power_load_rtc_data(void* data, size_t size);

#endif // POWER_H
