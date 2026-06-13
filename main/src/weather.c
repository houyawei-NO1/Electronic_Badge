/**
 * @file weather.c
 * @brief Weather API implementation (和风天气)
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "weather.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "miniz.h"

static const char* TAG = "天气";

static bool gzip_decompress(const uint8_t* input, size_t input_len, char* output, size_t* output_len)
{
    if (!input || !output || !output_len || *output_len == 0) {
        ESP_LOGE(TAG,"参数为空");
        return false;
    }

    // 解析gzip头，跳过可变长度头部
    size_t pos = 10; // 最小gzip头长度
    uint8_t flags = input[3];
    
    // 跳过可选字段
    if (flags & 0x04) { // FEXTRA
        uint16_t xlen = input[pos] | (input[pos+1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { // FNAME
        while (pos < input_len && input[pos]) pos++;
        pos++;
    }
    if (flags & 0x10) { // FCOMMENT
        while (pos < input_len && input[pos]) pos++;
        pos++;
    }
    if (flags & 0x02) { // FHCRC
        pos += 2;
    }
    
    const uint8_t *deflate_data = input + pos;
    size_t deflate_len = input_len - pos - 8;  // 去掉尾部8字节(CRC32+ISIZE)
    
    ESP_LOGD(TAG, "gzip头长度=%d, 压缩长度=%d, 输出长度=%d", pos, deflate_len, *output_len);

    tinfl_decompressor* dec = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    if (!dec) {
        ESP_LOGE(TAG, "分配解压器失败");
        return false;
    }
    memset(dec, 0, sizeof(tinfl_decompressor));
    tinfl_init(dec);

    size_t in_rem = deflate_len;
    size_t out_rem = *output_len;
    uint8_t* out_next = (uint8_t*)output;

    // 使用TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF确保输出不循环
    tinfl_status st = tinfl_decompress(
        dec,
        deflate_data, &in_rem,
        (uint8_t*)output, out_next, &out_rem,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
    );

    free(dec);

    if (st < TINFL_STATUS_DONE) {
        ESP_LOGE(TAG, "解压错误: %d", st);
        return false;
    }

    // out_rem 是剩余空间，实际写入字节数 = 总大小 - 剩余空间
    size_t written = *output_len - out_rem;
    *output_len = written;
    output[written] = '\0';
    ESP_LOGD(TAG, "解压了 %d 字节", written);
    return true;
}

// Simple JSON value extraction
static const char* json_get_string(const char* json, const char* key, char* out, size_t out_len)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    
    const char* p = strstr(json, search_key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else {
        // Numeric value
        size_t i = 0;
        while (*p && (*p == '-' || (*p >= '0' && *p <= '9')) && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
    return out;
}

typedef struct {
    char raw_data[4096];
    size_t raw_len;
    bool is_gzip;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    http_response_t* resp_info = (http_response_t*)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP事件错误");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP头: %s: %s", evt->header_key, evt->header_value);
            if (strcasecmp(evt->header_key, "Content-Encoding") == 0 &&
                strcasecmp(evt->header_value, "gzip") == 0) {
                resp_info->is_gzip = true;
                ESP_LOGD(TAG, "响应是gzip压缩的");
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len && resp_info->raw_len + evt->data_len < sizeof(resp_info->raw_data)) {
                memcpy(resp_info->raw_data + resp_info->raw_len, evt->data, evt->data_len);
                resp_info->raw_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Helper: find the matching } for a {, handling nested braces.
// Returns pointer to the matching }, or NULL if not found.
static const char* find_matching_brace(const char* start, const char* end_limit)
{
    if (!start || *start != '{') return NULL;
    int depth = 1;
    const char *p = start + 1;
    while (*p && p < end_limit) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) return p;
        }
        p++;
    }
    return NULL;
}

// Static buffers to avoid stack overflow
static http_response_t s_resp_info;
static char s_response[8192];  // Must be large enough for the biggest JSON (~4400 bytes for 24h hourly)

bool weather_fetch(weather_data_t* data)
{
    if (!data) return false;
    
    // Use static buffers to avoid large stack allocation
    memset(&s_resp_info, 0, sizeof(s_resp_info));
    memset(s_response, 0, sizeof(s_response));
    
    http_response_t* resp_info = &s_resp_info;
    char* response = s_response;
    
    // Build URL using API Host from sdkconfig
    char url[512];
    snprintf(url, sizeof(url), 
             "https://%s/v7/weather/now?key=%s&location=%s",
             WEATHER_API_HOST,
             WEATHER_API_KEY,
             WEATHER_LOCATION);
    
    ESP_LOGI(TAG, "请求URL: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = resp_info,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,  // Skip CN check for development
        .disable_auto_redirect = false,
        .buffer_size = 8192,
        .buffer_size_tx = 1024,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "初始化HTTP客户端失败");
        return false;
    }
    
    // Disable server compression - request uncompressed data
    // Note: Some servers ignore this header and always return gzip
    esp_http_client_set_header(client, "Accept-Encoding", "identity;q=1.0, *;q=0");
    esp_http_client_set_header(client, "User-Agent", "ESP32-Badge/1.0 (gzip not supported)");
    
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP请求失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP状态码: %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }
    
    esp_http_client_cleanup(client);
    
   // Decompress if gzip
if (resp_info->is_gzip) {
    ESP_LOGD(TAG, "解压gzip响应 (%d 字节)...", resp_info->raw_len);
    size_t decompressed_len = sizeof(s_response);
    memset(response, 0, sizeof(s_response));
    if (!gzip_decompress((const uint8_t*)resp_info->raw_data, resp_info->raw_len, response, &decompressed_len)) {
        ESP_LOGE(TAG, "解压响应失败");
        return false;
    }
    ESP_LOGD(TAG, "解压到 %d 字节", decompressed_len);
} else {
    memcpy(response, resp_info->raw_data, resp_info->raw_len);
    response[resp_info->raw_len] = '\0';
}
    
    // If response body is not too long, print it for debugging
    size_t resp_len = strlen(response);
    if (resp_len < 2000) {
        ESP_LOGI(TAG, "响应: %s", response);
    } else {
        ESP_LOGI(TAG, "响应(截断): %.2000s...", response);
    }

    // Check API code
    char value_buf[64];
    if (!json_get_string(response, "code", value_buf, sizeof(value_buf))) {
        ESP_LOGE(TAG, "从响应解析API代码失败");
        return false;
    }
    ESP_LOGD(TAG, "API代码: %s", value_buf);
    if (strcmp(value_buf, "200") != 0) {
        ESP_LOGE(TAG, "API错误代码: %s", value_buf);
        return false;
    }

    // Find "now" object in JSON
    const char* now_start = strstr(response, "\"now\"");
    if (!now_start) {
        ESP_LOGE(TAG, "响应中没有'now'对象");
        return false;
    }

    // Extract data from now object (only fields used by UI)
    if (json_get_string(now_start, "temp", value_buf, sizeof(value_buf))) {
        data->temperature = atoi(value_buf);
    }

    if (json_get_string(now_start, "humidity", value_buf, sizeof(value_buf))) {
        data->humidity = atoi(value_buf);
    }

    if (json_get_string(now_start, "windScale", value_buf, sizeof(value_buf))) {
        data->wind_scale = atoi(value_buf);
    }

    if (json_get_string(now_start, "text", value_buf, sizeof(value_buf))) {
        strncpy(data->weather_text, value_buf, sizeof(data->weather_text) - 1);
    }

    if (json_get_string(now_start, "icon", value_buf, sizeof(value_buf))) {
        data->weather_code = atoi(value_buf);
    }

    // Get update time from API response (obsTime field, ISO 8601 format: "2026-06-09T08:30+08:00")
    // NTP on ESP32-C3 is unreliable/slow, so we use the weather API's server time as authoritative time.
    time_t api_time = 0;
    if (json_get_string(now_start, "obsTime", value_buf, sizeof(value_buf))) {
        // Parse: "YYYY-MM-DDTHH:MM+TZ:00" or "YYYY-MM-DDTHH:MM:SS+TZ:00"
        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
        int tz_hour = 8;  // default to +08:00
        int parsed = sscanf(value_buf, "%4d-%2d-%2dT%2d:%2d:%2d+%2d:",
                            &year, &month, &day, &hour, &min, &sec, &tz_hour);
        if (parsed < 5) {
            sec = 0;
            parsed = sscanf(value_buf, "%4d-%2d-%2dT%2d:%2d+%2d:",
                            &year, &month, &day, &hour, &min, &tz_hour);
        }
        if (parsed >= 5 && year > 2000) {
            // Convert to UTC: obsTime is local time; subtract tz offset to get UTC
            // Then compute Unix epoch using UTC via mktime with TZ=UTC0
            struct tm t = {0};
            t.tm_year = year - 1900;
            t.tm_mon  = month - 1;
            t.tm_mday = day;
            t.tm_hour = hour;
            t.tm_min  = min;
            t.tm_sec  = sec;
            // Normalize to UTC: obsTime in +tz_hour means UTC is tz_hour earlier
            t.tm_hour -= tz_hour;
            // Use UTC timezone for epoch computation, then restore
            char old_tz[64] = "";
            char* p_old_tz = getenv("TZ");
            if (p_old_tz) strncpy(old_tz, p_old_tz, sizeof(old_tz) - 1);
            setenv("TZ", "UTC0", 1);
            tzset();
            time_t raw_epoch = mktime(&t);
            // Restore original TZ (usually empty → will fall back to UTC)
            if (strlen(old_tz) > 0) {
                setenv("TZ", old_tz, 1);
            } else {
                unsetenv("TZ");
            }
            tzset();
            if (raw_epoch > (time_t)1577836800) {  // > 2020-01-01 = valid
                api_time = raw_epoch;
                ESP_LOGI(TAG, "API时间: %s -> UTC epoch=%lu", value_buf, (unsigned long)api_time);
            }
        }
    }

    // Use API time if available, otherwise fall back to system time
    if (api_time > 0) {
        data->update_time = (uint32_t)api_time;
    } else {
        time_t now_time;
        time(&now_time);
        data->update_time = (uint32_t)now_time;
    }

    ESP_LOGI(TAG, "天气: %s, %d°C, 湿度%d%%, 风力%d级",
             data->weather_text, data->temperature,
             data->humidity, data->wind_scale);

    return true;
}

/* =========================================================================
 * Hourly forecast (24h endpoint)
 * ========================================================================= */
bool weather_fetch_hourly(hourly_forecast_t* forecast)
{
    if (!forecast) return false;

    memset(&s_resp_info, 0, sizeof(s_resp_info));
    memset(s_response, 0, sizeof(s_response));
    forecast->count = 0;

    char url[512];
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/24h?key=%s&location=%s",
             WEATHER_API_HOST, WEATHER_API_KEY, WEATHER_LOCATION);
    ESP_LOGI(TAG, "小时预报请求: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &s_resp_info,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
        .disable_auto_redirect = false,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "小时预报: 初始化HTTP客户端失败"); return false; }

    esp_http_client_set_header(client, "Accept-Encoding", "identity;q=1.0, *;q=0");
    esp_http_client_set_header(client, "User-Agent", "ESP32-Badge/1.0");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) { ESP_LOGE(TAG, "小时预报: HTTP请求失败: %s", esp_err_to_name(err)); esp_http_client_cleanup(client); return false; }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) { ESP_LOGE(TAG, "小时预报: HTTP状态码: %d", status_code); esp_http_client_cleanup(client); return false; }
    esp_http_client_cleanup(client);

    // Decompress if gzip
    if (s_resp_info.is_gzip) {
        size_t decompressed_len = sizeof(s_response);
        memset(s_response, 0, sizeof(s_response));
        if (!gzip_decompress((const uint8_t*)s_resp_info.raw_data, s_resp_info.raw_len, s_response, &decompressed_len))
            { ESP_LOGE(TAG, "小时预报: 解压失败"); return false; }
    } else {
        memcpy(s_response, s_resp_info.raw_data, s_resp_info.raw_len);
        s_response[s_resp_info.raw_len] = '\0';
    }

    // Check API code
    char vb[64];
    if (!json_get_string(s_response, "code", vb, sizeof(vb)) || strcmp(vb, "200") != 0)
        { ESP_LOGE(TAG, "小时预报: API错误 code=%s", vb); return false; }

    // Find "hourly" array
    const char *arr = strstr(s_response, "\"hourly\"");
    if (!arr) { ESP_LOGE(TAG, "小时预报: 没有hourly数组"); return false; }

    // Parse hourly items using brace-counting.
    const char *end_limit = s_response + strlen(s_response);
    int count = 0;
    const char *p = strstr(arr, "{\"fxTime\"");
    if (!p) { ESP_LOGE(TAG, "小时预报: 找不到{fxTime"); return false; }

    while (count < HOURLY_MAX && p) {
        hourly_item_t *item = &forecast->hours[count];

        char iso_time[32];
        if (json_get_string(p, "fxTime", iso_time, sizeof(iso_time))) {
            char *t = strchr(iso_time, 'T');
            if (t) {
                strncpy(item->fx_time, t + 1, 5);
                item->fx_time[5] = '\0';
            } else {
                strncpy(item->fx_time, iso_time, 5);
                item->fx_time[5] = '\0';
            }
        }
        if (json_get_string(p, "temp", vb, sizeof(vb))) item->temp = atoi(vb);
        if (json_get_string(p, "icon", vb, sizeof(vb))) item->icon = atoi(vb);

        count++;

        // Find matching } via brace counting
        const char *obj_end = find_matching_brace(p, end_limit);
        if (!obj_end) { ESP_LOGI(TAG, "小时: 第%d项后无匹配}}", count); break; }
        p = strstr(obj_end + 1, "{\"fxTime\"");
    }
    forecast->count = count;

    ESP_LOGI(TAG, "小时预报: 获取了 %d 个小时数据", count);
    for (int i = 0; i < count && i < 10; i++) {
        ESP_LOGI(TAG, "  [%d] %s %d°C icon=%d", i,
                 forecast->hours[i].fx_time, forecast->hours[i].temp,
                 forecast->hours[i].icon);
    }
    return count > 0;
}

/* =========================================================================
 * Daily forecast (7d endpoint)
 * ========================================================================= */
bool weather_fetch_daily(daily_forecast_t* forecast)
{
    if (!forecast) return false;

    memset(&s_resp_info, 0, sizeof(s_resp_info));
    memset(s_response, 0, sizeof(s_response));
    forecast->count = 0;

    char url[512];
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/10d?key=%s&location=%s",
             WEATHER_API_HOST, WEATHER_API_KEY, WEATHER_LOCATION);
    ESP_LOGI(TAG, "每日预报请求: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &s_resp_info,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
        .disable_auto_redirect = false,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "每日预报: 初始化HTTP客户端失败"); return false; }

    esp_http_client_set_header(client, "Accept-Encoding", "identity;q=1.0, *;q=0");
    esp_http_client_set_header(client, "User-Agent", "ESP32-Badge/1.0");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) { ESP_LOGE(TAG, "每日预报: HTTP请求失败: %s", esp_err_to_name(err)); esp_http_client_cleanup(client); return false; }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) { ESP_LOGE(TAG, "每日预报: HTTP状态码: %d", status_code); esp_http_client_cleanup(client); return false; }
    esp_http_client_cleanup(client);

    if (s_resp_info.is_gzip) {
        size_t decompressed_len = sizeof(s_response);
        memset(s_response, 0, sizeof(s_response));
        if (!gzip_decompress((const uint8_t*)s_resp_info.raw_data, s_resp_info.raw_len, s_response, &decompressed_len))
            { ESP_LOGE(TAG, "每日预报: 解压失败"); return false; }
    } else {
        memcpy(s_response, s_resp_info.raw_data, s_resp_info.raw_len);
        s_response[s_resp_info.raw_len] = '\0';
    }

    ESP_LOGI(TAG, "每日预报响应长度 %zu, 前400: %.400s", strlen(s_response), s_response);

    char vb[64];
    if (!json_get_string(s_response, "code", vb, sizeof(vb)) || strcmp(vb, "200") != 0)
        { ESP_LOGE(TAG, "每日预报: API错误 code=%s", vb); return false; }

    const char *arr = strstr(s_response, "\"daily\"");
    if (!arr) { ESP_LOGE(TAG, "每日预报: 没有daily数组"); return false; }

    // Parse daily items using brace-counting to find object boundaries.
    const char *end_limit = s_response + strlen(s_response);
    int count = 0;
    const char *p = strstr(arr, "{\"fxDate\"");
    if (!p) { ESP_LOGE(TAG, "每日预报: 找不到{fxDate"); return false; }

    while (count < DAILY_MAX && p) {
        daily_item_t *item = &forecast->days[count];

        char iso_date[16];
        if (json_get_string(p, "fxDate", iso_date, sizeof(iso_date))) {
            char *d1 = strchr(iso_date, '-');
            if (d1) {
                d1++;
                char *d2 = strchr(d1, '-');
                if (d2)
                    snprintf(item->fx_date, 6, "%.2s/%.2s", d1, d2 + 1);
                else {
                    strncpy(item->fx_date, d1, 5);
                    item->fx_date[5] = '\0';
                }
            } else {
                strncpy(item->fx_date, iso_date, 5);
                item->fx_date[5] = '\0';
            }
        }
        if (json_get_string(p, "tempMax", vb, sizeof(vb))) item->temp_max = atoi(vb);
        if (json_get_string(p, "tempMin", vb, sizeof(vb))) item->temp_min = atoi(vb);
        if (json_get_string(p, "iconDay", vb, sizeof(vb))) item->icon_day = atoi(vb);

        count++;

        // Find matching } via brace counting
        const char *obj_end = find_matching_brace(p, end_limit);
        if (!obj_end) { ESP_LOGI(TAG, "每日: 第%d项后无匹配}}", count); break; }
        p = strstr(obj_end + 1, "{\"fxDate\"");
    }
    forecast->count = count;

    ESP_LOGI(TAG, "每日预报: 获取了 %d 天数据", count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  [%d] %s %d~%d°C", i,
                 forecast->days[i].fx_date,
                 forecast->days[i].temp_min,
                 forecast->days[i].temp_max);
    }
    return count > 0;
}

/* =========================================================================
 * NVS persistence for weather data
 * ========================================================================= */
#define WEATHER_NVS_NAMESPACE  "weather"
#define WEATHER_NVS_KEY        "last_data"

bool weather_save_to_nvs(const weather_data_t* data)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS打开失败: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(nvs_handle, WEATHER_NVS_KEY, data, sizeof(weather_data_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS写入天气数据失败: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS提交失败: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "天气数据已保存到NVS");
    return true;
}

bool weather_load_from_nvs(weather_data_t* data)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS无天气数据 (namespace不存在)");
        return false;
    }

    size_t size = sizeof(weather_data_t);
    err = nvs_get_blob(nvs_handle, WEATHER_NVS_KEY, data, &size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS读取天气数据失败: %s", esp_err_to_name(err));
        return false;
    }

    if (size != sizeof(weather_data_t)) {
        ESP_LOGW(TAG, "NVS天气数据大小不匹配: %d vs %d", size, sizeof(weather_data_t));
        return false;
    }

    /* Validate: update_time must be reasonable (> 2020-01-01) */
    if (data->update_time < 1577836800UL) {
        ESP_LOGW(TAG, "NVS天气数据时间戳无效");
        return false;
    }

    ESP_LOGI(TAG, "从NVS加载天气数据: %s %d°C (更新于%lds前)",
             data->weather_text, data->temperature,
             (long)(time(NULL) - (time_t)data->update_time));
    return true;
}

/* =========================================================================
 * NVS persistence for hourly forecast
 * ========================================================================= */
bool weather_save_hourly_to_nvs(const hourly_forecast_t* data)
{
    if (!data) return false;
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "hourly_fc", data, sizeof(hourly_forecast_t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "小时预报NVS保存失败: %s", esp_err_to_name(err)); return false; }
    ESP_LOGI(TAG, "小时预报已保存到NVS (%d 小时)", data->count);
    return true;
}

bool weather_load_hourly_from_nvs(hourly_forecast_t* data)
{
    if (!data) return false;
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READONLY, &h) != ESP_OK) return false;
    size_t size = sizeof(hourly_forecast_t);
    esp_err_t err = nvs_get_blob(h, "hourly_fc", data, &size);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (size != sizeof(hourly_forecast_t)) return false;
    if (data->count <= 0 || data->count > HOURLY_MAX) return false;
    ESP_LOGI(TAG, "从NVS加载小时预报: %d 小时", data->count);
    return true;
}

/* =========================================================================
 * NVS persistence for daily forecast
 * ========================================================================= */
bool weather_save_daily_to_nvs(const daily_forecast_t* data)
{
    if (!data) return false;
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "daily_fc", data, sizeof(daily_forecast_t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "每日预报NVS保存失败: %s", esp_err_to_name(err)); return false; }
    ESP_LOGI(TAG, "每日预报已保存到NVS (%d 天)", data->count);
    return true;
}

bool weather_load_daily_from_nvs(daily_forecast_t* data)
{
    if (!data) return false;
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READONLY, &h) != ESP_OK) return false;
    size_t size = sizeof(daily_forecast_t);
    esp_err_t err = nvs_get_blob(h, "daily_fc", data, &size);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (size != sizeof(daily_forecast_t)) return false;
    if (data->count <= 0 || data->count > DAILY_MAX) return false;
    ESP_LOGI(TAG, "从NVS加载每日预报: %d 天", data->count);
    return true;
}
