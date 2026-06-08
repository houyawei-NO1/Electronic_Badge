/**
 * @file display.c
 * @brief GC9A01 display driver with LVGL UI
 *
 * Uses esp_lcd + esp_lcd_gc9a01 + esp_lvgl_port to drive
 * GC9A01 1.28" round display (240x240) with LVGL widgets.
 * UI screens are designed with PicoPixel and exported to ui/
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "display.h"
#include "config.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_gc9a01.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ui.h"

static const char* TAG = "显示";

// =============================================================================
// Module state
// =============================================================================
static bool is_initialized = false;
static lv_disp_t *disp_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

// Animation state
static lv_timer_t *colon_blink_timer = NULL;
static bool colon_visible = true;
static int last_hour = 0;
static int last_minute = 0;
static int last_wday = 1;

// =============================================================================
// GC9A01 manufacturer initialization sequence
// =============================================================================
static const gc9a01_lcd_init_cmd_t gc9a01_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0xEF, (uint8_t[]){0x00}, 0, 0},
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
    {0xB6, (uint8_t[]){0x20}, 1, 0},
    {0x36, (uint8_t[]){0x08}, 1, 0},
    {0x3A, (uint8_t[]){0x05}, 1, 0},
    {0x90, (uint8_t[]){0x08, 0x08, 0x08, 0x08}, 4, 0},
    {0xBD, (uint8_t[]){0x06}, 1, 0},
    {0xBF, (uint8_t[]){0x1C}, 1, 0},
    {0xA7, (uint8_t[]){0x45}, 1, 0},
    {0xA9, (uint8_t[]){0xBB}, 1, 0},
    {0xB8, (uint8_t[]){0x63}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x60, 0x01, 0x04}, 3, 0},
    {0xC3, (uint8_t[]){0x17}, 1, 0},
    {0xC4, (uint8_t[]){0x17}, 1, 0},
    {0xC9, (uint8_t[]){0x25}, 1, 0},
    {0xBE, (uint8_t[]){0x11}, 1, 0},
    {0xE1, (uint8_t[]){0x10, 0x0E}, 2, 0},
    {0xDF, (uint8_t[]){0x21, 0x10, 0x02}, 3, 0},
    {0xF0, (uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF1, (uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xF2, (uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF3, (uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xED, (uint8_t[]){0x1B, 0x0B}, 2, 0},
    {0xAC, (uint8_t[]){0x47}, 1, 0},
    {0xAE, (uint8_t[]){0x77}, 1, 0},
    {0xCB, (uint8_t[]){0x02}, 1, 0},
    {0xCD, (uint8_t[]){0x63}, 1, 0},
    {0x70, (uint8_t[]){0x07, 0x09, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9, 0},
    {0xE8, (uint8_t[]){0x34}, 1, 0},
    {0x62, (uint8_t[]){0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12, 0},
    {0x63, (uint8_t[]){0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12, 0},
    {0x64, (uint8_t[]){0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7, 0},
    {0x66, (uint8_t[]){0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10, 0},
    {0x67, (uint8_t[]){0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10, 0},
    {0x74, (uint8_t[]){0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7, 0},
    {0x35, (uint8_t[]){0x00}, 0, 0},
    {0x21, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

#define GC9A01_INIT_CMD_COUNT (sizeof(gc9a01_init_cmds) / sizeof(gc9a01_init_cmds[0]))

// =============================================================================
// Helper: Get weather name string (font-safe, avoids missing glyphs)
// Font has: 晴雨雪大小中冷温电  —  DOES NOT have: 云阴湿风级雾霾雷暴扫码微唤醒按键网
// For missing-char weather, use English as fallback.
// =============================================================================
static const char* get_weather_name(int16_t weather_code)
{
    // Use weather_code to pick a safe display name
    if (weather_code == 100) return "晴";
    if (weather_code >= 101 && weather_code <= 104) return "Cloudy";  // 多云/少云/晴间多云/阴
    if (weather_code >= 300 && weather_code <= 301) return "Rain";    // 阵雨
    if (weather_code >= 302 && weather_code <= 304) return "Storm";   // 雷阵雨
    if (weather_code >= 305 && weather_code <= 308) return "小雨";     // 小雨/中雨/大雨/暴雨
    if (weather_code >= 309 && weather_code <= 313) return "大雨";     // 各种雨
    if (weather_code == 400) return "小雪";
    if (weather_code == 401) return "中雪";
    if (weather_code >= 402 && weather_code <= 407) return "大雪";
    if (weather_code == 408) return "雨雪";  // 雨夹雪
    if (weather_code >= 500 && weather_code <= 502) return "Fog";      // 雾
    if (weather_code >= 511 && weather_code <= 515) return "Haze";     // 霾/沙尘
    if (weather_code >= 350 && weather_code <= 399) return "Storm";    // 特殊雷暴/雪
    if (weather_code >= 150 && weather_code <= 199) return "晴";
    return "晴";
}

// =============================================================================
// display_init - Initialize SPI bus, GC9A01 panel, and LVGL
// =============================================================================
esp_err_t display_init(void)
{
    if (is_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化显示 (LVGL + GC9A01)...");

    // Step 1: Initialize SPI bus
    ESP_LOGI(TAG, "初始化SPI总线 (SCK=%d, MOSI=%d)", GPIO_SPI_SCK, GPIO_SPI_MOSI);

    spi_bus_config_t buscfg = {
        .sclk_io_num = GPIO_SPI_SCK,
        .mosi_io_num = GPIO_SPI_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 80 * sizeof(uint16_t),
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 2: Create LCD panel IO (SPI)
    ESP_LOGI(TAG, "创建面板IO (DC=%d, CS=%d)", GPIO_LCD_DC, GPIO_LCD_CS);

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_LCD_CS,
        .dc_gpio_num = GPIO_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_FREQ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .lsb_first = 0,
        },
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "面板IO初始化失败: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }
    io_handle = panel_io;

    // Step 3: Create GC9A01 panel with manufacturer init sequence
    ESP_LOGI(TAG, "安装GC9A01面板驱动 (RST=%d)", GPIO_LCD_RST);

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
        ESP_LOGE(TAG, "GC9A01面板初始化失败: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(panel_io);
        spi_bus_free(SPI2_HOST);
        return ret;
    }
    panel_handle = panel;

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    ESP_LOGI(TAG, "GC9A01面板初始化成功");

    // Step 4: Initialize LVGL
    ESP_LOGI(TAG, "初始化LVGL...");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL端口初始化失败: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(panel_io);
        spi_bus_free(SPI2_HOST);
        return ret;
    }

    // Step 5: Add display to LVGL
    ESP_LOGI(TAG, "添加显示到LVGL...");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = DISPLAY_WIDTH * DISPLAY_HEIGHT / 10 * sizeof(uint16_t),
        .double_buffer = false,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    disp_handle = lvgl_port_add_disp(&disp_cfg);
    if (disp_handle == NULL) {
        ESP_LOGE(TAG, "添加显示到LVGL失败");
        lvgl_port_deinit();
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(panel_io);
        spi_bus_free(SPI2_HOST);
        return ESP_FAIL;
    }

    // Step 6: Initialize PicoPixel UI
    ESP_LOGI(TAG, "初始化PicoPixel UI...");
    ui_init();

    // Step 7: Set all screen backgrounds to pure black
    // This ensures no gray background bleeding through icons/containers
    lvgl_port_lock(0);
    // LVGL active screen
    lv_obj_t *scr = lv_disp_get_scr_act(disp_handle);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // All PicoPixel screens
    lv_obj_set_style_bg_color(objects.badge_main, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(objects.badge_main, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(objects.badge_success, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(objects.badge_success, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(objects.badge_loading, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(objects.badge_loading, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(objects.badge_config, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(objects.badge_config, LV_OPA_COVER, 0);
    lvgl_port_unlock();

    is_initialized = true;
    ESP_LOGI(TAG, "显示初始化成功 (LVGL + GC9A01 + PicoPixel)");
    return ESP_OK;
}

// =============================================================================
// Backlight control
// =============================================================================
void display_backlight_on(void)
{
    gpio_set_level(GPIO_LCD_RST, 1);
}

void display_backlight_off(void)
{
    // 息屏前停止动画和timer，避免引用已删除的对象
    // 不要调用 lv_obj_clean()，这会触发 LVGL 重绘，而马上又要 display_deinit()
    if (is_initialized && disp_handle) {
        lvgl_port_lock(0);
        if (colon_blink_timer) {
            lv_timer_del(colon_blink_timer);
            colon_blink_timer = NULL;
        }
        lv_anim_del(NULL, NULL);  // 停止所有动画
        lvgl_port_unlock();
    }
    gpio_set_level(GPIO_LCD_RST, 0);
}

// =============================================================================
// Screen switching functions (PicoPixel UI)
// =============================================================================

// =============================================================================
// 天气图标映射表
// =============================================================================
// Weather icon declarations (from images.h)
#include "images.h"

// 天气代码转图标指针
static const lv_img_dsc_t* weather_code_to_icon(int16_t code)
{
    switch (code) {
        case 100: return &weather_100;
        case 101: return &weather_101;
        case 102: return &weather_102;
        case 103: return &weather_103;
        case 104: return &weather_104;
        case 300: case 301: return &weather_300;
        case 302: case 303: case 304: return &weather_302;
        case 305: case 309: return &weather_305;
        case 306: return &weather_306;
        case 307: case 308: case 310: case 311: case 312: case 313: return &weather_307;
        case 400: return &weather_400;
        case 401: case 408: return &weather_401;
        case 402: case 403: case 409: case 410: return &weather_402;
        case 404: case 405: case 406: case 407: return &weather_404;
        case 500: case 502: case 503: case 504: case 514: case 515: return &weather_501;
        case 501: return &weather_501;
        case 507: case 508: case 509: case 510: return &weather_511;
        case 511: case 512: case 513: return &weather_511;
        default: return &weather_100;  // 默认晴
    }
}

// =============================================================================
// Colon blink timer callback — 负责时间标签的冒号闪烁 + 文本渲染
// display_main_screen 只更新 last_hour/last_minute/last_wday，不直接写 label_time
// =============================================================================
static void colon_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    colon_visible = !colon_visible;
    if (objects.label_time) {
        char buf[32];
        const char* sep = colon_visible ? ":" : " ";
        snprintf(buf, sizeof(buf), "%02d%s%02d  %d", last_hour, sep, last_minute, last_wday);
        lv_label_set_text(objects.label_time, buf);
    }
}

// =============================================================================
// Label breathe animation callback
// 签名必须是 lv_anim_exec_xcb_t 即 void*(var), int32_t(v)
// 否则 lv_anim_del / lv_anim_start 的类型匹配会出问题
// =============================================================================
static void label_breathe_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_set_style_text_opa(obj, (lv_opa_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// =============================================================================
// Weather icon float animation callback
// =============================================================================
static void icon_float_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_set_y(obj, v);
}

// =============================================================================
// Arc rotation animation callback
// =============================================================================
static void arc_rotation_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_arc_set_rotation(obj, v);
}

void display_main_screen(int hour, int minute, const char* weather_text,
                         int temperature, uint32_t last_update, int16_t weather_code,
                         uint8_t humidity, uint8_t wind_scale)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    loadScreen(SCREEN_ID_BADGE_MAIN);

    // Save time state for colon blink timer
    last_hour = hour;
    last_minute = minute;
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    int wday = tm_info->tm_wday;
    last_wday = (wday == 0) ? 7 : wday;

    // === 1. 更新时间（Montserrat 24px，纯数字不需要中文字体）===
    // 注意：label_time 的文本由 colon_blink_timer 负责更新（冒号闪烁效果）
    // 这里只更新 font + 立即刷新一次显示，避免500ms空白
    if (objects.label_time) {
        lv_obj_set_style_text_font(objects.label_time, &lv_font_montserrat_24, 0);
        // 立即渲染一次，不要等下一个 timer 周期
        char buf[32];
        const char* sep = colon_visible ? ":" : " ";
        snprintf(buf, sizeof(buf), "%02d%s%02d  %d", hour, sep, minute, last_wday);
        lv_label_set_text(objects.label_time, buf);
        ESP_LOGI(TAG, "[UI] label_time: \"%s\"", buf);
    }

    // Start colon blink timer if not already running (500ms = 2Hz blink)
    if (colon_blink_timer == NULL) {
        colon_blink_timer = lv_timer_create(colon_blink_timer_cb, 500, NULL);
        ESP_LOGI(TAG, "[动画] 冒号闪烁 timer 已创建 (500ms)");
    }

    // === 2. 更新日期（Montserrat 18px，纯数字+点号不需要中文字体）===
    if (objects.label_date) {
        lv_obj_set_style_text_font(objects.label_date, &lv_font_montserrat_18, 0);
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%d.%d.%d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);
        lv_label_set_text(objects.label_date, date_buf);
        ESP_LOGI(TAG, "[UI] label_date: \"%s\"", date_buf);
    }

    // === 3. 更新天气图标 ===
    if (objects.qweather_icons) {
        const lv_img_dsc_t* icon = weather_code_to_icon(weather_code);
        lv_img_set_src(objects.qweather_icons, icon);
        ESP_LOGI(TAG, "[UI] qweather_icons: weather_code=%d", weather_code);
    }

    // === 4. 更新天气文字（基于 weather_code，避免 API 返回的中文缺字） ===
    if (objects.label_weather) {
        lv_obj_set_style_text_font(objects.label_weather, &lv_font_simsun_16_cjk, 0);
        const char* safe_name = get_weather_name(weather_code);
        lv_label_set_text(objects.label_weather, safe_name);
        ESP_LOGI(TAG, "[UI] label_weather: \"%s\" (code=%d, orig=\"%s\")",
                 safe_name, weather_code, weather_text ? weather_text : "(null)");
    }

    // === 5. 更新温度（"度"在字体中，℃符号不在）===
    if (objects.label_temp) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d C", temperature);
        lv_label_set_text(objects.label_temp, buf);
        ESP_LOGI(TAG, "[UI] label_temp: \"%s\"", buf);
    }

    // === 6. 更新详细信息（湿度、风力 - 用英文避免中文缺字）===
    if (objects.label_detail) {
        lv_obj_set_style_text_font(objects.label_detail, &lv_font_simsun_16_cjk, 0);
        char detail_buf[64];
        snprintf(detail_buf, sizeof(detail_buf), "Hum:%d%% Wind:%d", humidity, wind_scale);
        lv_label_set_text(objects.label_detail, detail_buf);
        ESP_LOGI(TAG, "[UI] label_detail: \"%s\"", detail_buf);
    }

    // === 7. 更新最后更新时间 ===
    if (objects.label_update) {
        lv_obj_set_style_text_font(objects.label_update, &lv_font_simsun_16_cjk, 0);
        if (last_update > 0) {
            time_t update_time = (time_t)last_update;
            struct tm* utm = localtime(&update_time);
            char update_buf[64];
            snprintf(update_buf, sizeof(update_buf), "%d.%d.%d %02d:%02d更新",
                     utm->tm_year + 1900, utm->tm_mon + 1, utm->tm_mday,
                     utm->tm_hour, utm->tm_min);
            lv_label_set_text(objects.label_update, update_buf);
            ESP_LOGI(TAG, "[UI] label_update: \"%s\"", update_buf);
        } else {
            lv_label_set_text(objects.label_update, "未更新");
            ESP_LOGI(TAG, "[UI] label_update: \"未更新\"");
        }
    }

    // === 8. Start/update animations ===
    // label_update 呼吸动画：用正确的2参数回调 + 先清旧动画
    if (objects.label_update) {
        lv_anim_del(objects.label_update, label_breathe_anim_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.label_update);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)label_breathe_anim_cb);
        lv_anim_set_values(&a, 60, 255);  // 明显的明暗变化
        lv_anim_set_time(&a, 500);
        lv_anim_set_playback_time(&a, 500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        ESP_LOGI(TAG, "[动画] label_update 呼吸动画已启动 (1秒周期)");
    }

    // 天气图标浮动动画：先清除旧动画，再启动新的
    if (objects.qweather_icons) {
        lv_anim_del(objects.qweather_icons, icon_float_anim_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.qweather_icons);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)icon_float_anim_cb);
        lv_anim_set_values(&a, 80, 86);  // y: 80 -> 86 -> 80 (±3px float)
        lv_anim_set_time(&a, 2000);
        lv_anim_set_playback_time(&a, 2000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        ESP_LOGI(TAG, "[动画] 天气图标浮动已启动 (4秒周期)");
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "主屏幕: %02d:%02d %s %dC Hum:%d%% Wind:%d",
             hour, minute, weather_text ? weather_text : "--", temperature,
             humidity, wind_scale);
}

void display_loading(const char* message)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    loadScreen(SCREEN_ID_BADGE_LOADING);

    // 设置中间的状态标签
    if (objects.label_1) {
        lv_obj_set_style_text_font(objects.label_1, &lv_font_simsun_16_cjk, 0);
        if (message && strlen(message) > 0) {
            lv_label_set_text(objects.label_1, message);
        } else {
            lv_label_set_text(objects.label_1, "同步中...");
        }
    }

    // 配置 Arc: 更小的弧段 + 旋转动画
    // 原始 screens.c 把 value 设成 359 (整圈)，看不出在转动
    // 这里改成 60° 的短弧，背景轨道淡化，然后旋转整个 arc 让它转起来
    if (objects.arc_1) {
        // 指示器覆盖 60° 的扇形弧（范围 0-360，值=60）
        lv_arc_set_value(objects.arc_1, 60);
        // 从 0 度开始（把起点放到顶部偏右一点视觉更自然）
        lv_arc_set_rotation(objects.arc_1, 0);
        // 背景轨道淡化 —— 让旋转的弧段更明显
        lv_obj_set_style_arc_opa(objects.arc_1, 50, LV_PART_MAIN | LV_STATE_DEFAULT);

        // 启动旋转动画：从 0° 转到 360°，约 1.5 秒一圈，无限重复
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.arc_1);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)arc_rotation_anim_cb);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 1500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    lvgl_port_unlock();
}

void display_loading_status(const char* status)
{
    if (!is_initialized || !disp_handle) return;
    if (!status || strlen(status) == 0) return;

    lvgl_port_lock(0);
    if (objects.label_1) {
        lv_obj_set_style_text_font(objects.label_1, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_1, status);
    }
    lvgl_port_unlock();
}

void display_config_mode(void)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    loadScreen(SCREEN_ID_BADGE_CONFIG);

    // 设置中文字体并更新文字
    if (objects.label_4) {
        lv_obj_set_style_text_font(objects.label_4, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_4, "配置模式");
    }
    if (objects.label_5) {
        lv_obj_set_style_text_font(objects.label_5, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_5, "WAKE");
    }
    if (objects.label_6) {
        lv_obj_set_style_text_font(objects.label_6, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_6, "WECHAT");
    }
    if (objects.label_7) {
        lv_obj_set_style_text_font(objects.label_7, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_7, "SCAN");
    }

    // 启动 Arc 旋转动画
    if (objects.arc_3) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.arc_3);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)arc_rotation_anim_cb);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    lvgl_port_unlock();
}

void display_config_success(void)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    loadScreen(SCREEN_ID_BADGE_SUCCESS);

    // 设置中文字体并更新文字
    if (objects.label_2) {
        lv_obj_set_style_text_font(objects.label_2, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_2, "配置成功");
    }
    if (objects.label_3) {
        lv_obj_set_style_text_font(objects.label_3, &lv_font_simsun_16_cjk, 0);
        lv_label_set_text(objects.label_3, "PRESS TO");
    }

    lvgl_port_unlock();
}

// =============================================================================
// display_deinit - Cleanup
// =============================================================================
void display_deinit(void)
{
    if (!is_initialized) return;

    ESP_LOGI(TAG, "反初始化显示...");

    // Step 1: 停止所有 LVGL 动画和 timer
    if (colon_blink_timer) {
        lv_timer_del(colon_blink_timer);
        colon_blink_timer = NULL;
    }
    lv_anim_del(NULL, NULL);

    // Step 2: 在 LVGL task 还在运行时，先安全移除显示
    // 必须在 lvgl_port_deinit() 之前做，否则 LVGL 已被销毁
    if (disp_handle) {
        lvgl_port_lock(0);
        lvgl_port_remove_disp(disp_handle);
        lvgl_port_unlock();
        disp_handle = NULL;
    }

    // Step 3: 停止 LVGL task（设置 running=false，task 会自我退出）
    lvgl_port_deinit();

    // 等待 LVGL task 完全退出并自我删除
    vTaskDelay(pdMS_TO_TICKS(100));

    // Step 4: 删除硬件资源
    if (panel_handle) {
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }

    if (io_handle) {
        esp_lcd_panel_io_del(io_handle);
        io_handle = NULL;
    }

    spi_bus_free(SPI2_HOST);

    is_initialized = false;
    ESP_LOGI(TAG, "显示反初始化完成");
}
