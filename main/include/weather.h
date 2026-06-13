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

// Freshness thresholds (seconds)
#define HOURLY_FRESH_SECS  3600    // 1 hour
#define DAILY_FRESH_SECS   43200   // 12 hours

// Weather data structure (current conditions) — only fields used by UI
typedef struct {
    int16_t weather_code;      // Weather condition code
    int8_t temperature;        // Temperature in Celsius
    uint8_t humidity;          // Humidity percentage
    uint8_t wind_scale;        // Wind scale (Beaufort, 0-12)
    char weather_text[32];     // Weather description
    uint32_t update_time;      // Last update timestamp
} weather_data_t;

// Single hourly forecast item — only fields used by UI
typedef struct {
    char fx_time[6];           // "HH:MM"
    int16_t icon;              // Weather icon code
    int8_t temp;               // Temperature
} hourly_item_t;

// Single daily forecast item — only fields used by UI
typedef struct {
    char fx_date[6];           // "MM/DD"
    int16_t icon_day;          // Daytime icon code
    int8_t temp_max;           // Max temperature
    int8_t temp_min;           // Min temperature
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

/**
 * @brief Save hourly forecast to NVS
 * @param data Pointer to hourly forecast to save
 * @return true if successful
 */
bool weather_save_hourly_to_nvs(const hourly_forecast_t* data);

/**
 * @brief Load hourly forecast from NVS
 * @param data Pointer to hourly forecast to fill
 * @return true if valid data was loaded
 */
bool weather_load_hourly_from_nvs(hourly_forecast_t* data);

/**
 * @brief Save daily forecast to NVS
 * @param data Pointer to daily forecast to save
 * @return true if successful
 */
bool weather_save_daily_to_nvs(const daily_forecast_t* data);

/**
 * @brief Load daily forecast from NVS
 * @param data Pointer to daily forecast to fill
 * @return true if valid data was loaded
 */
bool weather_load_daily_from_nvs(daily_forecast_t* data);

#endif // WEATHER_H
