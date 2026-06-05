/**
 * @file display.c
 * @brief GC9A01 display driver with LVGL v9 UI implementation
 *
 * Uses esp_lcd + esp_lcd_gc9a01 + esp_lvgl_port to drive
 * GC9A01 1.28" round display (240x240) with LVGL widgets.
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "display.h"
#include "config.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_gc9a01.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char* TAG = "Display";

// =============================================================================
// Module state
// =============================================================================
static bool is_initialized = false;
static lv_disp_t *disp_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

// =============================================================================
// GC9A01 manufacturer initialization sequence
// (from: 1.28寸初始化HSD+GC9A01.txt)
// =============================================================================
static const gc9a01_lcd_init_cmd_t gc9a01_init_cmds[] = {
    // Enable inter-command (0xFE + 0xEF)
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0xEF, (uint8_t[]){0x00}, 0, 0},

    // Internal register settings
    {0xEB, (uint8_t[]){0x14}, 1, 0},
    {0x84, (uint8_t[]){0x40}, 1, 0},
    {0x85, (uint8_t[]){0xF1}, 1, 0},
    {0x86, (uint8_t[]){0x98}, 1, 0},
    {0x87, (uint8_t[]){0x28}, 1, 0},
    {0x88, (uint8_t[]){0x0A}, 1, 0},
    {0x89, (uint8_t[]){0x21}, 1, 0},
    {0x8A, (uint8_t[]){0x00}, 1, 0},
    {0x8B, (uint8_t[]){0x80}, 1, 0},
    {0x8C, (uint8_t[]){0x01}, 1, 0},
    {0x8D, (uint8_t[]){0x00}, 1, 0},
    {0x8E, (uint8_t[]){0xDF}, 1, 0},
    {0x8F, (uint8_t[]){0x52}, 1, 0},

    // Display function control
    {0xB6, (uint8_t[]){0x20}, 1, 0},

    // Memory Access Control (0x08 = BGR only, no mirror)
    // 0x48 had MX (mirror X) bit set causing horizontal flip
    {0x36, (uint8_t[]){0x08}, 1, 0},

    // Pixel Format Set (0x05 = 16-bit)
    {0x3A, (uint8_t[]){0x05}, 1, 0},

    // Additional settings
    {0x90, (uint8_t[]){0x08, 0x08, 0x08, 0x08}, 4, 0},
    {0xBD, (uint8_t[]){0x06}, 1, 0},
    {0xBF, (uint8_t[]){0x1C}, 1, 0},
    {0xA7, (uint8_t[]){0x45}, 1, 0},
    {0xA9, (uint8_t[]){0xBB}, 1, 0},
    {0xB8, (uint8_t[]){0x63}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},

    // Enable inter-command again
    {0xFF, (uint8_t[]){0x60, 0x01, 0x04}, 3, 0},

    // Power control
    {0xC3, (uint8_t[]){0x17}, 1, 0},
    {0xC4, (uint8_t[]){0x17}, 1, 0},
    {0xC9, (uint8_t[]){0x25}, 1, 0},
    {0xBE, (uint8_t[]){0x11}, 1, 0},
    {0xE1, (uint8_t[]){0x10, 0x0E}, 2, 0},
    {0xDF, (uint8_t[]){0x21, 0x10, 0x02}, 3, 0},

    // Gamma settings
    {0xF0, (uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF1, (uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xF2, (uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF3, (uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},

    // More internal settings
    {0xED, (uint8_t[]){0x1B, 0x0B}, 2, 0},
    {0xAC, (uint8_t[]){0x47}, 1, 0},
    {0xAE, (uint8_t[]){0x77}, 1, 0},
    {0xCB, (uint8_t[]){0x02}, 1, 0},
    {0xCD, (uint8_t[]){0x63}, 1, 0},

    // 70h settings
    {0x70, (uint8_t[]){0x07, 0x09, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9, 0},

    // Frame rate
    {0xE8, (uint8_t[]){0x34}, 1, 0},

    // 62h/63h/64h settings
    {0x62, (uint8_t[]){0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12, 0},
    {0x63, (uint8_t[]){0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12, 0},
    {0x64, (uint8_t[]){0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7, 0},

    // 66h/67h/74h settings
    {0x66, (uint8_t[]){0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10, 0},
    {0x67, (uint8_t[]){0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10, 0},
    {0x74, (uint8_t[]){0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7, 0},

    // Tearing effect line off
    {0x35, (uint8_t[]){0x00}, 0, 0},

    // Display inversion on
    {0x21, (uint8_t[]){0x00}, 0, 0},

    // Sleep out
    {0x11, (uint8_t[]){0x00}, 0, 120},

    // Display on
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

#define GC9A01_INIT_CMD_COUNT (sizeof(gc9a01_init_cmds) / sizeof(gc9a01_init_cmds[0]))

// =============================================================================
// LVGL color definitions (lv_color_t)
// =============================================================================
#define LV_COLOR_BG_DARK     lv_color_hex(0x0B1E3A)   // Deep navy blue
#define LV_COLOR_BG_MID      lv_color_hex(0x152952)   // Medium blue
#define LV_COLOR_TIME_WHITE  lv_color_hex(0xFFFFFF)
#define LV_COLOR_TEMP_CYAN   lv_color_hex(0x80DEEA)
#define LV_COLOR_UPDATE_GRAY lv_color_hex(0x607D8B)
#define LV_COLOR_LOADING_BG  lv_color_hex(0x1A237E)
#define LV_COLOR_LOADING_TXT lv_color_hex(0xFFFFFF)
#define LV_COLOR_CONFIG_BG   lv_color_hex(0x0D47A1)
#define LV_COLOR_CONFIG_TXT  lv_color_hex(0xFFFFFF)
#define LV_COLOR_SUCCESS_BG  lv_color_hex(0x2E7D32)
#define LV_COLOR_SUCCESS_TXT lv_color_hex(0xFFFFFF)
#define LV_COLOR_ARC_BG      lv_color_hex(0x1A237E)
#define LV_COLOR_ARC_IND     lv_color_hex(0x42A5F5)

// =============================================================================
// Helper: Determine weather type from text
// =============================================================================
static weather_type_t get_weather_type(const char* weather_text)
{
    if (!weather_text) return WEATHER_SUNNY;
    if (strstr(weather_text, "rain") || strstr(weather_text, "Rain") ||
        strstr(weather_text, "\xe9\x9b\xa8") /* 雨 */) {
        return WEATHER_RAINY;
    }
    if (strstr(weather_text, "cloud") || strstr(weather_text, "Cloud") ||
        strstr(weather_text, "\xe4\xba\x91") /* 云 */ ||
        strstr(weather_text, "\xe9\x98\xb4") /* 阴 */) {
        return WEATHER_CLOUDY;
    }
    if (strstr(weather_text, "snow") || strstr(weather_text, "Snow") ||
        strstr(weather_text, "\xe9\x9b\xaa") /* 雪 */) {
        return WEATHER_SNOWY;
    }
    if (strstr(weather_text, "thunder") || strstr(weather_text, "Thunder") ||
        strstr(weather_text, "\xe9\x9b\xb7") /* 雷 */) {
        return WEATHER_THUNDER;
    }
    if (strstr(weather_text, "fog") || strstr(weather_text, "Fog") ||
        strstr(weather_text, "\xe9\x9b\xbe") /* 雾 */) {
        return WEATHER_FOGGY;
    }
    return WEATHER_SUNNY;
}

// =============================================================================
// Helper: Get weather icon symbol (LVGL built-in)
// =============================================================================
static const char* get_weather_symbol(weather_type_t weather)
{
    switch (weather) {
        case WEATHER_SUNNY:   return LV_SYMBOL_CHARGE;    // Sun-like symbol
        case WEATHER_CLOUDY:  return LV_SYMBOL_NEW_LINE;  // Cloud-like
        case WEATHER_RAINY:   return LV_SYMBOL_SETTINGS;  // No rain symbol
        case WEATHER_SNOWY:   return LV_SYMBOL_IMAGE;     // No snow symbol
        case WEATHER_THUNDER: return LV_SYMBOL_BELL;      // No thunder symbol
        case WEATHER_FOGGY:   return LV_SYMBOL_EYE_OPEN;  // Eye for visibility
        default:              return LV_SYMBOL_CHARGE;
    }
}

// =============================================================================
// Helper: Get weather background color
// =============================================================================
static lv_color_t get_weather_bg_color(weather_type_t weather)
{
    switch (weather) {
        case WEATHER_SUNNY:   return lv_color_hex(0x0B3D91);  // Sunny blue
        case WEATHER_CLOUDY:  return lv_color_hex(0x37474F);  // Gray
        case WEATHER_RAINY:   return lv_color_hex(0x1A237E);  // Dark blue
        case WEATHER_SNOWY:   return lv_color_hex(0x4FC3F7);  // Light blue
        case WEATHER_THUNDER: return lv_color_hex(0x263238);  // Dark gray
        case WEATHER_FOGGY:   return lv_color_hex(0x546E7A);  // Fog gray
        default:              return lv_color_hex(0x0B3D91);
    }
}

// =============================================================================
// Helper: Get weather name string
// =============================================================================
static const char* get_weather_name(weather_type_t weather)
{
    switch (weather) {
        case WEATHER_SUNNY:   return "Sunny";
        case WEATHER_CLOUDY:  return "Cloudy";
        case WEATHER_RAINY:   return "Rainy";
        case WEATHER_SNOWY:   return "Snowy";
        case WEATHER_THUNDER: return "Thunder";
        case WEATHER_FOGGY:   return "Foggy";
        default:              return "Sunny";
    }
}

// =============================================================================
// display_init - Initialize SPI bus, GC9A01 panel, and LVGL
// =============================================================================
esp_err_t display_init(void)
{
    if (is_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing display (LVGL + GC9A01)...");

    // =====================================================================
    // Step 1: Initialize SPI bus
    // =====================================================================
    ESP_LOGI(TAG, "Initializing SPI bus (SCK=%d, MOSI=%d)", GPIO_SPI_SCK, GPIO_SPI_MOSI);

    spi_bus_config_t buscfg = {
        .sclk_io_num = GPIO_SPI_SCK,
        .mosi_io_num = GPIO_SPI_MOSI,
        .miso_io_num = -1,       // No MISO needed for LCD
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 80 * sizeof(uint16_t),  // 80 lines at a time
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // =====================================================================
    // Step 2: Create LCD panel IO (SPI)
    // =====================================================================
    ESP_LOGI(TAG, "Creating panel IO (DC=%d, CS=%d)", GPIO_LCD_DC, GPIO_LCD_CS);

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_LCD_CS,
        .dc_gpio_num = GPIO_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_FREQ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = NULL,  // No callback needed
        .user_ctx = NULL,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .lsb_first = 0,
        },
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }
    io_handle = panel_io;

    // =====================================================================
    // Step 3: Create GC9A01 panel with manufacturer init sequence
    // =====================================================================
    ESP_LOGI(TAG, "Installing GC9A01 panel driver (RST=%d)", GPIO_LCD_RST);

    esp_lcd_panel_handle_t panel = NULL;
    gc9a01_vendor_config_t vendor_config = {
        .init_cmds = gc9a01_init_cmds,
        .init_cmds_size = GC9A01_INIT_CMD_COUNT,
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ret = esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GC9A01 panel init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(panel_io);
        spi_bus_free(SPI2_HOST);
        return ret;
    }
    panel_handle = panel;

    // Reset and initialize panel
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    ESP_LOGI(TAG, "GC9A01 panel initialized successfully");

    // =====================================================================
    // Step 4: Initialize LVGL
    // =====================================================================
    ESP_LOGI(TAG, "Initializing LVGL...");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(panel_io);
        spi_bus_free(SPI2_HOST);
        return ret;
    }

    // =====================================================================
    // Step 5: Add display to LVGL
    // =====================================================================
    ESP_LOGI(TAG, "Adding display to LVGL...");

    // Buffer size: 10% of screen = 240*240/10*2 = 11520 bytes
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = DISPLAY_WIDTH * DISPLAY_HEIGHT / 10 * sizeof(uint16_t),
        .double_buffer = false,  // Save memory on ESP32-C3
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = false,
            .sw_rotate = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    disp_handle = lvgl_port_add_disp(&disp_cfg);
    if (disp_handle == NULL) {
        ESP_LOGE(TAG, "Failed to add display to LVGL");
        lvgl_port_deinit();
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(panel_io);
        spi_bus_free(SPI2_HOST);
        return ESP_FAIL;
    }

    is_initialized = true;
    ESP_LOGI(TAG, "Display initialized successfully (LVGL + GC9A01)");
    return ESP_OK;
}

// =============================================================================
// Backlight control
// =============================================================================
void display_backlight_on(void)
{
    // RST (GPIO5) controls both screen reset and backlight power
    // Keep RST high to maintain screen and backlight power
    gpio_set_level(GPIO_LCD_RST, 1);
}

void display_backlight_off(void)
{
    // RST (GPIO5) controls both screen reset and backlight power
    // Pull RST low to turn off screen and backlight
    // Note: This will also reset the screen, re-init needed on next power-on
    gpio_set_level(GPIO_LCD_RST, 0);
}

// =============================================================================
// display_clear - Clear screen with LVGL
// =============================================================================
void display_clear(uint16_t color)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp_handle);
    lv_obj_set_style_bg_color(scr, lv_color_hex(color), 0);
    lv_obj_clean(scr);
    lvgl_port_unlock();
}

// =============================================================================
// display_main_screen - Main screen with time, weather, temperature
// =============================================================================
void display_main_screen(int hour, int minute, const char* weather_text,
                         int temperature, uint32_t last_update)
{
    if (!is_initialized || !disp_handle) return;

    weather_type_t weather = get_weather_type(weather_text);
    lv_color_t bg_color = get_weather_bg_color(weather);

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp_handle);

    // Clean screen
    lv_obj_clean(scr);

    // Set background color
    lv_obj_set_style_bg_color(scr, bg_color, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // -----------------------------------------------------------------
    // Weather icon area (upper portion, centered)
    // -----------------------------------------------------------------
    lv_obj_t *icon_label = lv_label_create(scr);
    lv_label_set_text(icon_label, get_weather_symbol(weather));
    lv_obj_set_style_text_color(icon_label, LV_COLOR_TIME_WHITE, 0);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_set_style_text_opa(icon_label, LV_OPA_60, 0);
    lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, 30);

    // Weather name below icon
    lv_obj_t *weather_label = lv_label_create(scr);
    lv_label_set_text(weather_label, get_weather_name(weather));
    lv_obj_set_style_text_color(weather_label, LV_COLOR_TEMP_CYAN, 0);
    lv_obj_set_style_text_font(weather_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_align(weather_label, LV_ALIGN_TOP_MID, 0, 95);

    // -----------------------------------------------------------------
    // Temperature display
    // -----------------------------------------------------------------
    lv_obj_t *temp_label = lv_label_create(scr);
    char temp_str[32];
    snprintf(temp_str, sizeof(temp_str), "%dC", temperature);  // Simplified, no degree symbol
    lv_label_set_text(temp_label, temp_str);
    lv_obj_set_style_text_color(temp_label, LV_COLOR_TIME_WHITE, 0);
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 125);

    // -----------------------------------------------------------------
    // Time display (large, centered in lower portion)
    // -----------------------------------------------------------------
    lv_obj_t *time_label = lv_label_create(scr);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
    lv_label_set_text(time_label, time_str);
    lv_obj_set_style_text_color(time_label, LV_COLOR_TIME_WHITE, 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_MID, 0, -50);

    // -----------------------------------------------------------------
    // Last update time (small, gray, at bottom)
    // -----------------------------------------------------------------
    if (last_update > 0) {
        lv_obj_t *update_label = lv_label_create(scr);
        time_t update_time = (time_t)last_update;
        struct tm* tm_info = localtime(&update_time);
        char update_str[32];
        strftime(update_str, sizeof(update_str), "Updated: %H:%M", tm_info);
        lv_label_set_text(update_label, update_str);
        lv_obj_set_style_text_color(update_label, LV_COLOR_UPDATE_GRAY, 0);
        lv_obj_set_style_text_font(update_label, &lv_font_montserrat_14, 0);
        lv_obj_align(update_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    lvgl_port_unlock();
}

// =============================================================================
// display_loading - Loading screen with spinner and message
// =============================================================================
void display_loading(const char* message)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp_handle);

    // Clean screen
    lv_obj_clean(scr);

    // Set background
    lv_obj_set_style_bg_color(scr, LV_COLOR_LOADING_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Loading text (spinner replaced with simple text for memory saving)
    lv_obj_t *msg_label = lv_label_create(scr);
    if (message) {
        lv_label_set_text(msg_label, message);
    } else {
        lv_label_set_text(msg_label, "Loading...");
    }
    lv_obj_set_style_text_color(msg_label, LV_COLOR_LOADING_TXT, 0);
    lv_obj_set_style_text_font(msg_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(msg_label);

    lvgl_port_unlock();
}

// =============================================================================
// display_config_mode - WiFi configuration screen
// =============================================================================
void display_config_mode(void)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp_handle);

    // Clean screen
    lv_obj_clean(scr);

    // Set background
    lv_obj_set_style_bg_color(scr, LV_COLOR_CONFIG_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // WiFi icon (using LV_SYMBOL_WIFI)
    lv_obj_t *wifi_icon = lv_label_create(scr);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, LV_COLOR_CONFIG_TXT, 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_MID, 0, 30);

    // "CONFIG MODE" title
    lv_obj_t *title_label = lv_label_create(scr);
    lv_label_set_text(title_label, "CONFIG MODE");
    lv_obj_set_style_text_color(title_label, LV_COLOR_CONFIG_TXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 95);

    // Instruction text
    lv_obj_t *info_label = lv_label_create(scr);
    lv_label_set_text(info_label, "Use WeChat Mini Program\n\"Yi Jian Pei Wang\"\nto configure WiFi");
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x90CAF9), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info_label, LV_ALIGN_TOP_MID, 0, 130);

    // Waiting indicator (pulsing dot)
    lv_obj_t *dot_label = lv_label_create(scr);
    lv_label_set_text(dot_label, LV_SYMBOL_BULLET " Waiting...");
    lv_obj_set_style_text_color(dot_label, lv_color_hex(0x64B5F6), 0);
    lv_obj_set_style_text_font(dot_label, &lv_font_montserrat_14, 0);
    lv_obj_align(dot_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    lvgl_port_unlock();
}

// =============================================================================
// display_config_success - Configuration success screen
// =============================================================================
void display_config_success(void)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp_handle);

    // Clean screen
    lv_obj_clean(scr);

    // Set background
    lv_obj_set_style_bg_color(scr, LV_COLOR_SUCCESS_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Success icon (checkmark)
    lv_obj_t *icon_label = lv_label_create(scr);
    lv_label_set_text(icon_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(icon_label, LV_COLOR_SUCCESS_TXT, 0);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, 40);

    // Success text
    lv_obj_t *msg_label = lv_label_create(scr);
    lv_label_set_text(msg_label, "Connected!");
    lv_obj_set_style_text_color(msg_label, LV_COLOR_SUCCESS_TXT, 0);
    lv_obj_set_style_text_font(msg_label, &lv_font_montserrat_14, 0);  // Use available font
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg_label, LV_ALIGN_TOP_MID, 0, 110);

    // Sub text
    lv_obj_t *sub_label = lv_label_create(scr);
    lv_label_set_text(sub_label, "WiFi configured");
    lv_obj_set_style_text_color(sub_label, lv_color_hex(0xA5D6A7), 0);
    lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub_label, LV_ALIGN_TOP_MID, 0, 145);

    lvgl_port_unlock();
}

// =============================================================================
// display_deinit - Cleanup
// =============================================================================
void display_deinit(void)
{
    if (!is_initialized) return;

    ESP_LOGI(TAG, "Deinitializing display...");

    // Remove LVGL display first
    if (disp_handle) {
        lvgl_port_lock(0);
        lvgl_port_remove_disp(disp_handle);
        lvgl_port_unlock();
        disp_handle = NULL;
    }

    // Deinit LVGL port
    lvgl_port_deinit();

    // Delete panel
    if (panel_handle) {
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }

    // Delete panel IO
    if (io_handle) {
        esp_lcd_panel_io_del(io_handle);
        io_handle = NULL;
    }

    // Free SPI bus
    spi_bus_free(SPI2_HOST);

    is_initialized = false;
    ESP_LOGI(TAG, "Display deinitialized");
}
