/**
 * @file config.h
 * @brief Global configuration for 电子吧唧 (Electronic Badge)
 * All configurable values are read from sdkconfig (Kconfig.projbuild)
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "sdkconfig.h"

// =============================================================================
// GPIO Pin Definitions (matched to schematic 2026-06-03)
// ESP32-C3 Super Mini module + 1.28" round screen module + custom connector board
//
// [HARDWARE VERSION NOTE]
// 调试版(经典款): 带串口芯片(CH340/CP2102)，GPIO20被串口占用
// 量产版(简约款): 不带串口芯片，GPIO20可用
//
// 当前配置为调试版，使用GPIO0作为SPI_SCK
// 量产时请修改 GPIO_SPI_SCK 为 20
//
// Note: RST (GPIO5) controls both screen reset and backlight power
// Note: BLK is not routed to ESP32-C3 (handled by screen module internally)
// =============================================================================

// SPI Clock - 调试版使用GPIO0（经典款带串口芯片，GPIO20被占用）
// 量产版(简约款)请改为: #define GPIO_SPI_SCK 20
// #define GPIO_SPI_SCK      0    // SCL (SPI Clock) - DEBUG VERSION
#define GPIO_SPI_SCK   20   // SCL (SPI Clock) - PRODUCTION VERSION

#define GPIO_SPI_MOSI     10   // SDA (SPI Data)
#define GPIO_LCD_DC       7    // DC (Data/Command)
#define GPIO_LCD_CS       6    // CS (Chip Select)
#define GPIO_LCD_RST      5    // RST (Reset for GC9A01 controller)
#define GPIO_BTN_WAKE     4    // WAKE button
#define GPIO_BTN_REFRESH  3    // REFRESH button

// =============================================================================
// Display Configuration (from sdkconfig)
// =============================================================================
#define DISPLAY_WIDTH      240
#define DISPLAY_HEIGHT     240

#ifdef CONFIG_SPI_CLOCK_MHZ
#define DISPLAY_SPI_FREQ   (CONFIG_SPI_CLOCK_MHZ * 1000000)
#else
#define DISPLAY_SPI_FREQ   (20 * 1000000)  // 20MHz default (stable with manual CS)
#endif

#ifdef CONFIG_DISPLAY_TIMEOUT_SECONDS
#define DISPLAY_TIMEOUT    CONFIG_DISPLAY_TIMEOUT_SECONDS
#else
#define DISPLAY_TIMEOUT    60  // 60 seconds default
#endif

// =============================================================================
// Low Power Mode Configuration
// =============================================================================
// Light sleep interval: 1 minute (time refresh period)
#define LIGHT_SLEEP_INTERVAL_US   (60 * 1000000ULL)   // 1 minute

// Weather update interval: 30 minutes (only in active/update mode)
#define WEATHER_UPDATE_INTERVAL_US (30 * 60 * 1000000ULL)  // 30 minutes

// Legacy deep sleep interval (kept for reference, no longer used)
#ifdef CONFIG_UPDATE_INTERVAL_MINUTES
#define SLEEP_INTERVAL_US     ((uint64_t)CONFIG_UPDATE_INTERVAL_MINUTES * 60 * 1000000ULL)
#else
#define SLEEP_INTERVAL_US     (30 * 60 * 1000000ULL)
#endif

// =============================================================================
// WiFi Configuration (from sdkconfig)
// =============================================================================
#ifdef CONFIG_MAX_WIFI_CONFIGS
#define WIFI_MAX_CONFIGS   CONFIG_MAX_WIFI_CONFIGS
#else
#define WIFI_MAX_CONFIGS   5
#endif

#define WIFI_CONNECT_TIMEOUT_MS  20000  // 20 seconds per config
#define WIFI_MAX_RETRY     3           // Max retry attempts per config

// SmartConfig timeout (5 minutes)
#define SMARTCONFIG_TIMEOUT_MS  (5 * 60 * 1000)

// =============================================================================
// Weather API Configuration (和风天气, from sdkconfig)
// =============================================================================
#ifdef CONFIG_WEATHER_API_HOST
#define WEATHER_API_HOST   CONFIG_WEATHER_API_HOST
#else
#define WEATHER_API_HOST   "devapi.qweather.com"  // Default host
#endif

#ifdef CONFIG_WEATHER_API_KEY
#define WEATHER_API_KEY    CONFIG_WEATHER_API_KEY
#else
#define WEATHER_API_KEY    ""
#endif

#ifdef CONFIG_WEATHER_LOCATION
#define WEATHER_LOCATION   CONFIG_WEATHER_LOCATION
#else
#define WEATHER_LOCATION   "101180714"  // NanYangWoLong, default
#endif

// =============================================================================
// NTP Configuration
// =============================================================================
#define NTP_SERVER_1       "ntp.aliyun.com"
#define NTP_SERVER_2       "time.pool.aliyun.com"
// Note: Timezone is set in code using setenv("TZ", "Asia/Shanghai", 1)

// =============================================================================
// RTC Memory (persist across deep sleep)
// =============================================================================
#define RTC_DATA_MAGIC      0xBAD00001  // Magic number to validate RTC data

typedef struct {
    uint32_t magic;                     // Must be RTC_DATA_MAGIC for valid data
    uint32_t last_update_timestamp;
    int16_t weather_code;
    int8_t temperature;
    uint8_t humidity;
    uint8_t wind_scale;      // 风力等级
    char weather_text[16];
    uint32_t boot_count;
} rtc_data_t;

// =============================================================================
// NVS Keys
// =============================================================================
#define NVS_NAMESPACE       "badge_config"
#define NVS_KEY_WIFI_PREFIX "wifi_"

// =============================================================================
// Debug Mode Configuration (from sdkconfig)
// =============================================================================
#ifdef CONFIG_DEBUG_MODE
    #include "esp_log.h"
    #define DEBUG_LOG(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
    #define DEBUG_LOG_GPIO(tag, gpio_num, level) \
        ESP_LOGI(tag, "GPIO%d state: %d", gpio_num, level)
#else
    #define DEBUG_LOG(tag, fmt, ...) ((void)0)
    #define DEBUG_LOG_GPIO(tag, gpio_num, level) ((void)0)
#endif

#endif // CONFIG_H
