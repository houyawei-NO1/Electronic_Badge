/**
 * @file weather.h
 * @brief Weather API module (和风天气)
 */
#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>
#include <stdbool.h>

// Max items per forecast
#define HOURLY_MAX    24
#define DAILY_MAX     10

// Weather data structure (current conditions)
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

// Single hourly forecast item
typedef struct {
    char fx_time[24];          // Forecast time "YYYY-MM-DDTHH:MM+08:00"
    int16_t icon;              // Weather icon code
    int8_t temp;               // Temperature
    char text[16];             // Weather description
    uint8_t pop;               // Precipitation probability (%)
} hourly_item_t;

// Single daily forecast item
typedef struct {
    char fx_date[16];          // Forecast date "YYYY-MM-DD"
    int16_t icon_day;          // Daytime icon code
    int16_t icon_night;        // Nighttime icon code
    int8_t temp_max;           // Max temperature
    int8_t temp_min;           // Min temperature
    char text_day[16];         // Daytime weather description
    char text_night[16];       // Nighttime weather description
} daily_item_t;

// Hourly forecast result
typedef struct {
    hourly_item_t hours[HOURLY_MAX];
    int count;                 // Actual number of items returned
} hourly_forecast_t;

// Daily forecast result
typedef struct {
    daily_item_t days[DAILY_MAX];
    int count;                 // Actual number of items returned
} daily_forecast_t;

/**
 * @brief Fetch current weather data
 * @param data Pointer to weather data structure to fill
 * @return true if successful
 */
bool weather_fetch(weather_data_t* data);

/**
 * @brief Fetch hourly forecast (24h endpoint)
 * @param forecast Pointer to hourly forecast structure to fill
 * @return true if successful
 */
bool weather_fetch_hourly(hourly_forecast_t* forecast);

/**
 * @brief Fetch daily forecast (7d/10d endpoint)
 * @param forecast Pointer to daily forecast structure to fill
 * @return true if successful
 */
bool weather_fetch_daily(daily_forecast_t* forecast);

/**
 * @brief Save weather data to NVS (non-volatile storage)
 * @param data Pointer to weather data to save
 * @return true if successful
 */
bool weather_save_to_nvs(const weather_data_t* data);

/**
 * @brief Load weather data from NVS
 * @param data Pointer to weather data structure to fill
 * @return true if valid data was loaded
 */
bool weather_load_from_nvs(weather_data_t* data);

#endif // WEATHER_H
