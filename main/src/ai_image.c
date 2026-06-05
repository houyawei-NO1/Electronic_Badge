/**
 * @file ai_image.c
 * @brief AI image generation using Pollinations.AI
 */
#include <string.h>
#include <time.h>
#include "ai_image.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/task.h"

static const char* TAG = "AIImage";
static const char* IMAGE_PATH = "/spiffs/ai_weather.jpg";
static bool s_spiffs_initialized = false;

// Holiday detection (simplified - add more as needed)
typedef struct {
    const char* name;
    uint8_t month;
    uint8_t day;
} holiday_t;

static const holiday_t holidays[] = {
    { "Chinese New Year", 1, 1 },     // Simplified, varies by year
    { "Lantern Festival", 1, 15 },
    { "Valentine's Day", 2, 14 },
    { "Qingming Festival", 4, 5 },    // Approximate
    { "Labor Day", 5, 1 },
    { "Dragon Boat Festival", 5, 5 }, // Approximate
    { "Mid-Autumn Festival", 8, 15 }, // Approximate
    { "National Day", 10, 1 },
    { "Halloween", 10, 31 },
    { "Christmas", 12, 25 },
    { "New Year's Day", 1, 1 },
};

static const int holiday_count = sizeof(holidays) / sizeof(holidays[0]);

static bool ai_image_ensure_spiffs(void)
{
    if (s_spiffs_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return false;
    }

    // Check SPIFFS info
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%d, used=%d, free=%d", total, used, total - used);
    }

    s_spiffs_initialized = true;
    ESP_LOGI(TAG, "SPIFFS initialized successfully");
    return true;
}

void ai_image_init(void)
{
    ai_image_ensure_spiffs();
    ESP_LOGI(TAG, "AI image module initialized");
}

const char* ai_image_get_path(void)
{
    return IMAGE_PATH;
}

bool ai_image_exists(void)
{
    if (!ai_image_ensure_spiffs()) {
        return false;
    }
    FILE* f = fopen(IMAGE_PATH, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

void ai_image_delete(void)
{
    if (ai_image_ensure_spiffs()) {
        unlink(IMAGE_PATH);
    }
}

const char* ai_get_current_festival(void)
{
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);

    for (int i = 0; i < holiday_count; i++) {
        if (tm_info->tm_mon + 1 == holidays[i].month &&
            tm_info->tm_mday == holidays[i].day) {
            return holidays[i].name;
        }
    }

    // Check for seasonal themes
    if (tm_info->tm_mon + 1 >= 6 && tm_info->tm_mon + 1 <= 8) {
        return "Summer Vacation";
    }
    if (tm_info->tm_mon + 1 >= 12 || tm_info->tm_mon + 1 <= 2) {
        return "Winter";
    }
    if (tm_info->tm_mon + 1 >= 3 && tm_info->tm_mon + 1 <= 5) {
        return "Spring";
    }
    if (tm_info->tm_mon + 1 >= 9 && tm_info->tm_mon + 1 <= 11) {
        return "Autumn";
    }

    return NULL;
}

void ai_build_prompt(const char* weather_text, int temperature,
                    const char* festival, char* buffer, size_t buffer_size)
{
    // Build English prompt for Pollinations.AI
    // The service works best with English prompts

    const char* season = "";
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);

    if (tm_info->tm_mon + 1 >= 6 && tm_info->tm_mon + 1 <= 8) {
        season = "summer";
    } else if (tm_info->tm_mon + 1 >= 12 || tm_info->tm_mon + 1 <= 2) {
        season = "winter";
    } else if (tm_info->tm_mon + 1 >= 3 && tm_info->tm_mon + 1 <= 5) {
        season = "spring";
    } else {
        season = "autumn";
    }

    // Map weather to English
    const char* weather_en = "sunny day";
    if (strstr(weather_text, "\u96e8") || strstr(weather_text, "rain") ||
        strstr(weather_text, "Rain")) {
        weather_en = "rainy day with raindrops";
    } else if (strstr(weather_text, "\u4e91") || strstr(weather_text, "cloud") ||
               strstr(weather_text, "Cloud")) {
        weather_en = "cloudy sky with soft clouds";
    } else if (strstr(weather_text, "\u96ea") || strstr(weather_text, "snow")) {
        weather_en = "snowy winter scene with snowflakes";
    } else if (strstr(weather_text, "\u96f7") || strstr(weather_text, "thunder")) {
        weather_en = "thunderstorm with lightning";
    } else if (strstr(weather_text, "\u96fe") || strstr(weather_text, "fog")) {
        weather_en = "misty morning with fog";
    }

    // Build prompt
    if (festival && strlen(festival) > 0) {
        snprintf(buffer, buffer_size,
                "Badge design, circular frame, %s, %s, %s festival decorations, "
                "cute cartoon style, soft colors, Chinese traditional elements, "
                "high quality, 240x240",
                weather_en, season, festival);
    } else {
        snprintf(buffer, buffer_size,
                "Badge design, circular frame, %s, beautiful %s landscape, "
                "cute cartoon characters, soft pastel colors, peaceful atmosphere, "
                "high quality, 240x240",
                weather_en, season);
    }

    ESP_LOGI(TAG, "Prompt: %s", buffer);
}

// Static buffers to avoid stack overflow
static char s_ai_prompt[512];
static char s_encoded_prompt[1024];
static char s_ai_url[2048];
static char s_ai_buffer[4096];

/**
 * @brief URL encode a string for use in URL path
 * @param input Input string
 * @param output Output buffer
 * @param output_size Output buffer size
 * @return Number of characters written
 *
 * Note: This encodes spaces as %20 for URL path usage.
 * Pollinations.AI prompt is in the URL path, not query parameters.
 */
static int url_encode_path(const char* input, char* output, size_t output_size)
{
    int enc_idx = 0;
    for (int i = 0; input[i] && enc_idx < (int)output_size - 4; i++) {
        char c = input[i];
        // In URL path, encode all non-unreserved characters
        // Unreserved: A-Z, a-z, 0-9, -, _, ., ~
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            output[enc_idx++] = c;
        } else if (c == ' ') {
            // Space in URL path should be %20, not +
            output[enc_idx++] = '%';
            output[enc_idx++] = '2';
            output[enc_idx++] = '0';
        } else {
            snprintf(&output[enc_idx], 4, "%%%02X", (unsigned char)c);
            enc_idx += 3;
        }
    }
    output[enc_idx] = '\0';
    return enc_idx;
}

bool ai_image_generate(const char* weather_text, int temperature, const char* festival)
{
    // Ensure SPIFFS is initialized
    if (!ai_image_ensure_spiffs()) {
        ESP_LOGE(TAG, "SPIFFS not available, cannot save image");
        return false;
    }

    ai_build_prompt(weather_text, temperature, festival, s_ai_prompt, sizeof(s_ai_prompt));

    // URL encode the prompt for URL path
    memset(s_encoded_prompt, 0, sizeof(s_encoded_prompt));
    int encoded_len = url_encode_path(s_ai_prompt, s_encoded_prompt, sizeof(s_encoded_prompt));
    ESP_LOGI(TAG, "Encoded prompt length: %d", encoded_len);

    // Build image URL
    snprintf(s_ai_url, sizeof(s_ai_url),
            "https://image.pollinations.ai/prompt/%s?width=%d&height=%d&nologo=true",
            s_encoded_prompt, AI_IMAGE_WIDTH, AI_IMAGE_HEIGHT);

    ESP_LOGI(TAG, "Generating image from: %s", s_ai_url);

    // Open output file
    FILE* f = fopen(IMAGE_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", IMAGE_PATH);
        // Try to get more info about the error
        struct stat st;
        if (stat("/spiffs", &st) != 0) {
            ESP_LOGE(TAG, "SPIFFS directory does not exist");
        }
        return false;
    }
    ESP_LOGI(TAG, "Opened file for writing: %s", IMAGE_PATH);

    // Download image
    esp_http_client_config_t config = {
        .url = s_ai_url,
        .timeout_ms = AI_IMAGE_TIMEOUT_MS,
        .event_handler = NULL,
        .user_data = NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        fclose(f);
        return false;
    }

    ESP_LOGI(TAG, "HTTP client initialized, connecting...");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        return false;
    }

    ESP_LOGI(TAG, "Connection opened, fetching headers...");

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_cleanup(client);
        fclose(f);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d, Content-Length: %d", status_code, content_length);

    // Check for rate limiting or errors
    if (status_code == 429) {
        ESP_LOGW(TAG, "Rate limited by Pollinations.AI (429 Too Many Requests)");
        esp_http_client_cleanup(client);
        fclose(f);
        unlink(IMAGE_PATH);
        return false;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "Unexpected HTTP status: %d", status_code);
        // Try to read error response
        int error_read = esp_http_client_read(client, s_ai_buffer, sizeof(s_ai_buffer) - 1);
        if (error_read > 0) {
            s_ai_buffer[error_read] = '\0';
            ESP_LOGW(TAG, "Error response: %s", s_ai_buffer);
        }
        esp_http_client_cleanup(client);
        fclose(f);
        unlink(IMAGE_PATH);
        return false;
    }

    // Read and write data
    int total_read = 0;
    bool first_chunk = true;
    bool is_valid_jpeg = false;

    ESP_LOGI(TAG, "Downloading image data...");

    while (1) {
        int read_len = esp_http_client_read(client, s_ai_buffer, sizeof(s_ai_buffer));
        if (read_len <= 0) break;

        fwrite(s_ai_buffer, 1, read_len, f);
        total_read += read_len;

        // Check for JPEG magic bytes on first read
        if (first_chunk && read_len >= 3) {
            first_chunk = false;
            is_valid_jpeg = ((unsigned char)s_ai_buffer[0] == 0xFF &&
                            (unsigned char)s_ai_buffer[1] == 0xD8 &&
                            (unsigned char)s_ai_buffer[2] == 0xFF);
            if (!is_valid_jpeg) {
                ESP_LOGW(TAG, "Response does not appear to be JPEG (magic: %02X %02X %02X)",
                        (unsigned char)s_ai_buffer[0],
                        (unsigned char)s_ai_buffer[1],
                        (unsigned char)s_ai_buffer[2]);
            } else {
                ESP_LOGI(TAG, "Valid JPEG header detected");
            }
        }
    }

    esp_http_client_cleanup(client);
    fclose(f);

    ESP_LOGI(TAG, "Downloaded %d bytes", total_read);

    // Verify it's a valid image
    if (total_read < 1000) {
        ESP_LOGE(TAG, "Image too small (%d bytes), likely an error response", total_read);
        unlink(IMAGE_PATH);
        return false;
    }

    if (!is_valid_jpeg) {
        ESP_LOGW(TAG, "Downloaded data is not a valid JPEG");
        unlink(IMAGE_PATH);
        return false;
    }

    ESP_LOGI(TAG, "AI image saved to: %s (%d bytes)", IMAGE_PATH, total_read);
    return true;
}
