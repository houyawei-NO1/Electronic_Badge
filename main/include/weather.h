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
    int8_t temperature;        // Temperature in Celsius
    int8_t feels_like;         // Feels like temperature
    uint8_t humidity;          // Humidity percentage
    uint8_t wind_speed;        // Wind speed (km/h)
    uint8_t wind_scale;        // Wind scale (Beaufort, 0-12)
    uint16_t pressure;         // Atmospheric pressure (hPa)
    uint8_t visibility;        // Visibility (km)
    char weather_text[32];     // Weather description
    char wind_dir[16];         // Wind direction
    uint32_t update_time;      // Last update timestamp
} weather_data_t;

/**
 * @brief Fetch current weather data
 * @param data Pointer to weather data structure to fill
 * @return true if successful
 */
bool weather_fetch(weather_data_t* data);

#endif // WEATHER_H
