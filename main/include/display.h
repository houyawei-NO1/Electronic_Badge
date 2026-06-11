/**
 * @file display.h
 * @brief Display module - GC9A01 driver with LVGL UI
 *
 * Uses esp_lcd + esp_lvgl_port to drive GC9A01 1.28" round display (240x240).
 * All UI rendering is done through LVGL v8.3.x widgets.
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
 * @param wday Day of week (0-6)
 * @param weather_code Weather condition code (QWeather)
 * @param weather_text Weather description
 * @param temperature Temperature in Celsius
 * @param humidity Humidity percentage
 * @param wind_scale Wind scale (Beaufort, 0-12)
 * @param update_time_str Update time string
 */
void display_main_screen(int hour, int minute, int wday,
                         int16_t weather_code, const char* weather_text,
                         int8_t temperature, uint8_t humidity, uint8_t wind_scale,
                         const char* update_time_str);

/**
 * @brief Display loading message
 * @param message Message to display
 */
void display_loading(const char* message);

/**
 * @brief Load a pre-converted weather icon binary from SPIFFS into RAM.
 *
 *        PNG files are converted to RGB565+alpha binaries at BUILD TIME
 *        by scripts/png_to_rgb565a.py and embedded into the SPIFFS image
 *        as <code>-fill.bin. At runtime this function simply fread()s the
 *        binary file into a malloc()'d buffer.
 *
 *        IMPORTANT:
 *        - This function NEVER calls lodepng, lv_mem_alloc or lv_mem_free.
 *        - Only malloc/free/fopen/fread are used — safe to call without
 *          holding lvgl_port_lock. Never call it while holding the lock.
 *        - display_main_screen() internally calls this when cache is stale.
 *          Most callers do NOT need to call this function directly.
 *
 * @param weather_code QWeather condition code (e.g. 100, 101, 150, 400)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file missing,
 *         ESP_ERR_NO_MEM on OOM, ESP_ERR_INVALID_SIZE on corrupted binary
 */
esp_err_t display_prepare_weather_icon(int16_t weather_code);

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
 * @brief Display screensaver before sleep
 * Shows a simple clock or pattern to prevent screen burn-in
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 */
void display_screensaver(int hour, int minute);

/**
 * @brief Deinitialize display
 */
void display_deinit(void);

/**
 * @brief Check if display is initialized and ready
 * @return true if initialized, false otherwise
 */
bool display_is_initialized(void);

#endif // DISPLAY_H
