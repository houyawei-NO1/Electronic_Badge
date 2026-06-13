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

// Forecast data (fetched alongside current weather)
static hourly_forecast_t s_hourly_forecast = {0};
static daily_forecast_t s_daily_forecast = {0};
static bool s_has_forecast = false;

// Screen navigation index for short-press WAKE cycling
// 0 = main, 1 = hourly, 2 = daily
static int s_screen_page = 0;

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
/**
 * @brief Synchronize system time via NTP
 * @return epoch seconds on success, 0 on failure
 *
 * IMPORTANT: The returned epoch must be re-applied via settimeofday()
 * AFTER wifi_disconnect(), because esp_wifi_stop() resets the system clock.
 */
static time_t sync_time(void)
{
    ESP_LOGI(TAG, "正在同步NTP时间...");
    esp_sntp_config_t sntp_conf = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_1);
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
        return now;  // 返回 epoch，调用方在 wifi_disconnect() 后重新设置
    } else {
        ESP_LOGW(TAG, "NTP同步超时");
        return 0;
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
// Helper: safe WiFi disconnect (preserves system time across esp_wifi_stop)
// =============================================================================
/**
 * esp_wifi_stop() resets the system clock. This helper saves the current
 * time before disconnecting and restores it afterwards.
 */
static void wifi_disconnect_safe(void)
{
    time_t now;
    time(&now);
    wifi_disconnect();
    // Restore time after esp_wifi_stop() cleared it
    if (now > 1577836800L) {  // valid: after 2020-01-01
        struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
}

// Forward declarations (static functions defined later in this file)
static void enter_config_mode(void);

// =============================================================================
// Helper: refresh main screen with current RAM data (used after time sync)
// =============================================================================
static void refresh_main_screen(void)
{
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
}

// =============================================================================
// Mode: Display (main screen with time + weather)
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

    // Reset screen to main page
    s_screen_page = 0;

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

    // 后台静默同步时间标志（只执行一次）
    bool time_synced = system_time_valid();
    bool time_sync_in_progress = false;

    // Main loop: handle buttons for DISPLAY_TIMEOUT seconds
    uint32_t timeout_counter = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_counter += 100;

        ui_tick();

        // --- 后台静默时间同步（用户无感知）---
        // 如果系统时间无效（年份 < 2020），后台连 WiFi 同步 NTP，
        // 同步成功后静默刷新主界面的时间显示，不中断用户操作。
        if (!time_synced && !time_sync_in_progress) {
            ESP_LOGI(TAG, "系统时间无效，后台同步时间...");
            time_sync_in_progress = true;

            bool sync_ok = false;

            // 优先尝试 NTP 同步
            if (wifi_connect()) {
                sync_time();
                wifi_disconnect_safe();  // 自动保护系统时间不被 esp_wifi_stop() 清除
                sync_ok = system_time_valid();
            }

            // NTP 失败或 WiFi 连不上 → 用天气 update_time 兜底
            if (!sync_ok && s_has_weather && s_weather_data.update_time > 1577836800UL) {
                ESP_LOGW(TAG, "NTP失败，用天气API时间兜底");
                set_system_time_from_epoch(s_weather_data.update_time);
                sync_ok = system_time_valid();
            }

            if (sync_ok) {
                time_synced = true;
                ESP_LOGI(TAG, "后台时间同步成功，刷新显示");
                refresh_main_screen();
            } else {
                ESP_LOGW(TAG, "后台时间同步失败，时间可能不准确");
            }

            time_sync_in_progress = false;
        }

        // WAKE button handling:
        //   Long press (while held) → exit to low power mode
        //   Short press (after release) → cycle between main/hourly/daily screens
        if (button_is_pressed(BUTTON_WAKE)) {
            button_event_t event = button_get_event(BUTTON_WAKE);
            if (event == BUTTON_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "长按WAKE键，进入低功耗模式");
                while (button_is_pressed(BUTTON_WAKE)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }
        } else {
            // Button not pressed — check for short press event (fired on release)
            button_event_t wake_short = button_get_event(BUTTON_WAKE);
            if (wake_short == BUTTON_EVENT_PRESS) {
                s_screen_page = (s_screen_page + 1) % 3;  // 0→1→2→0
                timeout_counter = 0;

                if (s_screen_page == 0) {
                    // Main screen
                    time(&now);
                    tm_info = localtime(&now);
                    format_update_time(update_str, sizeof(update_str), s_last_weather_update);
                    display_main_screen(
                        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_wday,
                        s_has_weather ? s_weather_data.weather_code : 100,
                        s_has_weather ? s_weather_data.weather_text : "晴",
                        s_has_weather ? s_weather_data.temperature : 25,
                        s_has_weather ? s_weather_data.humidity : 0,
                        s_has_weather ? s_weather_data.wind_scale : 0,
                        update_str);
                    ESP_LOGI(TAG, "切换到主界面");
                } else if (s_screen_page == 1) {
                    // Hourly forecast
                    if (s_has_forecast) {
                        loadScreen(SCREEN_ID_BADGE_HOURLY);
                        display_hourly_forecast(&s_hourly_forecast);
                    } else {
                        ESP_LOGW(TAG, "无小时预报数据，显示主界面");
                        s_screen_page = 0;
                    }
                    ESP_LOGI(TAG, "切换到小时预报界面");
                } else if (s_screen_page == 2) {
                    // Daily forecast
                    if (s_has_forecast) {
                        loadScreen(SCREEN_ID_BADGE_DAILY);
                        display_daily_forecast(&s_daily_forecast);
                    } else {
                        ESP_LOGW(TAG, "无每日预报数据，显示主界面");
                        s_screen_page = 0;
                    }
                    ESP_LOGI(TAG, "切换到每日预报界面");
                }
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
                    // Also fetch forecast data
                    weather_fetch_hourly(&s_hourly_forecast);
                    weather_fetch_daily(&s_daily_forecast);
                    if (s_hourly_forecast.count > 0 || s_daily_forecast.count > 0)
                        s_has_forecast = true;
                } else {
                    ESP_LOGW(TAG, "天气获取失败");
                }
                wifi_disconnect_safe();
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
            enter_config_mode();
            // enter_config_mode 内部成功后会自动进入更新+显示模式，
            // 失败后直接返回。不管成功失败，都跳出当前显示循环
            // 回到 while(1) 主循环重新判断。
            break;
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

    // 注意: power_enable_light_sleep() 和 power_disable_light_sleep()
    // 现在由 power_enter_low_power_mode() 在内部管理（仅真正 light sleep 路径启用，
    // USB 连接走软件轮询时不启用）。这里不用再调它们。

    // Show screensaver with current time
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    display_screensaver(tm_info->tm_hour, tm_info->tm_min);

    // Ensure WiFi is disconnected in low power mode
    if (wifi_is_connected()) {
        ESP_LOGI(TAG, "低功耗模式: 断开WiFi");
        wifi_disconnect_safe();
    }

    // Light sleep loop: wake up every minute to refresh time
    while (1) {
        bool timer_wakeup = power_enter_low_power_mode(LIGHT_SLEEP_INTERVAL_US);

        if (!timer_wakeup) {
            // GPIO wakeup (button pressed) -> return to caller to re-enter display mode
            ESP_LOGI(TAG, "按键唤醒，进入显示模式");
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

        // Fetch hourly and daily forecast data (best-effort, WiFi still up)
        if (weather_fetch_hourly(&s_hourly_forecast)) {
            s_has_forecast = true;
        }
        if (weather_fetch_daily(&s_daily_forecast)) {
            s_has_forecast = true;
        }

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
    uint32_t timeout_counter = 0;
    bool config_success = false;  // 初始默认失败

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
        snprintf(progress, sizeof(progress), "Wechat SmartConfig%.*s", dots, "...");
        display_loading(progress);
        }
    }

// 走到这里只有两种情况：超时 / 按键退出，统一保持 false
// 上方只有配网成功分支会改成 true

config_cleanup:
    if (config_success) {
        // 配网成功：只停止 SmartConfig，保持 WiFi 连接不断
        // 由后续的 enter_update_mode() 直接使用已连好的 WiFi
        wifi_smartconfig_done();
    } else {
        // 配网失败/退出：完全停止 SmartConfig 和 WiFi
        wifi_stop_smartconfig();
    }
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
