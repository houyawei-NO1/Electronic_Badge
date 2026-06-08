/**
 * @file display.h
 * @brief Display module - GC9A01 driver with LVGL UI
 *
 * Uses esp_lcd + esp_lvgl_port to drive GC9A01 1.28" round display (240x240).
 * All UI rendering is done through LVGL v9 widgets.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

/**
 * @brief Initialize display module (SPI + GC9A01 + LVGL)
 */
esp_err_t display_init(void);

/**
 * @brief Turn on display backlight
 */
void display_backlight_on(void);

/**
 * @brief Turn off display backlight
 */
void display_backlight_off(void);

/**
 * @brief Display main screen with time and weather
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 * @param weather_text Weather description
 * @param temperature Temperature in Celsius
 * @param last_update Last update timestamp (Unix time)
 * @param weather_code Weather condition code (QWeather)
 * @param humidity Humidity percentage
 * @param wind_scale Wind scale (Beaufort, 0-12)
 */
void display_main_screen(int hour, int minute, const char* weather_text,
                         int temperature, uint32_t last_update, int16_t weather_code,
                         uint8_t humidity, uint8_t wind_scale);

/**
 * @brief Display loading message
 * @param message Message to display
 */
void display_loading(const char* message);

/**
 * @brief Update the status text on the loading screen without
 *        recreating it. Safe to call while arc animation is running.
 * @param status New status text (e.g. "正在配置WIF", "正在同步", "同步天气")
 */
void display_loading_status(const char* status);

/**
 * @brief Display WiFi config mode screen
 */
void display_config_mode(void);

/**
 * @brief Display config success message
 */
void display_config_success(void);

/**
 * @brief Deinitialize display
 */
void display_deinit(void);

#endif // DISPLAY_H
