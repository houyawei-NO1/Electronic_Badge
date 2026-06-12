/**
 * @file main.c
 * @brief 电子吧唧 - Electronic Badge Main Program
 *
 * Features:
 * - Display time and weather
 * - Weather icons from local resources
 * - Low-power mode (light sleep) with 1-minute time refresh
 * - SmartConfig for WiFi provisioning (use WeChat Mini Program "一键配网")
 *
 * [POWER MODE]
 * Light sleep is used instead of deep sleep because the RST pin (GPIO5)
 * is shared with the backlight. Deep sleep would pull GPIO5 low, resetting
 * the GC9A01 controller and requiring full re-initialization on wakeup.
 *
 * In light sleep:
 *   - CPU halts, RAM is retained
 *   - GPIO states preserved (backlight stays on)
 *   - Screen content remains visible
 *   - Wake up every minute to refresh the time
 *   - WiFi stays off to save power
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
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

// Global state (retained in RAM during light sleep)
static weather_data_t s_weather_data = {0};
static bool s_has_weather = false;
static uint32_t s_last_weather_update = 0;  // epoch seconds
static uint32_t s_boot_count = 0;

// =============================================================================
// Helper: format update-time string
// =============================================================================
static void format_update_time(char* buf, size_t buf_size, uint32_t timestamp)
{
    time_t ts = (time_t)timestamp;
    if (ts > 1577836800UL) {
        struct tm* tm_info = localtime(&ts);
        snprintf(buf, buf_size, "%02d:%02d 已更新",
                 tm_info->tm_hour, tm_info->tm_min);
    } else {
        strncpy(buf, "未更新", buf_size);
        buf[buf_size - 1] = '\0';
    }
}

// =============================================================================
// Helper: check if system time is valid (year >= 2020)
// =============================================================================
static bool system_time_valid(void)
{
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    return (tm_info->tm_year + 1900) >= 2020;
}

// =============================================================================
// Helper: sync system time via SNTP
// =============================================================================
static void sync_time(void)
{
    ESP_LOGI(TAG, "正在同步NTP时间...");
    esp_sntp_config_t sntp_conf = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_conf);

    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) != ESP_OK && retry < 10) {
        retry++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_netif_sntp_deinit();

    if (retry < 10) {
        time_t now;
        time(&now);
        struct tm* tm_info = localtime(&now);
        ESP_LOGI(TAG, "NTP同步成功: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    } else {
        ESP_LOGW(TAG, "NTP同步超时");
    }
}

// =============================================================================
// Helper: set system time from epoch seconds
// =============================================================================
static void set_system_time_from_epoch(uint32_t epoch)
{
    struct timeval tv = {
        .tv_sec = (time_t)epoch,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "系统时间已设置: epoch=%lu", (unsigned long)epoch);
}

// =============================================================================
// Mode: Active Display (user viewing the screen)
// =============================================================================
static void enter_display_mode(void)
{
    ESP_LOGI(TAG, "=== 显示模式 ===");

    // Initialize display (first time) or ensure it's on
    if (!display_is_initialized()) {
        if (display_init() != ESP_OK) {
            ESP_LOGE(TAG, "显示初始化失败");
            return;
        }
    }
    display_backlight_on();

    // Initialize button handler
    button_init();

    // Show main screen with current data
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    char update_str[32] = {0};
    format_update_time(update_str, sizeof(update_str), s_last_weather_update);

    display_main_screen(
        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_wday,
        s_has_weather ? s_weather_data.weather_code : 100,
        s_has_weather ? s_weather_data.weather_text : "晴",
        s_has_weather ? s_weather_data.temperature : 25,
        s_has_weather ? s_weather_data.humidity : 0,
        s_has_weather ? s_weather_data.wind_scale : 0,
        update_str);

    // Main loop: handle buttons for DISPLAY_TIMEOUT seconds
    uint32_t timeout_counter = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_counter += 100;

        ui_tick();

        // WAKE button long press -> exit to low power mode
        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "长按WAKE键，进入低功耗模式");
                while (button_is_pressed(BUTTON_WAKE)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }
        }

        // REFRESH button short press -> update weather
        button_event_t refresh_event = button_get_event(BUTTON_REFRESH);
        if (refresh_event == BUTTON_EVENT_PRESS) {
            ESP_LOGI(TAG, "短按REFRESH键，手动更新天气");
            if (wifi_connect()) {
                sync_time();
                if (weather_fetch(&s_weather_data)) {
                    s_has_weather = true;
                    s_last_weather_update = s_weather_data.update_time;

                    time(&now);
                    tm_info = localtime(&now);
                    format_update_time(update_str, sizeof(update_str), s_last_weather_update);
                    display_main_screen(
                        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_wday,
                        s_weather_data.weather_code, s_weather_data.weather_text,
                        s_weather_data.temperature, s_weather_data.humidity,
                        s_weather_data.wind_scale, update_str);
                    ESP_LOGI(TAG, "天气更新成功");
                } else {
                    ESP_LOGW(TAG, "天气获取失败");
                }
                wifi_disconnect();
            } else {
                ESP_LOGW(TAG, "WiFi连接失败");
                display_loading("WiFi ERROR");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            timeout_counter = 0;  // Reset timeout after interaction
        }
        // REFRESH button long press -> enter config mode
        else if (refresh_event == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "长按REFRESH键，进入配网模式");
            display_config_mode();
            vTaskDelay(pdMS_TO_TICKS(3000));
            // Return to display mode after showing config screen
            timeout_counter = 0;
        }

        // Timeout -> exit to low power mode
        if (timeout_counter >= DISPLAY_TIMEOUT * 1000) {
            ESP_LOGI(TAG, "显示超时，进入低功耗模式");
            break;
        }
    }

    button_reset();
}

// =============================================================================
// Mode: Low Power (light sleep, screen shows screensaver)
// =============================================================================
static void enter_low_power_mode(void)
{
    ESP_LOGI(TAG, "=== 低功耗模式 ===");

    // ✅ 在这里才启用自动 tickless light sleep
    // 之前一直保持 CPU 全速，保证 UART 日志正常输出
    power_enable_light_sleep();

    // Show screensaver with current time
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    display_screensaver(tm_info->tm_hour, tm_info->tm_min);

    // Ensure WiFi is disconnected in low power mode
    if (wifi_is_connected()) {
        ESP_LOGI(TAG, "低功耗模式: 断开WiFi");
        wifi_disconnect();
    }

    // Light sleep loop: wake up every minute to refresh time
    while (1) {
        bool timer_wakeup = power_enter_low_power_mode(LIGHT_SLEEP_INTERVAL_US);

        if (!timer_wakeup) {
            // GPIO wakeup (button pressed) -> return to caller to re-enter display mode
            ESP_LOGI(TAG, "按键唤醒，进入显示模式");
            // ✅ 禁用自动 light sleep，恢复 CPU 全速
            //    否则唤醒后所有 vTaskDelay 仍会进入自动 light sleep，导致日志停摆
            power_disable_light_sleep();
            // 给系统一点时间从 light sleep 恢复到全速，确保后续日志正常输出
            vTaskDelay(pdMS_TO_TICKS(50));
            return;
        }

        // Timer wakeup -> refresh screensaver time
        time(&now);
        tm_info = localtime(&now);

        // Only refresh if minute changed (avoid unnecessary redraw)
        static int last_minute = -1;
        if (tm_info->tm_min != last_minute) {
            last_minute = tm_info->tm_min;
            display_screensaver(tm_info->tm_hour, tm_info->tm_min);
            ESP_LOGI(TAG, "屏保刷新: %02d:%02d", tm_info->tm_hour, tm_info->tm_min);
        }

        // Check if weather data is stale (> 30 min) and needs update
        // (Only check, don't update here - update happens when user enters display mode)
        time_t now_sec = time(NULL);
        if (s_has_weather &&
            (now_sec - (time_t)s_last_weather_update) > (30 * 60)) {
            ESP_LOGI(TAG, "天气数据已过期 (>30分钟)，下次进入显示模式时更新");
        }
    }
}

// =============================================================================
// Mode: Weather Update (connect WiFi, fetch weather, then display)
// =============================================================================
static bool enter_update_mode(void)
{
    ESP_LOGI(TAG, "=== 更新模式 ===");

    if (!display_is_initialized()) {
        if (display_init() != ESP_OK) {
            ESP_LOGE(TAG, "显示初始化失败");
            return false;
        }
    }
    display_backlight_on();
    display_loading("正在配置WIF");

    // Connect to WiFi
    display_loading_status("正在配置WIF");
    if (!wifi_connect()) {
        ESP_LOGW(TAG, "WiFi连接失败");
        display_loading("WiFi ERROR");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return false;
    }

    // Sync time
    display_loading_status("正在同步");
    sync_time();

    // Show main screen with cached data first
    {
        time_t now;
        time(&now);
        struct tm* tm_info = localtime(&now);
        char update_str[32];
        format_update_time(update_str, sizeof(update_str), s_last_weather_update);
        display_main_screen(
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_wday,
            s_has_weather ? s_weather_data.weather_code : 100,
            s_has_weather ? s_weather_data.weather_text : "晴",
            s_has_weather ? s_weather_data.temperature : 25,
            s_has_weather ? s_weather_data.humidity : 0,
            s_has_weather ? s_weather_data.wind_scale : 0,
            update_str);
    }

    // Fetch weather
    memset(&s_weather_data, 0, sizeof(s_weather_data));
    if (weather_fetch(&s_weather_data)) {
        // Fix system time if needed
        if (!system_time_valid() && s_weather_data.update_time > 1577836800UL) {
            set_system_time_from_epoch(s_weather_data.update_time);
        }

        s_has_weather = true;
        s_last_weather_update = s_weather_data.update_time;

        // Save to NVS so data persists across reboots/wakeups
        weather_save_to_nvs(&s_weather_data);

        // Refresh screen with new data
        time_t now;
        time(&now);
        struct tm* tm_info = localtime(&now);
        char update_str[32];
        format_update_time(update_str, sizeof(update_str), s_last_weather_update);
        display_main_screen(
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_wday,
            s_weather_data.weather_code, s_weather_data.weather_text,
            s_weather_data.temperature, s_weather_data.humidity,
            s_weather_data.wind_scale, update_str);

        ESP_LOGI(TAG, "天气更新成功: code=%d temp=%d",
                 s_weather_data.weather_code, s_weather_data.temperature);
        return true;
    } else {
        ESP_LOGW(TAG, "天气获取失败");
        return false;
    }
}

// =============================================================================
// Mode: WiFi Config (SmartConfig)
// =============================================================================
static void enter_config_mode(void)
{
    ESP_LOGI(TAG, "=== 配网模式 ===");

    if (!display_is_initialized()) {
        if (display_init() != ESP_OK) {
            ESP_LOGE(TAG, "显示初始化失败");
            return;
        }
    }
    display_backlight_on();
    display_config_mode();

    button_init();

    if (!wifi_start_smartconfig()) {
        ESP_LOGE(TAG, "启动SmartConfig失败");
        display_loading("失败");
        vTaskDelay(pdMS_TO_TICKS(2000));
        goto config_cleanup;
    }

    ESP_LOGI(TAG, "SmartConfig已启动，等待手机...");

    uint32_t timeout_counter = 0;
    bool config_success = false;

    while (timeout_counter < SMARTCONFIG_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        timeout_counter += 500;

        if (wifi_is_connected()) {
            ESP_LOGI(TAG, "配网成功!");
            display_config_success();
            vTaskDelay(pdMS_TO_TICKS(2000));
            config_success = true;
            break;
        }

        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "长按WAKE键，退出配网");
                while (button_is_pressed(BUTTON_WAKE)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }
        }

        if (timeout_counter % 2000 == 0) {
            int dots = (timeout_counter / 2000) % 4;
            char progress[32];
            snprintf(progress, sizeof(progress), "配网中%.*s", dots, "...");
            display_loading(progress);
        }
    }

config_cleanup:
    wifi_stop_smartconfig();
    button_reset();

    if (config_success) {
        ESP_LOGI(TAG, "配网成功，更新天气...");
        if (enter_update_mode()) {
            enter_display_mode();
        }
    } else {
        ESP_LOGW(TAG, "配网失败/超时");
    }
}

// =============================================================================
// Main Entry
// =============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  电子吧唧 启动中...");
    ESP_LOGI(TAG, "  ESP32-C3 Super Mini");
    ESP_LOGI(TAG, "  低功耗模式 (light sleep)");
    ESP_LOGI(TAG, "=================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Set timezone
    setenv("TZ", "CST-8", 1);
    tzset();

    // Initialize WiFi manager
    wifi_manager_init();

    // Initialize power management (GPIO only, no auto light sleep)
    power_init();

    // Initialize display
    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败，重启");
        esp_restart();
    }
    display_backlight_on();

    // Load cached weather data from NVS (persists across reboots/wakeups)
    if (weather_load_from_nvs(&s_weather_data)) {
        s_has_weather = true;
        s_last_weather_update = s_weather_data.update_time;
        ESP_LOGI(TAG, "从NVS加载天气数据成功");
    } else {
        ESP_LOGI(TAG, "NVS无缓存天气数据，首次启动需联网获取");
    }

    // Get wakeup cause
    esp_sleep_wakeup_cause_t wakeup_cause = power_get_wakeup_cause();
    ESP_LOGI(TAG, "唤醒原因: %d", wakeup_cause);

    // Check if we have WiFi config
    wifi_manager_config_t configs[5];
    int nvs_count = wifi_load_configs(configs, 5);
    bool has_sdkconfig_wifi = (strlen(CONFIG_WIFI_SSID) > 0);

    ESP_LOGI(TAG, "NVS配置数: %d, sdkconfig WiFi: %s",
             nvs_count, has_sdkconfig_wifi ? "yes" : "no");

    // First boot or no WiFi config
    if (nvs_count == 0 && !has_sdkconfig_wifi) {
        ESP_LOGI(TAG, "未找到WiFi配置，进入配网模式");
        enter_config_mode();
        // After config, fall through into the main loop below
    }

    // =========================================================================
    // 主循环 (永远循环)
    // 每次低功耗按键唤醒后，回到这里重新判断是否需要更新天气
    // 数据新鲜 → 直接显示主界面（不再显示WiFi连接界面）
    // 数据过期 → 连接WiFi更新天气
    // =========================================================================
    while (1) {
        time_t now = time(NULL);
        s_boot_count++;

        // 天气数据是否需要更新：
        //   - 没有天气数据 → 需要
        //   - 距离上次更新超过 1 小时 → 需要
        bool need_update = !s_has_weather ||
                           ((now - (time_t)s_last_weather_update) > 3600);

        ESP_LOGI(TAG, "--- 主循环 #%lu ---", s_boot_count);
        ESP_LOGI(TAG, "是否有天气: %s, 上次更新: %lds前, 需要更新: %s",
                 s_has_weather ? "yes" : "no",
                 s_has_weather ? (long)(now - (time_t)s_last_weather_update) : -1,
                 need_update ? "yes" : "no");

        if (need_update) {
            ESP_LOGI(TAG, "天气数据过期或缺失，进入更新模式");
            if (enter_update_mode()) {
                ESP_LOGI(TAG, "天气更新成功，进入显示模式");
            } else {
                ESP_LOGW(TAG, "天气更新失败，使用缓存数据进入显示模式");
            }
        } else {
            ESP_LOGI(TAG, "天气数据新鲜，直接进入显示模式");
        }

        // 显示主界面（时间 + 天气 + 温度）
        // 超时或WAKE长按后 return
        enter_display_mode();

        // 进入低功耗屏保模式（大时间显示 + light sleep）
        // 按键唤醒后 return，回到循环顶部
        ESP_LOGI(TAG, "进入低功耗模式");
        enter_low_power_mode();

        // 唤醒后继续循环 → 重新判断是否需要更新天气
        // 不会走到 esp_restart()，也不会重启
    }
}
