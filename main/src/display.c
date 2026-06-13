/**
 * @file display.c
 * @brief GC9A01 display driver with LVGL UI
 *
 * GC9A01 1.28" round LCD (240x240) + LVGL v8.3.x + PicoPixel UI.
 *
 * [CRITICAL HARDWARE NOTE]
 * RST (GPIO5) controls BOTH panel reset AND backlight power on this
 * module. RST HIGH → backlight on; RST LOW → backlight off. There is
 * no separate backlight-enable pin.
 *
 * [WEATHER ICONS — ZERO RUN-TIME DECODING]
 *   Weather icons are pre-converted from PNG to raw RGB565+alpha binary
 *   at BUILD TIME by scripts/png_to_rgb565a.py and stored in SPIFFS as
 *   <code>-fill.bin. At run-time we simply fread() into a malloc'd buffer
 *   and hand the lv_img_dsc_t pointer to LVGL.
 *
 *   Icon size: 48x48 (fits 51 icons in 512 KB SPIFFS partition.
 *   Night codes (150-153, 350-351, 456-457) share day icons (100-103,
 *   300-301, 406-407) via weather_code_to_bin_path() mapping — no separate
 *   bin files exist for night codes.
 *
 *   Why not decode PNG at run-time?
 *   - LVGL's png decoder uses lv_mem_alloc/lv_mem_free (LVGL internal heap)
 *     NOT the standard heap. Calling free() on an lv_mem_alloc'd pointer
 *     triggers heap_caps_free → assert fail → crash.
 *   - Calling lv_mem_alloc/lv_mem_free outside lvgl_port_lock is also
 *     a concurrency bug — LVGL's allocator walks its free-list without
 *     any other mutex protection than the port lock.
 *   - PNG Huffman decoding is CPU-heavy; doing it while holding the port
 *     lock starves the LVGL refresh task and triggers the task watchdog.
 *
 *   Binary file layout (little-endian, tightly packed):
 *       bytes 0-1 : uint16_t width
 *       bytes 2-3 : uint16_t height
 *       bytes 4+  : w * h * 3 bytes — for each pixel:
 *                     byte0 = (RGB565) & 0xFF
 *                     byte1 = (RGB565 >> 8) & 0xFF
 *                     byte2 = alpha (0 transparent, 255 opaque)
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "display.h"
#include "config.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ui.h"
#include "esp_spiffs.h"

static const char *TAG = "显示";

/* =========================================================================
 * Module state
 * ========================================================================= */
typedef struct {
    bool valid;
    int16_t code;           /* cached weather code (skip re-load on match) */
    uint8_t *pixel_buf;     /* malloc'd buffer: [2B width][2B height][w*h*3B pixels] */
    lv_img_dsc_t dsc;       /* LVGL descriptor; dsc.data points into pixel_buf */
} cached_icon_t;

static cached_icon_t cached_weather_icon;
static bool is_initialized = false;

bool display_is_initialized(void)
{
    return is_initialized;
}
static lv_disp_t *disp_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static lv_timer_t *colon_blink_timer = NULL;
static bool colon_visible = true;

/* Time state (for colon blink) */
static int last_hour = -1;
static int last_minute = -1;
static int last_wday = -1;

/* =========================================================================
 * SPIFFS helpers
 * ========================================================================= */

/**
 * @brief Map a QWeather condition code → SPIFFS path of the pre-decoded
 *        RGB565+alpha binary file.
 */
static void weather_code_to_bin_path(int16_t code, char *buf, size_t buf_size)
{
    /* "夜间" code aliases to the 日间 icon for the same weather */
    int16_t day_code = code;
    switch (code) {
        case 150: day_code = 100; break;
        case 151: day_code = 101; break;
        case 152: day_code = 102; break;
        case 153: day_code = 103; break;
        case 350: day_code = 300; break;
        case 351: day_code = 301; break;
        case 456: day_code = 406; break;
        case 457: day_code = 407; break;
    }
    snprintf(buf, buf_size, "/spiffs/%d-fill.bin", day_code);
}

static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "初始化 SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("spiffs", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%dKB, used=%dKB", total / 1024, used / 1024);
    }
    return ESP_OK;
}

/* =========================================================================
 * Weather icon loader — fread() the pre-converted RGB565+alpha binary
 * =========================================================================
 *
 * IMPORTANT: This function only uses malloc / free / fopen / fread.
 * It NEVER calls lv_mem_alloc / lv_mem_free / lodepng_decode32. That
 * guarantees it is safe to call from ANY task at ANY time, with or
 * without lvgl_port_lock held.
 * ========================================================================= */
esp_err_t display_prepare_weather_icon(int16_t weather_code)
{
    /* Drop the old buffer before allocating a new one */
    if (cached_weather_icon.pixel_buf) {
        free(cached_weather_icon.pixel_buf);
    }
    memset(&cached_weather_icon, 0, sizeof(cached_weather_icon));

    char path[64];
    weather_code_to_bin_path(weather_code, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "[图标] 二进制文件不存在: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* --- Read 4-byte header: width (uint16 LE), height (uint16 LE) --- */
    uint8_t hdr[4];
    size_t nr = fread(hdr, 1, sizeof(hdr), f);
    if (nr != sizeof(hdr)) {
        ESP_LOGE(TAG, "[图标] 读 header 失败: %s", path);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t w = (uint16_t)((uint16_t)hdr[1] << 8 | hdr[0]);
    uint16_t h = (uint16_t)((uint16_t)hdr[3] << 8 | hdr[2]);
    uint32_t pixel_bytes = (uint32_t)w * (uint32_t)h * 3U;
    uint32_t total_bytes = 4U + pixel_bytes;

    /* --- Allocate one contiguous buffer for header + pixels so
     *     dsc.data can simply point at offset 4 --- */
    uint8_t *buf = (uint8_t *)malloc(total_bytes);
    if (!buf) {
        ESP_LOGE(TAG, "[图标] malloc 失败 (%u bytes)", (unsigned)total_bytes);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    buf[0] = hdr[0]; buf[1] = hdr[1];
    buf[2] = hdr[2]; buf[3] = hdr[3];

    nr = fread(buf + 4, 1, pixel_bytes, f);
    fclose(f);
    if (nr != pixel_bytes) {
        ESP_LOGE(TAG, "[图标] 像素数据不完整: %s (got %zu, want %u)",
                 path, nr, (unsigned)pixel_bytes);
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    /* --- Populate lv_img_dsc_t (point into buf + 4) --- */
    cached_weather_icon.pixel_buf = buf;
    cached_weather_icon.dsc.header.always_zero = 0;
    cached_weather_icon.dsc.header.w = w;
    cached_weather_icon.dsc.header.h = h;
    cached_weather_icon.dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    cached_weather_icon.dsc.data_size = pixel_bytes;
    cached_weather_icon.dsc.data = buf + 4;
    cached_weather_icon.code = weather_code;
    cached_weather_icon.valid = true;

    ESP_LOGI(TAG, "[图标] 已加载: %s (%ux%u, cf=TRUE_COLOR_ALPHA, %u bytes)",
             path, (unsigned)w, (unsigned)h, (unsigned)total_bytes);
    return ESP_OK;
}

/* =========================================================================
 * Display initialization
 * ========================================================================= */
esp_err_t display_init(void)
{
    if (is_initialized) {
        ESP_LOGW(TAG, "显示已初始化，跳过");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 GC9A01 显示...");

    /* --- 0. Ensure RST/BL pin is configured as output and held high
     *        before SPI traffic starts. power_init() may have left it
     *        as input pull-down, which can glitch the panel. --- */
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << GPIO_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(GPIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* --- 1. SPI bus --- */
    spi_bus_config_t buscfg = {
        .sclk_io_num = GPIO_SPI_SCK,
        .mosi_io_num = GPIO_SPI_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* --- 2. Panel IO (8-bit SPI) --- */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = GPIO_LCD_DC,
        .cs_gpio_num = GPIO_LCD_CS,
        .pclk_hz = DISPLAY_SPI_FREQ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io_handle));

    /* --- 3. GC9A01 panel --- */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_LCD_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io_handle, &panel_config, &panel_handle));

    /* Perform hardware reset and wait for panel to stabilize */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(10));   /* GC9A01 needs ~5ms after reset */

    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* Backlight on: RST pin is shared with backlight on this module.
     * Must wait after disp_on_off before driving RST high, otherwise
     * the panel may latch into reset mode instead of normal operation. */
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(GPIO_LCD_RST, 1);

    ESP_LOGI(TAG, "Panel 初始化完成 (RST=背光=ON)");

    /* --- 4. LVGL port --- */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 5,
        .task_stack = 4096,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* --- 5. Register display with LVGL --- */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_handle,
        .panel_handle = panel_handle,
        .buffer_size = DISPLAY_WIDTH * 20,   /* ~1/12 screen, single-buffered */
        .double_buffer = false,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .direct_mode = false,
        },
    };
    disp_handle = lvgl_port_add_disp(&disp_cfg);
    if (disp_handle == NULL) {
        ESP_LOGE(TAG, "添加显示到 LVGL 失败");
        lvgl_port_deinit();
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
        esp_lcd_panel_io_del(panel_io_handle);
        panel_io_handle = NULL;
        spi_bus_free(SPI2_HOST);
        return ESP_FAIL;
    }

    /* --- 6. SPIFFS (holds the pre-decoded icon binaries) --- */
    init_spiffs();

    /* --- 7. PicoPixel UI (must hold port lock — LVGL task is running) --- */
    ESP_LOGI(TAG, "初始化 PicoPixel UI...");
    lvgl_port_lock(0);
    ui_init();
    lvgl_port_unlock();

    /* --- 8. Force label colors / fonts to white / simsun_16_cjk.
     * 注意: ui_init() 不再调用 lv_scr_load(badge_main), 因此
     * lv_disp_get_scr_act() 此时返回 NULL, 直接操作已创建的
     * objects.badge_main 对象即可. 真实屏幕的首次加载发生在
     * display_main_screen() 写入真实时间/天气数据之后, 这样
     * 可以彻底避免上电瞬间出现 PicoPixel 原始占位值. --- */
    lvgl_port_lock(0);
    lv_obj_t *main_scr = objects.badge_main;
    if (main_scr) {
        lv_obj_set_style_bg_color(main_scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(main_scr, LV_OPA_COVER, 0);
    }

    /* All labels → white text, opaque (never let theme pick gray/black) */
    lv_obj_t *label_list[] = {
        objects.label_1, objects.label_2, objects.label_3,
        objects.label_4, objects.label_5, objects.label_6,
        objects.label_7, objects.label_time, objects.label_date,
        objects.label_weather, objects.label_temp, objects.label_detail,
        objects.label_update,
    };
    for (size_t i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        lv_obj_t *lbl = label_list[i];
        if (!lbl) continue;
        lv_obj_set_style_text_color(lbl, lv_color_white(),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(lbl, LV_OPA_COVER,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    /* Chinese-capable labels → simsun_16_cjk font */
    lv_obj_t *cn_labels[] = {
        objects.label_1, objects.label_weather, objects.label_update,
    };
    for (size_t i = 0; i < sizeof(cn_labels) / sizeof(cn_labels[0]); i++) {
        if (cn_labels[i]) {
            lv_obj_set_style_text_font(cn_labels[i], &lv_font_simsun_16_cjk,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    /* Time includes Chinese weekday (周一~周日) — simsun */
    if (objects.label_time)
        lv_obj_set_style_text_font(objects.label_time, &lv_font_simsun_16_cjk, 0);
    /* Date / temp / detail — english/numerals — montserrat */
    if (objects.label_date)
        lv_obj_set_style_text_font(objects.label_date, &lv_font_montserrat_18, 0);
    if (objects.label_temp)
        lv_obj_set_style_text_font(objects.label_temp, &lv_font_montserrat_18, 0);
    if (objects.label_detail)
        lv_obj_set_style_text_font(objects.label_detail, &lv_font_montserrat_14, 0);
    lvgl_port_unlock();

    /* 不再在这里强制 lv_refr_now():
     *  - display_init() 阶段屏幕未被 lv_scr_load() 加载, 刷新无意义
     *  - display_main_screen() 写入真实数据后会触发首次 lv_scr_load,
     *    LVGL 会在下次 lv_task_handler 中自动刷新, 上电不会再闪默认值 */

    is_initialized = true;
    ESP_LOGI(TAG, "显示初始化成功 (延迟加载屏幕, 待真实时间/天气写入)");
    return ESP_OK;
}

/* =========================================================================
 * Backlight control (RST pin doubles as backlight power on this module)
 * ========================================================================= */
void display_backlight_on(void)
{
    gpio_set_level(GPIO_LCD_RST, 1);
}

void display_backlight_off(void)
{
    if (is_initialized && disp_handle) {
        lvgl_port_lock(0);
        if (colon_blink_timer) {
            lv_timer_del(colon_blink_timer);
            colon_blink_timer = NULL;
        }
        lv_anim_del(NULL, NULL);
        lvgl_port_unlock();
    }
    gpio_set_level(GPIO_LCD_RST, 0);
}

/* =========================================================================
 * Display de-initialization (strict order to avoid ISR use-after-free)
 * =========================================================================
 *
 * Correct tear-down order:
 *   1. Stop LVGL timers / animations
 *   2. lvgl_port_remove_disp(disp_handle)    — disconnect LVGL from panel
 *   3. lvgl_port_deinit()                    — stop LVGL task
 *   4. esp_lcd_panel_del(panel_handle)       — uninstall panel driver
 *   5. esp_lcd_panel_io_del(panel_io_handle) — de-register from SPI bus
 *   6. delay 30 ms                            — drain in-flight DMA/ISRs
 *   7. spi_bus_free(SPI2_HOST)                — release SPI bus
 *   8. free icon buffer, unregister spiffs
 */
void display_deinit(void)
{
    if (!is_initialized) return;

    ESP_LOGI(TAG, "正在反初始化显示...");

    /* 1 */
    lvgl_port_lock(0);
    if (colon_blink_timer) {
        lv_timer_del(colon_blink_timer);
        colon_blink_timer = NULL;
    }
    lv_anim_del(NULL, NULL);
    lvgl_port_unlock();

    /* 2, 3 */
    if (disp_handle) {
        lvgl_port_remove_disp(disp_handle);
        disp_handle = NULL;
    }
    lvgl_port_deinit();

    /* 4, 5 */
    if (panel_handle) {
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }
    if (panel_io_handle) {
        esp_lcd_panel_io_del(panel_io_handle);
        panel_io_handle = NULL;
    }

    /* 6 */
    vTaskDelay(pdMS_TO_TICKS(30));

    /* 7 */
    spi_bus_free(SPI2_HOST);

    /* 8 */
    if (cached_weather_icon.pixel_buf) {
        free(cached_weather_icon.pixel_buf);
        cached_weather_icon.pixel_buf = NULL;
    }
    memset(&cached_weather_icon, 0, sizeof(cached_weather_icon));
    esp_vfs_spiffs_unregister("spiffs");

    is_initialized = false;
    ESP_LOGI(TAG, "显示反初始化完成");
}

/* =========================================================================
 * Animation callbacks
 * ========================================================================= */
static void icon_float_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_set_y(obj, v);
}

static void icon_sway_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_set_x(obj, v);
}

static void icon_rotate_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_img_set_angle(obj, v);
}

static void label_breathe_anim_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_set_style_text_opa(obj, (lv_opa_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void arc_phase1_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_arc_set_start_angle(obj, 270);
    lv_arc_set_end_angle(obj, 270 + v);
}

static void arc_phase2_cb(void *var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_arc_set_start_angle(obj, v);
    lv_arc_set_end_angle(obj, v + 60);
}

static void arc_phase1_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, obj);
    lv_anim_set_values(&a2, 0, 360);
    lv_anim_set_time(&a2, 3000);
    lv_anim_set_repeat_count(&a2, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a2, arc_phase2_cb);
    lv_anim_start(&a2);
}

/* =========================================================================
 * Loading screen (shown while fetching WiFi / weather data)
 * ========================================================================= */
void display_loading(const char *message)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_anim_del(NULL, NULL);
    loadScreen(SCREEN_ID_BADGE_LOADING);
    lv_obj_t *act = lv_disp_get_scr_act(disp_handle);
    lv_obj_set_style_bg_color(act, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(act, LV_OPA_COVER, 0);

    if (objects.label_1) {
        lv_obj_set_style_text_font(objects.label_1, &lv_font_simsun_16_cjk,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(objects.label_1, lv_color_white(),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(objects.label_1, LV_OPA_COVER,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        if (message) lv_label_set_text(objects.label_1, message);
    }

    /* Two-phase arc: fill 0→360°, then spin a 60° slice forever */
    if (objects.arc_1) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.arc_1);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 3000);
        lv_anim_set_repeat_count(&a, 0);
        lv_anim_set_exec_cb(&a, arc_phase1_cb);
        lv_anim_set_ready_cb(&a, arc_phase1_ready_cb);
        lv_anim_start(&a);
    }
    lvgl_port_unlock();
}

void display_loading_status(const char *status)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    if (objects.label_1) {
        lv_obj_set_style_text_font(objects.label_1, &lv_font_simsun_16_cjk,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(objects.label_1, lv_color_white(),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(objects.label_1, LV_OPA_COVER,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(objects.label_1, status);
    }
    lvgl_port_unlock();
}

/* =========================================================================
 * Hourly forecast display — dynamic widgets on badge_hourly
 * =========================================================================
 *
 * Creates up to 10 forecast items in a 2+3+3+2 grid.
 * 2-col rows: icon → time(left) + temp(right) side-by-side
 * 3-col rows: icon → time(above) + temp(below) */
void display_hourly_forecast(const hourly_forecast_t* forecast)
{
    if (!is_initialized || !disp_handle || !forecast) return;

    lvgl_port_lock(0);
    lv_anim_del(NULL, NULL);

    lv_obj_t *scr = objects.badge_hourly;
    if (!scr) { lvgl_port_unlock(); return; }

    lv_obj_clean(scr);

    int n = forecast->count;
    if (n > 10) n = 10;

    int row_sizes[] = {2, 3, 3, 2};
    int row_y[]     = {8, 63, 118, 173};
    int col2_cx[] = {60, 180};
    int col3_cx[] = {40, 120, 200};

    int item_idx = 0;
    for (int row = 0; row < 4 && item_idx < n; row++) {
        int cols = row_sizes[row];
        int *cx = (cols == 2) ? col2_cx : col3_cx;

        for (int c = 0; c < cols && item_idx < n; c++, item_idx++) {
            const hourly_item_t *h = &forecast->hours[item_idx];
            int col_cx = cx[c];
            int cy = row_y[row];

            // Icon ~20x20 (zoom=110)
            lv_obj_t *icon = lv_img_create(scr);
            if (h->icon > 0) {
                display_prepare_weather_icon(h->icon);
                if (cached_weather_icon.valid) {
                    lv_img_set_src(icon, &cached_weather_icon.dsc);
                    lv_img_set_zoom(icon, 120);
                }
            }
            lv_obj_set_pos(icon, col_cx - 20, cy + 2);

            if (cols == 2) {
                // 2-col: time + temp side-by-side
                lv_obj_t *tl = lv_label_create(scr);
                lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(tl, lv_palette_main(LV_PALETTE_GREY), 0);
                lv_label_set_text(tl, h->fx_time[0] ? h->fx_time : "--:--");
                lv_obj_align(tl, LV_ALIGN_TOP_LEFT, col_cx - 25, cy + 36);

                lv_obj_t *tl_temp = lv_label_create(scr);
                lv_obj_set_style_text_font(tl_temp, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(tl_temp, lv_color_white(), 0);
                char buf[16];
                snprintf(buf, sizeof(buf), "%d°", h->temp);
                lv_label_set_text(tl_temp, buf);
                lv_obj_align(tl_temp, LV_ALIGN_TOP_LEFT, col_cx + 13, cy + 36);
            } else {
                // 3-col: time above, temp below
                lv_obj_t *tl = lv_label_create(scr);
                lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(tl, lv_palette_main(LV_PALETTE_GREY), 0);
                lv_label_set_text(tl, h->fx_time[0] ? h->fx_time : "--:--");
                lv_obj_set_pos(tl, col_cx - 30, cy + 36);

                lv_obj_t *tl_temp = lv_label_create(scr);
                lv_obj_set_style_text_font(tl_temp, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(tl_temp, lv_color_white(), 0);
                char buf[16];
                snprintf(buf, sizeof(buf), "%d°", h->temp);
                lv_label_set_text(tl_temp, buf);
                lv_obj_set_pos(tl_temp, col_cx + 12, cy + 36);
            }
        }
    }

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "小时");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lvgl_port_unlock();
}

/* =========================================================================
 * Daily forecast display — dynamic widgets on badge_daily
 * =========================================================================
 *
 * Creates up to 10 forecast days in a 2+3+3+2 grid.
 * 2-col rows: icon → date(left) + temp(right) side-by-side
 * 3-col rows: icon → date(above) + temp(below) */
void display_daily_forecast(const daily_forecast_t* forecast)
{
    if (!is_initialized || !disp_handle || !forecast) return;

    lvgl_port_lock(0);
    lv_anim_del(NULL, NULL);

    lv_obj_t *scr = objects.badge_daily;
    if (!scr) { lvgl_port_unlock(); return; }

    lv_obj_clean(scr);

    int n = forecast->count;
    if (n > 10) n = 10;

    int row_sizes[] = {2, 3, 3, 2};
    int row_y[]     = {8, 63, 118, 173};
    int col2_cx[] = {60, 180};
    int col3_cx[] = {40, 120, 200};

    int item_idx = 0;
    for (int row = 0; row < 4 && item_idx < n; row++) {
        int cols = row_sizes[row];
        int *cx = (cols == 2) ? col2_cx : col3_cx;

        for (int c = 0; c < cols && item_idx < n; c++, item_idx++) {
            const daily_item_t *d = &forecast->days[item_idx];
            int col_cx = cx[c];
            int cy = row_y[row];

            // Icon ~20x20 (zoom=110)
            lv_obj_t *icon = lv_img_create(scr);
            if (d->icon_day > 0) {
                display_prepare_weather_icon(d->icon_day);
                if (cached_weather_icon.valid) {
                    lv_img_set_src(icon, &cached_weather_icon.dsc);
                    lv_img_set_zoom(icon, 110);
                }
            }
            lv_obj_set_pos(icon, col_cx-50, cy + 2);

            const char *day_label = d->fx_date[0] ? d->fx_date : "--/--";

            if (cols == 2) {
                // 2-col: date + temp side-by-side
                lv_obj_t *dl = lv_label_create(scr);
                lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(dl, lv_palette_main(LV_PALETTE_GREY), 0);
                lv_label_set_text(dl, day_label);
                lv_obj_align(dl, LV_ALIGN_TOP_LEFT, col_cx - 24, cy );

                lv_obj_t *tl_temp = lv_label_create(scr);
                lv_obj_set_style_text_font(tl_temp, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(tl_temp, lv_color_white(), 0);
                char buf[24];
                snprintf(buf, sizeof(buf), "%d/%d", d->temp_min, d->temp_max);
                lv_label_set_text(tl_temp, buf);
                lv_obj_align(tl_temp, LV_ALIGN_TOP_LEFT, col_cx + 10, cy );
            } else {
                // 3-col: date above, temp below
                lv_obj_t *dl = lv_label_create(scr);
                lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(dl, lv_palette_main(LV_PALETTE_GREY), 0);
                lv_label_set_text(dl, day_label);
                lv_obj_set_pos(dl, col_cx - 20, cy + 36);

                lv_obj_t *tl_temp = lv_label_create(scr);
                lv_obj_set_style_text_font(tl_temp, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(tl_temp, lv_color_white(), 0);
                char buf[24];
                snprintf(buf, sizeof(buf), "%d/%d", d->temp_min, d->temp_max);
                lv_label_set_text(tl_temp, buf);
                lv_obj_set_pos(tl_temp, col_cx + 4, cy + 36);
            }
        }
    }

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "十日");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lvgl_port_unlock();
}

/* =========================================================================
 * Config mode screen
 * ========================================================================= */
void display_config_mode(void)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_anim_del(NULL, NULL);
    loadScreen(SCREEN_ID_BADGE_CONFIG);
    lv_obj_t *act = lv_disp_get_scr_act(disp_handle);
    lv_obj_set_style_bg_color(act, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(act, LV_OPA_COVER, 0);
    lvgl_port_unlock();
}

void display_config_success(void)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);
    lv_anim_del(NULL, NULL);
    loadScreen(SCREEN_ID_BADGE_SUCCESS);
    lv_obj_t *act = lv_disp_get_scr_act(disp_handle);
    lv_obj_set_style_bg_color(act, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(act, LV_OPA_COVER, 0);
    lvgl_port_unlock();
}

/* =========================================================================
 * Screensaver — draw big clock covering badge_main, restore on wake
 * =========================================================================
 *
 * CRITICAL DESIGN: We do NOT destroy any existing widgets.  We do NOT
 * switch screens.  We do NOT create new badge_main screens.
 *
 * Instead, we create a full-screen opaque black container on top of ALL
 * existing badge_main children.  The clock + hint text are drawn inside
 * this container.  When the user wakes up, display_main_screen() simply
 * deletes this container — the original badge_main widgets are intact
 * underneath, and all objects.* pointers remain valid.
 *
 * This completely avoids all the previous crashes caused by:
 *   - lv_obj_clean() → dangling objects.* pointers
 *   - create_screen_badge_main() → new screen objects
 *   - lv_scr_load() / lv_disp_load_scr() → LVGL internal animation crashes
 * ========================================================================= */
static lv_obj_t *screensaver_overlay = NULL;

void display_screensaver(int hour, int minute)
{
    if (!is_initialized || !disp_handle) return;

    lvgl_port_lock(0);

    // Kill all pending animations
    lv_anim_del(NULL, NULL);

    // Stop colon blink timer — it accesses objects.label_time
    if (colon_blink_timer) {
        lv_timer_del(colon_blink_timer);
        colon_blink_timer = NULL;
    }

    // Remove old overlay if present (shouldn't happen, but be safe)
    if (screensaver_overlay) {
        lv_obj_del(screensaver_overlay);
        screensaver_overlay = NULL;
    }

    // Create full-screen opaque black container on the display's TOP LAYER.
    //
    // lv_disp_get_layer_top(disp_handle) returns a special top-layer of the
    // display that sits ABOVE all screens (badge_main, badge_loading, etc.)
    // without being part of any screen's widget tree.  This means:
    //   - lv_obj_del(screensaver_overlay) will NOT affect badge_main children
    //   - badge_main widget pointers (objects.label_time etc.) stay valid
    //   - No LVGL style/tree corruption from deleting the overlay
    //   - The overlay appears on top regardless of which screen is active
    //
    // Previously we used lv_obj_create(scr) (child of badge_main), and
    // deleting it from a different active screen corrupted LVGL internals.
    lv_obj_t *layer = lv_disp_get_layer_top(disp_handle);
    screensaver_overlay = lv_obj_create(layer);
    lv_obj_set_pos(screensaver_overlay, 0, 0);
    lv_obj_set_size(screensaver_overlay, 240, 240);
    lv_obj_set_style_bg_color(screensaver_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screensaver_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver_overlay, 0, 0);
    lv_obj_set_style_pad_all(screensaver_overlay, 0, 0);
    lv_obj_clear_flag(screensaver_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Big clock centered
    lv_obj_t *label_clock = lv_label_create(screensaver_overlay);
    lv_obj_set_style_text_font(label_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_clock, lv_color_white(), 0);
    lv_obj_set_style_text_align(label_clock, LV_TEXT_ALIGN_CENTER, 0);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(label_clock, buf);
    lv_obj_center(label_clock);

    // Hint text at the bottom
    lv_obj_t *label_hint = lv_label_create(screensaver_overlay);
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label_hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(label_hint, "Press WAKE");
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lvgl_port_unlock();
}

/* =========================================================================
 * Weather name (English fallback, ~12 chars max for UI labels)
 * ========================================================================= */
/* =========================================================================
 * Weather name lookup — Chinese preferred, English fallback
 * =========================================================================
 *
 * Priority:
 *   1. Chinese name if ALL characters are in lv_font_simsun_16_cjk
 *   2. English name otherwise
 * ========================================================================= */
#include "font_chars.h"

static const char *get_weather_name(int16_t code)
{
    /* Chinese names — checked against font at runtime */
    static const struct { int16_t code; const char *cn; const char *en; } table[] = {
        {100, "晴",       "Sunny"},
        {101, "多云",     "Cloudy"},
        {102, "少云",     "Few Clouds"},
        {103, "晴间多云", "Partly Cloudy"},
        {104, "阴",       "Overcast"},
        {150, "晴",       "Sunny"},
        {151, "多云",     "Cloudy"},
        {152, "少云",     "Few Clouds"},
        {153, "晴间多云", "Partly Cloudy"},
        {300, "阵雨",     "Shower"},
        {301, "强阵雨",   "Heavy Shower"},
        {302, "雷阵雨",   "Thunderstorm"},
        {303, "强雷阵雨", "Heavy Thunderstorm"},
        {304, "冰雹",     "Hail"},
        {305, "小雨",     "Light Rain"},
        {306, "中雨",     "Moderate Rain"},
        {307, "大雨",     "Heavy Rain"},
        {308, "极端降雨", "Extreme Rain"},
        {309, "毛毛雨",   "Drizzle"},
        {310, "暴雨",     "Storm"},
        {311, "大暴雨",   "Heavy Storm"},
        {312, "特大暴雨", "Severe Storm"},
        {313, "冻雨",     "Freezing Rain"},
        {314, "小到中雨", "Light-Moderate Rain"},
        {315, "中到大雨", "Moderate-Heavy Rain"},
        {316, "大到暴雨", "Heavy-Storm Rain"},
        {317, "暴雨到大暴雨", "Storm-Heavy Storm"},
        {318, "大暴雨到特大暴雨", "Heavy-Severe Storm"},
        {350, "阵雨",     "Shower"},
        {351, "强阵雨",   "Heavy Shower"},
        {399, "雨",       "Rain"},
        {400, "小雪",     "Light Snow"},
        {401, "中雪",     "Moderate Snow"},
        {402, "大雪",     "Heavy Snow"},
        {403, "暴雪",     "Blizzard"},
        {404, "雨夹雪",   "Sleet"},
        {405, "雨雪天气", "Rain-Snow Mix"},
        {406, "雨夹雪",   "Rain-Sleet"},
        {407, "阵雪",     "Snow Shower"},
        {408, "中雪",     "Moderate Snow"},
        {409, "大雪",     "Heavy Snow"},
        {410, "暴雪",     "Blizzard"},
        {456, "雨夹雪",   "Rain-Sleet"},
        {457, "阵雪",     "Snow Shower"},
        {499, "雪",       "Snow"},
        {500, "薄雾",     "Mist"},
        {501, "雾",       "Foggy"},
        {502, "霾",       "Haze"},
        {503, "扬沙",     "Sand"},
        {504, "浮尘",     "Dust"},
        {507, "沙尘暴",   "Sandstorm"},
        {508, "强沙尘暴", "Severe Sandstorm"},
        {509, "浓雾",     "Dense Fog"},
        {510, "强浓雾",   "Strong Fog"},
        {511, "中度霾",   "Moderate Haze"},
        {512, "重度霾",   "Heavy Haze"},
        {513, "严重霾",   "Severe Haze"},
        {514, "大雾",     "Heavy Fog"},
        {515, "特强浓雾", "Extra Heavy Fog"},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].code == code) {
            /* Use Chinese only if font supports ALL chars */
            if (font_supports_chinese(table[i].cn)) {
                return table[i].cn;
            }
            return table[i].en;
        }
    }
    return "Unknown";
}

/* =========================================================================
 * Icon rotation correction (some source PNGs are pre-rotated)
 * ========================================================================= */
static int16_t icon_rotation_correction(int16_t code)
{
    if (code == 101) return 0;    /* "多云" PNG needs 90° CW */
    return 0;
}

/* =========================================================================
 * Weekday name (Chinese: 周一 ~ 周日)
 * ========================================================================= */
static const char *wday_name(int wday)
{
    static const char *names[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
    };
    if (wday < 0 || wday > 6) return "未知";
    return names[wday];
}

/* =========================================================================
 * Colon blink timer callback
 * ========================================================================= */
static void colon_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    colon_visible = !colon_visible;
    if (objects.label_time) {
        char buf[48];
        const char *sep = colon_visible ? ":" : " ";
        snprintf(buf, sizeof(buf), "%02d%s%02d  %s",
                 last_hour, sep, last_minute, wday_name(last_wday));
        lv_label_set_text(objects.label_time, buf);
    }
}

/* =========================================================================
 * Main screen — update time / icon / weather / details.
 * =========================================================================
 *
 * Icon loading happens OUTSIDE lvgl_port_lock (pure fread + malloc, no LVGL
 * calls). Inside the lock we only do lv_img_set_src and styling — fast.
 * ========================================================================= */
void display_main_screen(int hour, int minute, int wday,
                         int16_t weather_code, const char *weather_text,
                         int8_t temperature, uint8_t humidity, uint8_t wind_scale,
                         const char *update_time_str)
{
    if (!is_initialized || !disp_handle) return;

    /* --- Pre-load icon OUTSIDE lvgl_port_lock (no LVGL calls here) --- */
    if (!cached_weather_icon.valid || cached_weather_icon.code != weather_code) {
        ESP_LOGI(TAG, "[主界面] 加载天气图标 code=%d", weather_code);
        esp_err_t icon_err = display_prepare_weather_icon(weather_code);
        if (icon_err != ESP_OK) {
            ESP_LOGW(TAG, "[主界面] 图标加载失败: %s (code=%d)",
                     esp_err_to_name(icon_err), weather_code);
            /* Continue without icon — screen will still show time/weather text */
        }
    }

    /* --- Everything LVGL happens INSIDE the port lock --- */
    lvgl_port_lock(0);

    lv_anim_del(NULL, NULL);

    /* Delete screensaver overlay if present.
     * The overlay is a full-screen black container drawn on top of
     * badge_main children.  Deleting it reveals the intact badge_main
     * widgets underneath — all objects.* pointers are still valid. */
    if (screensaver_overlay) {
        lv_obj_del(screensaver_overlay);
        screensaver_overlay = NULL;
    }

    /* Load badge_main screen if not already active (first boot).
     * After screensaver, badge_main is still the active screen (we never
     * switched away), so this is skipped — avoiding the screen switch
     * animation that caused crashes. */
    lv_obj_t *act = lv_disp_get_scr_act(disp_handle);
    if (!act || act != objects.badge_main) {
        loadScreen(SCREEN_ID_BADGE_MAIN);
    }

    /* Time + colon blink timer */
    if (objects.label_time) {
        /* Use simsun font so Chinese weekday (周一~周日) renders correctly */
        lv_obj_set_style_text_font(objects.label_time, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(objects.label_time, lv_color_white(), 0);

        /* Always get time from system clock directly (like screensaver does).
         * Do NOT rely on caller-provided hour/minute because those may be
         * stale or wrong (e.g. after wifi_disconnect resets the clock). */
        time_t now;
        time(&now);
        struct tm *tm_now = localtime(&now);
        last_hour = tm_now->tm_hour;
        last_minute = tm_now->tm_min;
        last_wday = tm_now->tm_wday;

        char buf[48];
        snprintf(buf, sizeof(buf), "%02d:%02d  %s",
                 last_hour, last_minute, wday_name(last_wday));
        lv_label_set_text(objects.label_time, buf);

        if (colon_blink_timer) lv_timer_del(colon_blink_timer);
        colon_blink_timer = lv_timer_create(colon_blink_timer_cb, 500, NULL);
    }

    /* Date — always from system clock */
    if (objects.label_date) {
        lv_obj_set_style_text_font(objects.label_date, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(objects.label_date, lv_color_white(), 0);
        time_t now;
        time(&now);
        struct tm *tm_now = localtime(&now);
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%d.%d.%d",
                 tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday);
        lv_label_set_text(objects.label_date, date_buf);
    }

    /* Weather icon */
    if (objects.qweather_icons) {
        if (cached_weather_icon.valid) {
            lv_img_set_src(objects.qweather_icons, &cached_weather_icon.dsc);

            ESP_LOGI(TAG, "[UI] 图标: 使用缓存 (%ux%u, code=%d)",
                     (unsigned)cached_weather_icon.dsc.header.w,
                     (unsigned)cached_weather_icon.dsc.header.h,
                     weather_code);
        } else {
            ESP_LOGW(TAG, "[UI] 图标缓存无效 (code=%d)", weather_code);
        }

        int16_t correction = icon_rotation_correction(weather_code);
        if (correction != 0) {
            lv_img_set_angle(objects.qweather_icons,
                             (int16_t)(correction + 3600));
        } else {
            lv_img_set_angle(objects.qweather_icons, 0);
        }
    }

    /* Weather description */
    if (objects.label_weather) {
        lv_obj_set_style_text_font(objects.label_weather, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(objects.label_weather, lv_color_white(), 0);
        /* Use API weather_text only if font supports ALL chars;
         * otherwise fall back to get_weather_name() which returns
         * Chinese if supported or English otherwise. */
        if (weather_text && weather_text[0] != '\0' &&
            font_supports_chinese(weather_text)) {
            lv_label_set_text(objects.label_weather, weather_text);
        } else {
            lv_label_set_text(objects.label_weather, get_weather_name(weather_code));
        }
    }

    /* Temperature */
    if (objects.label_temp) {
        lv_obj_set_style_text_color(objects.label_temp, lv_color_white(), 0);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d°C", temperature);
        lv_label_set_text(objects.label_temp, buf);
    }

    /* Humidity / wind */
    if (objects.label_detail) {
        lv_obj_set_style_text_font(objects.label_detail, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(objects.label_detail, lv_color_white(), 0);
        char detail_buf[48];
        snprintf(detail_buf, sizeof(detail_buf), "Hum:%d%% Wind:%d",
                 humidity, wind_scale);
        lv_label_set_text(objects.label_detail, detail_buf);
    }

    /* Update-time label (breathing animation) */
    if (objects.label_update) {
        lv_obj_set_style_text_font(objects.label_update, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(objects.label_update, lv_color_white(), 0);
        lv_label_set_text(objects.label_update, update_time_str);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.label_update);
        lv_anim_set_values(&a, LV_OPA_30, LV_OPA_100);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_playback_time(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, label_breathe_anim_cb);
        lv_anim_start(&a);
    }

    /* --- Icon motion animation (rotate/sway/float depending on code) --- */
    lv_anim_del(objects.qweather_icons, NULL);
    {
        int16_t code = weather_code;
        if (code == 100 || code == 499) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, objects.qweather_icons);
            lv_anim_set_values(&a, 0, 3600);
            lv_anim_set_time(&a, 6000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, icon_rotate_anim_cb);
            lv_anim_start(&a);
        } else if (code == 101 || code == 151 || code == 103 || code == 153 ||
                   (code >= 300 && code <= 318) || code == 399 ||
                   (code >= 400 && code <= 410) || code == 499) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, objects.qweather_icons);
            lv_anim_set_values(&a, 78, 82);
            lv_anim_set_time(&a, 3000);
            lv_anim_set_playback_time(&a, 3000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, icon_sway_anim_cb);
            lv_anim_start(&a);
        } else {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, objects.qweather_icons);
            lv_anim_set_values(&a, 70, 74);
            lv_anim_set_time(&a, 2000);
            lv_anim_set_playback_time(&a, 2000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, icon_float_anim_cb);
            lv_anim_start(&a);
        }
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "主屏幕: %02d:%02d %s %d°C Hum:%d%% Wind:%d",
             hour, minute, get_weather_name(weather_code),
             temperature, humidity, wind_scale);
}
