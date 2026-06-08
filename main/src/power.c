/**
 * @file power.c
 * @brief Power management implementation
 */
#include <string.h>
#include "power.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

static RTC_NOINIT_ATTR uint8_t rtc_backup_data[256];

void power_init(void)
{
    // Configure GPIO for minimum power consumption during sleep
    // All used GPIOs should be configured as input with pull-up/down
    // Note: GPIO0/1 are used by RTC 32.768kHz crystal, do NOT configure
    
    // Configure button pins as inputs with pull-up
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << GPIO_BTN_WAKE) | (1ULL << GPIO_BTN_REFRESH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_conf);
    
    // Configure LCD pins to prevent leakage during deep sleep
    // Note: RST (GPIO5) also controls backlight, pull down to save power
    gpio_config_t lcd_conf = {
        .pin_bit_mask = (1ULL << GPIO_LCD_CS) |
                        (1ULL << GPIO_SPI_SCK) |
                        (1ULL << GPIO_SPI_MOSI) |
                        (1ULL << GPIO_LCD_DC) |
                        (1ULL << GPIO_LCD_RST),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // Pull down to save power
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&lcd_conf);
}

void power_configure_wakeup(uint64_t timer_interval_us)
{
    // Critical: ESP32-C3 RTC GPIO (GPIO0-5) have hold function in deep sleep
    // Must disable hold before configuring wakeup, otherwise GPIO state is locked
    gpio_deep_sleep_hold_dis();
    
    // Explicitly disable RTC pad hold for GPIO4 (WAKE button)
    // RTC GPIO hold is controlled by RTC_CNTL_RTC_PAD_HOLD_REG
    gpio_hold_dis(GPIO_BTN_WAKE);
    
    // Configure button pin with internal pull-up before sleep
    gpio_config_t wake_conf = {
        .pin_bit_mask = 1ULL << GPIO_BTN_WAKE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&wake_conf);
    
    // Ensure pull-up is active
    gpio_set_pull_mode(GPIO_BTN_WAKE, GPIO_PULLUP_ONLY);
    
    // Configure GPIO wakeup using bitmask (ESP32-C3 deep sleep compatible)
    // GPIO4 = bit 4 = 0x10 = 16
    const uint64_t wakeup_pin_mask = 1ULL << GPIO_BTN_WAKE;
    esp_deep_sleep_enable_gpio_wakeup(wakeup_pin_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    
    // Configure timer wakeup if interval > 0
    if (timer_interval_us > 0) {
        esp_sleep_enable_timer_wakeup(timer_interval_us);
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
}

void power_enter_deep_sleep(uint64_t wakeup_interval_us)
{
    // Configure wakeup sources
    power_configure_wakeup(wakeup_interval_us);
    
    // Note: WiFi should be stopped/disconnected by caller before calling this function
    // Caller is responsible for managing WiFi state (see enter_update_mode)
    
    // Enter deep sleep
    esp_deep_sleep_start();
}

esp_sleep_wakeup_cause_t power_get_wakeup_cause(void)
{
    return esp_sleep_get_wakeup_cause();
}

void power_save_rtc_data(const void* data, size_t size)
{
    if (size > sizeof(rtc_backup_data)) {
        size = sizeof(rtc_backup_data);
    }
    memcpy(rtc_backup_data, data, size);
}

bool power_load_rtc_data(void* data, size_t size)
{
    if (size > sizeof(rtc_backup_data)) {
        size = sizeof(rtc_backup_data);
    }
    memcpy(data, rtc_backup_data, size);
    
    // Validate magic number
    rtc_data_t* rtc = (rtc_data_t*)data;
    if (rtc->magic != RTC_DATA_MAGIC) {
        // Invalid data, initialize to zero
        memset(data, 0, size);
        rtc->magic = RTC_DATA_MAGIC;  // Set magic for next save
        return false;  // Data was invalid
    }
    return true;  // Data is valid
}
