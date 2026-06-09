/**
 * @file main.c
 * @brief 电子吧唧 - Electronic Badge Main Program
 * 
 * Features:
 * - Display time and weather
 * - Weather icons from local resources
 * - Deep sleep for low power consumption
 * - SmartConfig for WiFi provisioning (use WeChat Mini Program "一键配网")
 */
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"

#include "config.h"
#include "power.h"
#include "button.h"
#include "display.h"
#include "ui.h"
#include "wifi_manager.h"
#include "weather.h"

static const char* TAG = "主程序";

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
        ESP_LOGI(TAG, "等待系统时间同步... (%d/%d)", retry + 1, 5);
        retry++;
    }
    
    esp_netif_sntp_deinit();
    
    // Set timezone to China Standard Time (CST-8)
    setenv("TZ", "CST-8", 1);
    tzset();
    
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    ESP_LOGI(TAG, "当前时间: %02d:%02d:%02d", 
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

// =============================================================================
// Mode Handlers
// =============================================================================
static void enter_display_mode(void)
{
    ESP_LOGI(TAG, "=== 显示模式 ===");

    // Load RTC data
    power_load_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
    s_has_rtc_data = s_rtc_data.boot_count > 0;

    // Initialize display
    display_init();
    display_backlight_on();

    // Initialize button handler
    button_init();

    // 如果刚从enter_update_mode过来，显示已经初始化且数据已刷新
    // 但为了确保时间最新，还是刷新一次主界面
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                       s_has_rtc_data ? s_rtc_data.weather_text : "晴",
                       s_has_rtc_data ? s_rtc_data.temperature : 25,
                       s_rtc_data.last_update_timestamp,
                       s_has_rtc_data ? s_rtc_data.weather_code : 100,
                       s_has_rtc_data ? s_rtc_data.humidity : 0,
                       s_has_rtc_data ? s_rtc_data.wind_scale : 0);

    // Start display timeout counter
    uint32_t timeout_counter = 0;
    
    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_counter += 100;

        // PicoPixel UI tick (for animations)
        ui_tick();
        
        // Check WAKE button (long press = sleep)
        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "长按WAKE键，进入休眠");
                // Wait for button release before entering sleep
                // Otherwise GPIO stays low and immediately wakes from deep sleep
                ESP_LOGI(TAG, "等待WAKE键释放...");
                while (button_is_pressed(BUTTON_WAKE)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                ESP_LOGI(TAG, "WAKE键已释放，进入深度休眠");
                break;
            }
        }
        
        // Check REFRESH button
        button_event_t refresh_event = button_get_event(BUTTON_REFRESH);
        if (refresh_event == BUTTON_EVENT_PRESS) {
            ESP_LOGI(TAG, "短按REFRESH键，手动更新");

            // 如果WiFi已连接，直接获取天气，无需重新连接
            if (wifi_is_connected()) {
                ESP_LOGI(TAG, "WiFi已连接，直接获取天气");
                // 静默获取天气并更新屏幕
                memset(&s_weather_data, 0, sizeof(s_weather_data));
                if (weather_fetch(&s_weather_data)) {
                    s_rtc_data.weather_code = s_weather_data.weather_code;
                    s_rtc_data.temperature = s_weather_data.temperature;
                    s_rtc_data.humidity = s_weather_data.humidity;
                    s_rtc_data.wind_scale = s_weather_data.wind_scale;
                    s_rtc_data.last_update_timestamp = s_weather_data.update_time;
                    strncpy(s_rtc_data.weather_text, s_weather_data.weather_text,
                            sizeof(s_rtc_data.weather_text) - 1);
                    s_has_rtc_data = true;
                    power_save_rtc_data(&s_rtc_data, sizeof(s_rtc_data));

                    time_t now;
                    time(&now);
                    struct tm* tm_info = localtime(&now);
                    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                                       s_weather_data.weather_text,
                                       s_weather_data.temperature,
                                       s_weather_data.update_time,
                                       s_weather_data.weather_code,
                                       s_weather_data.humidity,
                                       s_weather_data.wind_scale);
                    ESP_LOGI(TAG, "天气更新成功");
                } else {
                    ESP_LOGW(TAG, "天气获取失败，保持缓存数据");
                }
            } else {
                // WiFi未连接，需要重新连接
                display_loading("正在配置WIF");

                if (wifi_connect()) {
                    display_loading_status("正在同步");
                    sync_time();

                    // Show main screen immediately with cached RTC data
                    {
                        time_t now;
                        time(&now);
                        struct tm* tm_info = localtime(&now);
                        display_main_screen(
                            tm_info->tm_hour, tm_info->tm_min,
                            s_has_rtc_data ? s_rtc_data.weather_text : "晴",
                            s_has_rtc_data ? s_rtc_data.temperature : 25,
                            s_rtc_data.last_update_timestamp,
                            s_has_rtc_data ? s_rtc_data.weather_code : 100,
                            s_has_rtc_data ? s_rtc_data.humidity : 0,
                            s_has_rtc_data ? s_rtc_data.wind_scale : 0);
                    }

                    // Fetch weather and silently update the screen
                    memset(&s_weather_data, 0, sizeof(s_weather_data));
                    if (weather_fetch(&s_weather_data)) {
                        s_rtc_data.weather_code = s_weather_data.weather_code;
                        s_rtc_data.temperature = s_weather_data.temperature;
                        s_rtc_data.humidity = s_weather_data.humidity;
                        s_rtc_data.wind_scale = s_weather_data.wind_scale;
                        s_rtc_data.last_update_timestamp = s_weather_data.update_time;
                        strncpy(s_rtc_data.weather_text, s_weather_data.weather_text,
                                sizeof(s_rtc_data.weather_text) - 1);
                        s_has_rtc_data = true;
                        power_save_rtc_data(&s_rtc_data, sizeof(s_rtc_data));

                        time_t now;
                        time(&now);
                        struct tm* tm_info = localtime(&now);
                        display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                                           s_weather_data.weather_text,
                                           s_weather_data.temperature,
                                           s_weather_data.update_time,
                                           s_weather_data.weather_code,
                                           s_weather_data.humidity,
                                           s_weather_data.wind_scale);
                        ESP_LOGI(TAG, "天气更新成功");
                    } else {
                        ESP_LOGW(TAG, "天气获取失败，保持缓存数据");
                    }
                } else {
                    ESP_LOGW(TAG, "手动更新WiFi连接失败");
                    display_loading("WiFi ERROR");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    // Restore main screen
                    time_t now;
                    time(&now);
                    struct tm* tm_info = localtime(&now);
                    display_main_screen(tm_info->tm_hour, tm_info->tm_min,
                                       s_has_rtc_data ? s_rtc_data.weather_text : "晴",
                                       s_has_rtc_data ? s_rtc_data.temperature : 25,
                                       s_rtc_data.last_update_timestamp,
                                       s_has_rtc_data ? s_rtc_data.weather_code : 100,
                                       s_has_rtc_data ? s_rtc_data.humidity : 0,
                                       s_has_rtc_data ? s_rtc_data.wind_scale : 0);
                }
            }

            timeout_counter = 0;
        } else if (refresh_event == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "长按REFRESH键，进入配网模式");
            display_config_mode();
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        
        // Check timeout
        if (timeout_counter >= DISPLAY_TIMEOUT * 1000) {
            ESP_LOGI(TAG, "显示超时，进入休眠");
            break;
        }
    }
    
    // 进入休眠前：不断开显示，而是显示屏保
    // 硬件限制无法关闭背光，关闭显示没有意义，保持屏保显示
    button_reset();

    // 获取当前时间用于屏保（复用函数开头声明的 now / tm_info）
    time(&now);
    tm_info = localtime(&now);
    display_screensaver(tm_info->tm_hour, tm_info->tm_min);

    // 给屏保一点时间渲染
    vTaskDelay(pdMS_TO_TICKS(100));

    // 进入休眠前断开WiFi，降低功耗
    if (wifi_is_connected()) {
        ESP_LOGI(TAG, "休眠前断开WiFi");
        wifi_disconnect();
    }
}

static bool enter_update_mode(bool* out_wifi_failed)
{
    ESP_LOGI(TAG, "=== 更新模式 ===");
    
    if (out_wifi_failed) {
        *out_wifi_failed = false;
    }
    
    // Initialize display for update progress
    display_init();
    display_backlight_on();
    display_loading("正在配置WIF");
    
    // Load RTC data
    bool rtc_valid = power_load_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
    if (!rtc_valid) {
        ESP_LOGI(TAG, "RTC数据无效，已初始化为零");
        s_has_rtc_data = false;
    } else {
        s_has_rtc_data = true;
        ESP_LOGI(TAG, "RTC数据已加载: code=%d temp=%d",
                 s_rtc_data.weather_code, s_rtc_data.temperature);
    }
    s_rtc_data.boot_count++;
    
    // Connect to WiFi
    display_loading_status("正在配置WIF");
    if (!wifi_connect()) {
        ESP_LOGW(TAG, "WiFi连接失败");
        display_loading("WiFi ERROR");
        vTaskDelay(pdMS_TO_TICKS(1000));
        display_backlight_off();
        display_deinit();
        if (out_wifi_failed) {
            *out_wifi_failed = true;
        }
        return false;
    }

    // Sync time — done, show main screen immediately with cached RTC data
    display_loading_status("正在同步");
    sync_time();

    // Show main screen right after time sync (uses cached RTC weather data)
    // Users see real content instead of a loading screen while waiting for weather
    {
        time_t now;
        time(&now);
        struct tm* tm_info = localtime(&now);
        display_main_screen(
            tm_info->tm_hour, tm_info->tm_min,
            s_has_rtc_data ? s_rtc_data.weather_text : "晴",
            s_has_rtc_data ? s_rtc_data.temperature : 25,
            s_rtc_data.last_update_timestamp,
            s_has_rtc_data ? s_rtc_data.weather_code : 100,
            s_has_rtc_data ? s_rtc_data.humidity : 0,
            s_has_rtc_data ? s_rtc_data.wind_scale : 0);
        ESP_LOGI(TAG, "时间同步完成，用缓存数据显示主界面 (code=%d)",
                 s_rtc_data.weather_code);
    }

    // Fetch weather in background — silently update the screen when done
    memset(&s_weather_data, 0, sizeof(s_weather_data));
    bool weather_ok = weather_fetch(&s_weather_data);
    
    if (weather_ok) {
        // Update RTC data
        s_rtc_data.weather_code = s_weather_data.weather_code;
        s_rtc_data.temperature = s_weather_data.temperature;
        s_rtc_data.humidity = s_weather_data.humidity;
        s_rtc_data.wind_scale = s_weather_data.wind_scale;
        s_rtc_data.last_update_timestamp = s_weather_data.update_time;
        strncpy(s_rtc_data.weather_text, s_weather_data.weather_text,
                sizeof(s_rtc_data.weather_text) - 1);
        s_has_rtc_data = true;
        // Persist to RTC memory
        power_save_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
        ESP_LOGI(TAG, "RTC数据已保存: code=%d temp=%d time=%lu",
                 s_rtc_data.weather_code, s_rtc_data.temperature,
                 (unsigned long)s_rtc_data.last_update_timestamp);

        // Refresh main screen with fresh weather data
        time_t now;
        time(&now);
        struct tm* tm_info = localtime(&now);
        display_main_screen(
            tm_info->tm_hour, tm_info->tm_min,
            s_weather_data.weather_text,
            s_weather_data.temperature,
            s_weather_data.update_time,
            s_weather_data.weather_code,
            s_weather_data.humidity,
            s_weather_data.wind_scale);
    } else {
        ESP_LOGW(TAG, "天气拉取失败，保持缓存数据");
    }

    // 保持WiFi连接，不要在这里断开
    // 保持显示开启，让调用者（enter_display_mode）给用户看30秒
    // WiFi和显示将在进入休眠前统一关闭

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
        ESP_LOGI(TAG, "进入深度休眠 %lu 分 %lu 秒", sleep_minutes, sleep_seconds);
    } else {
        ESP_LOGI(TAG, "进入深度休眠 %lu 分钟", sleep_minutes);
    }
    
    power_enter_deep_sleep(SLEEP_INTERVAL_US);
    // Should never reach here
    while(1);
}

static void enter_config_mode(void)
{
    ESP_LOGI(TAG, "=== 配网模式 ===");
    
    bool config_success = false;
    
    // Initialize display
    display_init();
    display_backlight_on();
    display_config_mode();
    
    // Initialize button handler
    button_init();
    
    // Start SmartConfig
    if (!wifi_start_smartconfig()) {
        ESP_LOGE(TAG, "启动SmartConfig失败");
        display_loading("失败");
        vTaskDelay(pdMS_TO_TICKS(2000));
        goto config_cleanup;
    }
    
    ESP_LOGI(TAG, "SmartConfig已启动，等待手机...");
    ESP_LOGI(TAG, "请使用微信小程序: 一键配网");
    
    // Wait for configuration or timeout
    uint32_t timeout_counter = 0;
    
    while (timeout_counter < SMARTCONFIG_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        timeout_counter += 500;
        
        // Check if WiFi is connected (SmartConfig success)
        if (wifi_is_connected()) {
            ESP_LOGI(TAG, "通过SmartConfig(一键配网)连接WiFi成功!");
            display_config_success();
            vTaskDelay(pdMS_TO_TICKS(2000));
            config_success = true;
            break;
        }
        
        // Check for WAKE button long press to exit
        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "长按WAKE键，退出配网模式");
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
            snprintf(progress, sizeof(progress), "配网中%.*s", dots, "...");
            display_loading(progress);
        }
    }
    
    if (!config_success && timeout_counter >= SMARTCONFIG_TIMEOUT_MS) {
        ESP_LOGW(TAG, "SmartConfig超时(一键配网超时)");
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
        ESP_LOGI(TAG, "配网成功，更新天气...");
        bool wifi_failed = false;
        bool update_ok = enter_update_mode(&wifi_failed);
        if (update_ok) {
            // 更新成功，进入显示模式让用户看30秒
            enter_display_mode();
        }
        // 然后进入休眠
        enter_sleep_mode();
    } else {
        // SmartConfig failed/timeout, enter display mode with default data
        ESP_LOGW(TAG, "SmartConfig失败(一键配网失败)，进入显示模式");
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
    ESP_LOGI(TAG, "  电子吧唧 启动中...");
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
    ESP_LOGI(TAG, "唤醒原因: %d", wakeup_cause);
    
    // Route based on wakeup cause
    switch (wakeup_cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            if (enter_update_mode(NULL)) {
                // 更新成功，进入显示模式让用户看30秒
                enter_display_mode();
            }
            enter_sleep_mode();
            break;
            
        case ESP_SLEEP_WAKEUP_GPIO:
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            // Load RTC data and increment boot count
            bool rtc_valid = power_load_rtc_data(&s_rtc_data, sizeof(s_rtc_data));
            if (!rtc_valid) {
                ESP_LOGI(TAG, "RTC数据无效，已初始化为零");
            }
            s_rtc_data.boot_count++;
            s_has_rtc_data = true;
            ESP_LOGI(TAG, "启动次数: %lu", s_rtc_data.boot_count);
            
            // Check if we have any WiFi config (NVS or sdkconfig)
            wifi_manager_config_t configs[5];
            int nvs_count = wifi_load_configs(configs, 5);
            bool has_sdkconfig_wifi = (strlen(CONFIG_WIFI_SSID) > 0);
            
            ESP_LOGI(TAG, "NVS配置数: %d, sdkconfig WiFi: %s", 
                     nvs_count, has_sdkconfig_wifi ? "yes" : "no");
            
            if (nvs_count == 0 && !has_sdkconfig_wifi) {
                // No WiFi config at all, enter config mode
                ESP_LOGI(TAG, "未找到WiFi配置，进入配网模式");
                enter_config_mode();
            } else {
                // Have WiFi config (NVS or sdkconfig), try to connect and update
                if (s_rtc_data.boot_count == 1) {
                    ESP_LOGI(TAG, "首次启动，更新天气...");
                } else {
                    ESP_LOGI(TAG, "检查天气数据新鲜度...");
                }
                
                time_t now = time(NULL);
                bool data_stale = (now - s_rtc_data.last_update_timestamp) > 3600;
                
                ESP_LOGI(TAG, "数据年龄: %ld 秒, 过期: %s", 
                         (long)(now - s_rtc_data.last_update_timestamp),
                         data_stale ? "yes" : "no");
                
                if (s_rtc_data.boot_count == 1 || data_stale) {
                    ESP_LOGI(TAG, "更新天气...");
                    bool wifi_failed = false;
                    bool update_ok = enter_update_mode(&wifi_failed);
                    
                    if (!update_ok && wifi_failed && s_rtc_data.boot_count == 1) {
                        // First boot and WiFi failed: enter config mode to let user configure
                        ESP_LOGW(TAG, "首次启动WiFi失败，进入配网模式");
                        enter_config_mode();
                        // enter_config_mode() will enter sleep or display mode
                        return;  // Should not reach here, but just in case
                    }
                    
                    // After update, enter display mode so user can see the weather
                    // instead of immediately going to sleep
                    ESP_LOGI(TAG, "更新完成，进入显示模式");
                    enter_display_mode();
                } else {
                    ESP_LOGI(TAG, "天气数据新鲜，进入显示模式");
                    enter_display_mode();
                }
            }
            break;
    }
    
    // Enter sleep after display mode
    enter_sleep_mode();
}
