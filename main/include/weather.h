/**
 * @file weather.h
 * @brief Weather API module (和风天气)
 */
#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>
#include <stdbool.h>

// Weather data structure
typedef struct {
    int16_t weather_code;      // Weather condition code
    int8_t temperature;       // Temperature in Celsius
    int8_t feels_like;         // Feels like temperature
    uint8_t humidity;          // Humidity percentage
    uint8_t wind_speed;        // Wind speed
    char weather_text[32];     // Weather description
    char wind_dir[16];         // Wind direction
    uint32_t update_time;      // Last update timestamp
} weather_data_t;

/**
 * @brief Initialize weather module
 */
void weather_init(void);

/**
 * @brief Fetch current weather data
 * @param data Pointer to weather data structure to fill
 * @return true if successful
 */
bool weather_fetch(weather_data_t* data);

/**
 * @brief Get weather text from weather code
 * @param code Weather condition code
 * @return Weather text description
 */
const char* weather_get_text(int16_t code);

/**
 * @brief Convert weather code to weather type for icons
 * @param code Weather condition code
 * @return Weather type enum
 */
int weather_code_to_type(int16_t code);

#endif // WEATHER_H
