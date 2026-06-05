/**
 * @file main.c
 * @brief 电子吧唧 - Electronic Badge Main Program
 * 
 * Features:
 * - Display time and weather
 * - AI-generated weather-themed images
 * - Deep sleep for low power consumption
 * - SmartConfig for WiFi provisioning (use WeChat Mini Program "一键配网")
 */
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "config.h"
#include "power.h"
#include "button.h"
#include "display.h"
#include "wifi_manager.h"
#include "weather.h"
#include "ai_image.h"

static const char* TAG = "Main";

// Global state
static rtc_data_t s_rtc_data = {0};
static weather_data_t s_weather_data = {0};
static bool s_has_rtc_data = false;

// =============================================================================
// NTP Time Sync
// =============================================================================
static void sync_time(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_1);
    config.smooth_sync = false;
    esp_netif_sntp_init(&config);
    
    // Wait for time sync
    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_ERR_TIMEOUT && retry < 5) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry + 1, 5);
        retry++;
    }
    
    esp_netif_sntp_deinit();
    
    // Set timezone to China Standard Time (CST-8)
    setenv("TZ", "CST-8", 1);
    tzset();
    
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    ESP_LOGI(TAG, "Current time: %02d:%02d:%02d", 
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

// =============================================================================
// Mode Handlers
// =============================================================================
static void enter_display_mode(void)
{
    ESP_LOGI(TAG, "=== DISPLAY MODE ===");
    
    // Load RTC data
    power_load_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
    s_has_rtc_data = s_rtc_data.boot_count > 0;
    
    // Initialize display
    display_init();
    display_backlight_on();
    
    // Show boot animation
    display_boot_animation();
    
    // Get current time
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    
    // Display main screen
    const char* weather_text = s_has_rtc_data ? s_rtc_data.weather_text : "晴";
    int temperature = s_has_rtc_data ? s_rtc_data.temperature : 25;
    
    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                       weather_text, temperature, s_rtc_data.last_update_timestamp);
    
    // Initialize button handler
    button_init();
    
    // Start display timeout counter
    uint32_t timeout_counter = 0;
    
    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_counter += 100;
        
        // Check WAKE button (long press = sleep)
        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "Long press WAKE, entering sleep");
                // Wait for button release before entering sleep
                // Otherwise GPIO stays low and immediately wakes from deep sleep
                ESP_LOGI(TAG, "Waiting for WAKE button release...");
                while (button_is_pressed(BUTTON_WAKE)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                ESP_LOGI(TAG, "WAKE button released, entering deep sleep");
                break;
            }
        }
        
        // Check REFRESH button
        button_event_t refresh_event = button_get_event(BUTTON_REFRESH);
        if (refresh_event == BUTTON_EVENT_PRESS) {
            ESP_LOGI(TAG, "Short press REFRESH, manual update");
            
            // Manual refresh: connect WiFi, sync time, fetch weather, update display
            display_loading("更新中...");
            
            if (wifi_connect()) {
                sync_time();
                
                memset(&s_weather_data, 0, sizeof(s_weather_data));
                if (weather_fetch(&s_weather_data)) {
                    // Update RTC data
                    s_rtc_data.weather_code = s_weather_data.weather_code;
                    s_rtc_data.temperature = s_weather_data.temperature;
                    s_rtc_data.feels_like = s_weather_data.feels_like;
                    s_rtc_data.humidity = s_weather_data.humidity;
                    s_rtc_data.last_update_timestamp = s_weather_data.update_time;
                    strncpy(s_rtc_data.weather_text, s_weather_data.weather_text,
                            sizeof(s_rtc_data.weather_text) - 1);
                    s_has_rtc_data = true;
                    
                    // Generate AI image (if enabled in sdkconfig)
                    const char* festival = ai_get_current_festival();
                    if (CONFIG_AI_IMAGE_ENABLED && ai_image_generate(s_weather_data.weather_text,
                                          s_weather_data.temperature, festival)) {
                        s_rtc_data.has_ai_image = 1;
                    }
                    
                    // Refresh display with new data
                    time_t now;
                    time(&now);
                    struct tm* tm_info = localtime(&now);
                    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                                       s_weather_data.weather_text,
                                       s_weather_data.temperature,
                                       s_weather_data.update_time);
                    ESP_LOGI(TAG, "Weather updated successfully");
                } else {
                    ESP_LOGW(TAG, "Weather fetch failed");
                    display_loading("更新失败");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    // Restore previous display
                    time_t now;
                    time(&now);
                    struct tm* tm_info = localtime(&now);
                    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                                       s_has_rtc_data ? s_rtc_data.weather_text : "晴",
                                       s_has_rtc_data ? s_rtc_data.temperature : 25,
                                       s_rtc_data.last_update_timestamp);
                }
                
                wifi_disconnect();
            } else {
                ESP_LOGW(TAG, "WiFi connection failed for manual update");
                display_loading("WiFi连接失败");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            
            timeout_counter = 0;
        } else if (refresh_event == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "Long press REFRESH, config mode");
            display_config_mode();
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        
        // Check timeout
        if (timeout_counter >= DISPLAY_TIMEOUT * 1000) {
            ESP_LOGI(TAG, "Display timeout, entering sleep");
            break;
        }
    }
    
    // Cleanup before sleep
    button_reset();
    display_backlight_off();
    display_deinit();
}

static bool enter_update_mode(bool* out_wifi_failed)
{
    ESP_LOGI(TAG, "=== UPDATE MODE ===");
    
    if (out_wifi_failed) {
        *out_wifi_failed = false;
    }
    
    // Initialize display for update progress
    display_init();
    display_backlight_on();
    display_loading("连接中...");
    
    // Load RTC data
    bool rtc_valid = power_load_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
    if (!rtc_valid) {
        ESP_LOGI(TAG, "RTC data invalid, initialized to zero");
    }
    s_rtc_data.boot_count++;
    s_has_rtc_data = true;
    
    // Connect to WiFi
    if (!wifi_connect()) {
        ESP_LOGW(TAG, "WiFi connection failed");
        display_loading("WiFi连接失败");
        vTaskDelay(pdMS_TO_TICKS(1000));
        display_backlight_off();
        display_deinit();
        if (out_wifi_failed) {
            *out_wifi_failed = true;
        }
        return false;
    }
    
    display_loading("更新中...");
    
    // Sync time
    sync_time();
    
    // Fetch weather
    memset(&s_weather_data, 0, sizeof(s_weather_data));
    bool weather_ok = weather_fetch(&s_weather_data);
    
    if (weather_ok) {
        // Update RTC data
        s_rtc_data.weather_code = s_weather_data.weather_code;
        s_rtc_data.temperature = s_weather_data.temperature;
        s_rtc_data.feels_like = s_weather_data.feels_like;
        s_rtc_data.humidity = s_weather_data.humidity;
        s_rtc_data.last_update_timestamp = s_weather_data.update_time;
        strncpy(s_rtc_data.weather_text, s_weather_data.weather_text, 
                sizeof(s_rtc_data.weather_text) - 1);
        
        // Generate AI image (if enabled in sdkconfig)
        display_loading("AI生成图片中...");
        const char* festival = ai_get_current_festival();
        if (CONFIG_AI_IMAGE_ENABLED && ai_image_generate(s_weather_data.weather_text,
                              s_weather_data.temperature, festival)) {
            s_rtc_data.has_ai_image = 1;
        }
    }
    
    // Disconnect WiFi
    wifi_disconnect();
    
    // Show result briefly
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                       s_has_rtc_data ? s_rtc_data.weather_text : "晴",
                       s_has_rtc_data ? s_rtc_data.temperature : 25,
                       s_rtc_data.last_update_timestamp);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    display_backlight_off();
    display_deinit();
    
    return weather_ok;
}

static void enter_sleep_mode(void)
{
    // Save RTC data
    power_save_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
    
    // Calculate sleep time in minutes
    uint32_t sleep_minutes = (uint32_t)(SLEEP_INTERVAL_US / 60000000ULL);
    uint32_t sleep_seconds = (uint32_t)((SLEEP_INTERVAL_US % 60000000ULL) / 1000000ULL);
    
    if (sleep_seconds > 0) {
        ESP_LOGI(TAG, "Entering deep sleep for %lu min %lu sec", sleep_minutes, sleep_seconds);
    } else {
        ESP_LOGI(TAG, "Entering deep sleep for %lu minutes", sleep_minutes);
    }
    
    power_enter_deep_sleep(SLEEP_INTERVAL_US);
    // Should never reach here
    while(1);
}

static void enter_config_mode(void)
{
    ESP_LOGI(TAG, "=== CONFIG MODE ===");
    
    bool config_success = false;
    
    // Initialize display
    display_init();
    display_backlight_on();
    display_config_mode();
    
    // Initialize button handler
    button_init();
    
    // Start SmartConfig
    if (!wifi_start_smartconfig()) {
        ESP_LOGE(TAG, "Failed to start SmartConfig");
        display_loading("失败");
        vTaskDelay(pdMS_TO_TICKS(2000));
        goto config_cleanup;
    }
    
    ESP_LOGI(TAG, "SmartConfig started, waiting for phone...");
    ESP_LOGI(TAG, "Use WeChat Mini Program: 一键配网");
    
    // Wait for configuration or timeout
    uint32_t timeout_counter = 0;
    
    while (timeout_counter < SMARTCONFIG_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        timeout_counter += 500;
        
        // Check if WiFi is connected (SmartConfig success)
        if (wifi_is_connected()) {
            ESP_LOGI(TAG, "WiFi connected via SmartConfig (一键配网)!");
            display_config_success();
            vTaskDelay(pdMS_TO_TICKS(2000));
            config_success = true;
            break;
        }
        
        // Check for WAKE button long press to exit
        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "Long press WAKE, exit config mode");
                // Wait for button release
                while (button_is_pressed(BUTTON_WAKE)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }
        }
        
        // Update display with progress dots every 2 seconds
        if (timeout_counter % 2000 == 0) {
            int dots = (timeout_counter / 2000) % 4;
            char progress[32];
            snprintf(progress, sizeof(progress), "Configuring%.*s", dots, "...");
            display_loading(progress);
        }
    }
    
    if (!config_success && timeout_counter >= SMARTCONFIG_TIMEOUT_MS) {
        ESP_LOGW(TAG, "SmartConfig timeout (一键配网超时)");
        display_loading("超时");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
config_cleanup:
    // Cleanup
    wifi_stop_smartconfig();
    button_reset();
    display_backlight_off();
    display_deinit();
    
    // If config successful, update weather immediately
    if (config_success) {
        ESP_LOGI(TAG, "Config success, updating weather...");
        bool wifi_failed = false;
        enter_update_mode(&wifi_failed);
        // enter_update_mode() returns if WiFi fails or weather fetch fails
        // Enter sleep after update attempt
        enter_sleep_mode();
    } else {
        // SmartConfig failed/timeout, enter display mode with default data
        ESP_LOGW(TAG, "SmartConfig failed (一键配网失败), entering display mode");
        enter_display_mode();
    }
}

// =============================================================================
// Main Entry
// =============================================================================
void app_main(void)
{
    // Initialize
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  Electronic Badge (电子吧唧)");
    ESP_LOGI(TAG, "  ESP32-C3 Super Mini");
    ESP_LOGI(TAG, "=================================");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Set WiFi log level to INFO for connection debugging
    esp_log_level_set("wifi", ESP_LOG_INFO);
    esp_log_level_set("WiFi", ESP_LOG_INFO);
    
    // Initialize WiFi manager (loads saved configs from NVS)
    wifi_manager_init();
    
    // Initialize power management
    power_init();
    
    // Get wakeup cause
    esp_sleep_wakeup_cause_t wakeup_cause = power_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %d", wakeup_cause);
    
    // Route based on wakeup cause
    switch (wakeup_cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            enter_update_mode(NULL);
            enter_sleep_mode();
            break;
            
        case ESP_SLEEP_WAKEUP_GPIO:
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            // Load RTC data and increment boot count
            bool rtc_valid = power_load_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
            if (!rtc_valid) {
                ESP_LOGI(TAG, "RTC data invalid, initialized to zero");
            }
            s_rtc_data.boot_count++;
            s_has_rtc_data = true;
            ESP_LOGI(TAG, "Boot count: %lu", s_rtc_data.boot_count);
            
            // Check if we have any WiFi config (NVS or sdkconfig)
            wifi_manager_config_t configs[5];
            int nvs_count = wifi_load_configs(configs, 5);
            bool has_sdkconfig_wifi = (strlen(CONFIG_WIFI_SSID) > 0);
            
            ESP_LOGI(TAG, "NVS configs: %d, sdkconfig WiFi: %s", 
                     nvs_count, has_sdkconfig_wifi ? "yes" : "no");
            
            if (nvs_count == 0 && !has_sdkconfig_wifi) {
                // No WiFi config at all, enter config mode
                ESP_LOGI(TAG, "No WiFi config found, entering config mode");
                enter_config_mode();
            } else {
                // Have WiFi config (NVS or sdkconfig), try to connect and update
                if (s_rtc_data.boot_count == 1) {
                    ESP_LOGI(TAG, "First boot, updating weather...");
                } else {
                    ESP_LOGI(TAG, "Checking weather data freshness...");
                }
                
                time_t now = time(NULL);
                bool data_stale = (now - s_rtc_data.last_update_timestamp) > 3600;
                
                ESP_LOGI(TAG, "Data age: %ld seconds, stale: %s", 
                         (long)(now - s_rtc_data.last_update_timestamp),
                         data_stale ? "yes" : "no");
                
                if (s_rtc_data.boot_count == 1 || data_stale) {
                    ESP_LOGI(TAG, "Updating weather...");
                    bool wifi_failed = false;
                    bool update_ok = enter_update_mode(&wifi_failed);
                    
                    if (!update_ok && wifi_failed && s_rtc_data.boot_count == 1) {
                        // First boot and WiFi failed: enter config mode to let user configure
                        ESP_LOGW(TAG, "First boot WiFi failed, entering config mode");
                        enter_config_mode();
                        // enter_config_mode() will enter sleep or display mode
                        return;  // Should not reach here, but just in case
                    }
                    
                    // Enter sleep after update attempt
                    enter_sleep_mode();
                } else {
                    ESP_LOGI(TAG, "Weather data fresh, entering display mode");
                    enter_display_mode();
                }
            }
            break;
    }
    
    // Enter sleep after display mode
    enter_sleep_mode();
}
