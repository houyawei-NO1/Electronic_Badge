/**
 * @file weather.c
 * @brief Weather API implementation (和风天气)
 */
#include <string.h>
#include <time.h>
#include "weather.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "miniz.h"

static const char* TAG = "Weather";

static bool gzip_decompress(const uint8_t* input, size_t input_len, char* output, size_t* output_len)
{
    if (!input || !output || !output_len || *output_len == 0) {
        ESP_LOGE(TAG,"null param");
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
    
    ESP_LOGD(TAG, "gzip header len=%d, deflate_len=%d, output_len=%d", pos, deflate_len, *output_len);

    tinfl_decompressor* dec = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    if (!dec) {
        ESP_LOGE(TAG, "failed to alloc decompressor");
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
        ESP_LOGE(TAG, "decompress err: %d", st);
        return false;
    }

    // out_rem 是剩余空间，实际写入字节数 = 总大小 - 剩余空间
    size_t written = *output_len - out_rem;
    *output_len = written;
    output[written] = '\0';
    ESP_LOGD(TAG, "decompressed %d bytes", written);
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
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "Header: %s: %s", evt->header_key, evt->header_value);
            if (strcasecmp(evt->header_key, "Content-Encoding") == 0 &&
                strcasecmp(evt->header_value, "gzip") == 0) {
                resp_info->is_gzip = true;
                ESP_LOGD(TAG, "Response is gzip compressed");
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

// Static buffers to avoid stack overflow
static http_response_t s_resp_info;
static char s_response[4096];

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
    
    ESP_LOGD(TAG, "Fetching weather from: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = resp_info,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,  // Skip CN check for development
        .disable_auto_redirect = false,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }
    
    // Disable server compression - request uncompressed data
    // Note: Some servers ignore this header and always return gzip
    esp_http_client_set_header(client, "Accept-Encoding", "identity;q=1.0, *;q=0");
    esp_http_client_set_header(client, "User-Agent", "ESP32-Badge/1.0 (gzip not supported)");
    
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP status code: %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }
    
    esp_http_client_cleanup(client);
    
   // Decompress if gzip
if (resp_info->is_gzip) {
    ESP_LOGD(TAG, "Decompressing gzip response (%d bytes)...", resp_info->raw_len);
    size_t decompressed_len = sizeof(s_response);
    memset(response, 0, sizeof(s_response));
    if (!gzip_decompress((const uint8_t*)resp_info->raw_data, resp_info->raw_len, response, &decompressed_len)) {
        ESP_LOGE(TAG, "Failed to decompress response");
        return false;
    }
    ESP_LOGD(TAG, "Decompressed to %d bytes", decompressed_len);
} else {
    memcpy(response, resp_info->raw_data, resp_info->raw_len);
    response[resp_info->raw_len] = '\0';
}
    
    // Check API code
    char value_buf[64];
    if (!json_get_string(response, "code", value_buf, sizeof(value_buf))) {
        ESP_LOGE(TAG, "Failed to parse API code from response");
        return false;
    }
    ESP_LOGD(TAG, "API code: %s", value_buf);
    if (strcmp(value_buf, "200") != 0) {
        ESP_LOGE(TAG, "API error code: %s", value_buf);
        return false;
    }
    
    // Find "now" object in JSON
    const char* now_start = strstr(response, "\"now\"");
    if (!now_start) {
        ESP_LOGE(TAG, "No 'now' object in response");
        return false;
    }
    
    // Extract data from now object
    if (json_get_string(now_start, "temp", value_buf, sizeof(value_buf))) {
        data->temperature = atoi(value_buf);
    }
    
    if (json_get_string(now_start, "feelsLike", value_buf, sizeof(value_buf))) {
        data->feels_like = atoi(value_buf);
    }
    
    if (json_get_string(now_start, "humidity", value_buf, sizeof(value_buf))) {
        data->humidity = atoi(value_buf);
    }
    
    if (json_get_string(now_start, "windSpeed", value_buf, sizeof(value_buf))) {
        data->wind_speed = atoi(value_buf);
    }
    
    if (json_get_string(now_start, "text", value_buf, sizeof(value_buf))) {
        strncpy(data->weather_text, value_buf, sizeof(data->weather_text) - 1);
    }
    
    if (json_get_string(now_start, "windDir", value_buf, sizeof(value_buf))) {
        strncpy(data->wind_dir, value_buf, sizeof(data->wind_dir) - 1);
    }
    
    if (json_get_string(now_start, "icon", value_buf, sizeof(value_buf))) {
        data->weather_code = atoi(value_buf);
    }
    
    // Get update time
    time_t now_time;
    time(&now_time);
    data->update_time = (uint32_t)now_time;
    
    ESP_LOGI(TAG, "Weather: %s, %dC, Humidity: %d%%",
             data->weather_text, data->temperature, data->humidity);
    
    return true;
}

const char* weather_get_text(int16_t code)
{
    // 和风天气天气代码转文本
    // https://dev.qweather.com/docs/resource/icons/
    switch (code) {
        case 100: return "Sunny";
        case 101: return "Cloudy";
        case 102: return "Few Clouds";
        case 103: return "Partly Cloudy";
        case 104: return "Overcast";
        case 200: return "Windy";
        case 201: return "Calm";
        case 202: return "Breezy";
        case 300: return "Light Rain";
        case 301: return "Moderate Rain";
        case 302: return "Heavy Rain";
        case 303: return "Shower";
        case 304: return "Thunderstorm";
        case 305: return "Light Rain";
        case 306: return "Moderate Rain";
        case 307: return "Heavy Rain";
        case 308: return "Heavy Rain";
        case 309: return "Light Rain";
        case 310: return "Heavy Rain";
        case 311: return "Rain";
        case 312: return "Heavy Rain";
        case 313: return "Shower";
        case 400: return "Light Snow";
        case 401: return "Moderate Snow";
        case 402: return "Heavy Snow";
        case 403: return "Snow";
        case 404: return "Snow";
        case 405: return "Heavy Snow";
        case 406: return "Snow";
        case 407: return "Snow";
        case 408: return "Heavy Snow";
        case 409: return "Light Snow";
        case 410: return "Heavy Snow";
        case 500: return "Mist";
        case 501: return "Fog";
        case 502: return "Dense Fog";
        case 503: return "Very Dense Fog";
        case 504: return "Fog";
        case 507: return "Dust";
        case 508: return "Sand";
        case 509: return "Haze";
        case 510: return "Strong Haze";
        case 511: return "Moderate Haze";
        case 512: return "Light Haze";
        case 513: return "Severe Haze";
        case 514: return "Moderate Haze";
        case 515: return "Heavy Haze";
        default:
            if (code >= 100 && code < 200) return "Clear";
            if (code >= 300 && code < 400) return "Rain";
            if (code >= 400 && code < 500) return "Snow";
            return "Unknown";
    }
}

int weather_code_to_type(int16_t code)
{
    // Convert weather code to display type
    if (code >= 100 && code < 200) {
        if (code == 100) return 0;  // Sunny
        return 1;  // Cloudy
    }
    if (code >= 300 && code < 400) {
        if (code == 304 || code >= 306) return 4;  // Thunder
        return 2;  // Rainy
    }
    if (code >= 400 && code < 500) {
        return 3;  // Snowy
    }
    if (code >= 500 && code < 520) {
        return 5;  // Foggy
    }
    if (code >= 520 && code < 600) {
        return 5;  // Haze
    }
    return 0;  // Default to sunny
}

void weather_init(void)
{
    ESP_LOGI(TAG, "Weather module initialized");
}
