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

// Color definitions (RGB565) - kept for backward compatibility
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_RED         0xF800
#define COLOR_GREEN       0x07E0
#define COLOR_BLUE        0x001F
#define COLOR_YELLOW      0xFFE0
#define COLOR_CYAN        0x07FF
#define COLOR_MAGENTA     0xF81F

// Weather condition types (for selecting default images)
typedef enum {
    WEATHER_SUNNY = 0,
    WEATHER_CLOUDY,
    WEATHER_RAINY,
    WEATHER_SNOWY,
    WEATHER_THUNDER,
    WEATHER_FOGGY,
    WEATHER_MAX
} weather_type_t;

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
 * @brief Clear screen with color (LVGL compatible)
 * @param color RGB565 color
 */
void display_clear(uint16_t color);

/**
 * @brief Display main screen with time and weather
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 * @param weather_text Weather description
 * @param temperature Temperature in Celsius
 * @param last_update Last update timestamp (Unix time)
 */
void display_main_screen(int hour, int minute, const char* weather_text,
                         int temperature, uint32_t last_update);

/**
 * @brief Display loading message
 * @param message Message to display
 */
void display_loading(const char* message);

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
