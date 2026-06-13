/**
 * @file wifi_manager.c
 * @brief WiFi management implementation
 */
#include <string.h>
#include <stdlib.h>
#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_smartconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static wifi_status_t s_wifi_status = WIFI_STATUS_IDLE;
static bool s_smartconfig_active = false;
static bool s_wifi_initialized = false;
static EventGroupHandle_t s_wifi_event_group;
static const int CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static int s_retry_num = 0;
static const char *TAG_WIFI = "WiFi";

// Default configs stored in flash
static wifi_manager_config_t s_configs[WIFI_MAX_CONFIGS];

void wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    memset(s_configs, 0, sizeof(s_configs));
    
    // Load saved configs from NVS
    wifi_load_configs(s_configs, WIFI_MAX_CONFIGS);
}

bool wifi_connect(void)
{
    if (s_wifi_status == WIFI_STATUS_CONNECTED) {
        return true;
    }
    
    s_wifi_status = WIFI_STATUS_CONNECTING;
    
    // Initialize TCP/IP and WiFi (only once)
    if (!s_wifi_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        
        // Register event handler
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                  &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                  &wifi_event_handler, NULL));
        
        s_wifi_initialized = true;
    }
    
    // Priority 1: Try NVS saved configurations first
    bool has_nvs_config = false;
    for (int i = 0; i < WIFI_MAX_CONFIGS; i++) {
        if (!s_configs[i].valid) continue;
        has_nvs_config = true;

        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_num = 0;
        
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = { .capable = true, .required = false },
            },
        };
        
        strncpy((char*)wifi_config.sta.ssid, s_configs[i].ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, s_configs[i].password, sizeof(wifi_config.sta.password));
        
        ESP_LOGI(TAG_WIFI, "========================================");
        ESP_LOGI(TAG_WIFI, "优先级2: 尝试NVS配置 %d: %s", i, s_configs[i].ssid);
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG_WIFI, "等待连接... (超时: %d 毫秒)", WIFI_CONNECT_TIMEOUT_MS);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        
        if (bits & CONNECTED_BIT) {
            s_wifi_status = WIFI_STATUS_CONNECTED;
            ESP_LOGI(TAG_WIFI, "成功: 连接到NVS配置: %s", s_configs[i].ssid);
            return true;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGW(TAG_WIFI, "失败: 达到最大重试次数: %s", s_configs[i].ssid);
        } else {
            ESP_LOGW(TAG_WIFI, "超时: 无响应: %s", s_configs[i].ssid);
        }
        
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    
    if (!has_nvs_config) {
        ESP_LOGW(TAG_WIFI, "未找到NVS WiFi配置");
    }
    
    // Priority 2: Fall back to sdkconfig default WiFi (e.g. first boot)
    const char* default_ssid = CONFIG_WIFI_SSID;
    const char* default_pass = CONFIG_WIFI_PASSWORD;
    
    if (default_ssid && strlen(default_ssid) > 0) {
        ESP_LOGI(TAG_WIFI, "========================================");
        ESP_LOGI(TAG_WIFI, "NVS失败，尝试sdkconfig默认WiFi");
        ESP_LOGI(TAG_WIFI, "WiFi名称: %s", default_ssid);
        
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_num = 0;
        
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = { .capable = true, .required = false },
            },
        };
        
        strncpy((char*)wifi_config.sta.ssid, default_ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, default_pass, sizeof(wifi_config.sta.password));
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        ESP_LOGI(TAG_WIFI, "等待连接... (超时: %d 毫秒)", WIFI_CONNECT_TIMEOUT_MS);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        
        if (bits & CONNECTED_BIT) {
            s_wifi_status = WIFI_STATUS_CONNECTED;
            ESP_LOGI(TAG_WIFI, "成功: 连接到sdkconfig默认WiFi: %s", default_ssid);
            return true;
        }
    }
    
    s_wifi_status = WIFI_STATUS_DISCONNECTED;
    return false;
}

void wifi_disconnect(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_status = WIFI_STATUS_DISCONNECTED;
}

wifi_status_t wifi_get_status(void)
{
    return s_wifi_status;
}

bool wifi_is_connected(void)
{
    return s_wifi_status == WIFI_STATUS_CONNECTED;
}

bool wifi_save_config(const char* ssid, const char* password)
{
    if (!ssid || !password) return false;
    
    // Find empty slot or replace oldest
    int slot = -1;
    for (int i = 0; i < WIFI_MAX_CONFIGS; i++) {
        if (!s_configs[i].valid) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        // All slots full, use slot 0 (FIFO)
        slot = 0;
    }
    
    // Save to local array
    strncpy(s_configs[slot].ssid, ssid, sizeof(s_configs[slot].ssid) - 1);
    strncpy(s_configs[slot].password, password, sizeof(s_configs[slot].password) - 1);
    s_configs[slot].valid = true;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return false;

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_WIFI_PREFIX, slot);

    err = nvs_set_blob(nvs_handle, key, &s_configs[slot], sizeof(wifi_manager_config_t));
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }

    // ⚠️ 必须 nvs_commit，否则数据只写在缓存中，复位后丢失！
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    return err == ESP_OK;
}

int wifi_load_configs(wifi_manager_config_t* configs, int max_count)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return 0;
    
    int loaded = 0;
    for (int i = 0; i < max_count && i < WIFI_MAX_CONFIGS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_WIFI_PREFIX, i);
        
        if (configs != NULL) {
            size_t size = sizeof(wifi_manager_config_t);
            err = nvs_get_blob(nvs_handle, key, &configs[i], &size);
            if (err == ESP_OK) {
                loaded++;
            }
        } else {
            // Just check if key exists
            size_t size = 0;
            err = nvs_get_blob(nvs_handle, key, NULL, &size);
            if (err == ESP_OK && size > 0) {
                loaded++;
            }
        }
    }
    
    nvs_close(nvs_handle);
    return loaded;
}

bool wifi_start_smartconfig(void)
{
    s_smartconfig_active = true;
    s_wifi_status = WIFI_STATUS_CONFIG_MODE;
    
    // Initialize if not already done
    if (!s_wifi_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        
        // Register handlers
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                  &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                  &wifi_event_handler, NULL));
        
        s_wifi_initialized = true;
    }
    
    // Register SmartConfig event handler
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, 
                                              &wifi_event_handler, NULL));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Start SmartConfig
    esp_err_t err = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    if (err != ESP_OK) return false;
    
    smartconfig_start_config_t cfg_sc = SMARTCONFIG_START_CONFIG_DEFAULT();
    err = esp_smartconfig_start(&cfg_sc);
    
    if (err != ESP_OK) {
        s_smartconfig_active = false;
        return false;
    }
    
    return true;
}

void wifi_stop_smartconfig(void)
{
    esp_smartconfig_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_smartconfig_active = false;
}

void wifi_smartconfig_done(void)
{
    // 只停止 SmartConfig，不停止 WiFi 连接
    // 配网成功后后续需要立即使用 WiFi 做 NTP 同步和天气更新
    esp_smartconfig_stop();
    s_smartconfig_active = false;
    ESP_LOGI(TAG_WIFI, "SmartConfig 已完成，保持WiFi连接");
}

// 扫描附近 AP 并打印信号强度（用于对比天线性能）
static void wifi_scan_ap_rssi(void);

void wifi_event_handler(void* arg, esp_event_base_t event_base,
                       int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG_WIFI, "WiFi STA已启动，连接中...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
                    ESP_LOGW(TAG_WIFI, "从 %s 断开连接, 原因: %d", 
                             event->ssid, event->reason);
                    
                    if (s_smartconfig_active) {
                        // In SmartConfig mode, just clear connected bit
                        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
                    } else {
                        // Normal mode: retry connection
                        if (s_retry_num < WIFI_MAX_RETRY) {
                            // 第一次重试时扫描附近 AP 信号强度，方便对比天线性能
                            if (s_retry_num == 0) {
                                wifi_scan_ap_rssi();
                            }
                            ESP_LOGI(TAG_WIFI, "重试连接 (%d/%d)...", 
                                     s_retry_num + 1, WIFI_MAX_RETRY);
                            esp_wifi_connect();
                            s_retry_num++;
                        } else {
                            ESP_LOGW(TAG_WIFI, "达到最大重试次数，连接失败");
                            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                            s_wifi_status = WIFI_STATUS_DISCONNECTED;
                        }
                    }
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG_WIFI, "已连接到AP，等待IP...");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG_WIFI, "获取到IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
            s_wifi_status = WIFI_STATUS_CONNECTED;
        }
    } else if (event_base == SC_EVENT) {
        switch (event_id) {
            case SC_EVENT_SCAN_DONE:
                ESP_LOGI("SmartConfig", "扫描完成");
                break;
            case SC_EVENT_FOUND_CHANNEL:
                ESP_LOGI("SmartConfig", "找到信道");
                break;
            case SC_EVENT_GOT_SSID_PSWD:
                ESP_LOGI("SmartConfig", "从一键配网获取到SSID和密码");
                
                smartconfig_event_got_ssid_pswd_t* evt = (smartconfig_event_got_ssid_pswd_t*)event_data;
                wifi_config_t config = {0};
                
                config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                config.sta.pmf_cfg.capable = true;
                config.sta.pmf_cfg.required = false;
                
                memcpy(config.sta.ssid, evt->ssid, sizeof(config.sta.ssid));
                memcpy(config.sta.password, evt->password, sizeof(config.sta.password));
                
                ESP_LOGI("SmartConfig", "WiFi名称: %s", config.sta.ssid);

                // 官方 demo 流程: 先 disconnect → set_config → connect
                // 必须先在 SmartConfig sniffer 模式下断开，才能正确应用新配置
                esp_wifi_disconnect();
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
                ESP_LOGI("SmartConfig", "WiFi配置已设置，开始连接...");

                // 保存配置到 NVS
                wifi_save_config((char*)config.sta.ssid, (char*)config.sta.password);

                esp_wifi_connect();
                break;
            default:
                break;
        }
    }
}

// 扫描附近 AP 并打印信号强度（RSSI），用于对比天线性能
static void wifi_scan_ap_rssi(void)
{
    uint16_t ap_count = 0;
    
    ESP_LOGI(TAG_WIFI, "===== 附近 WiFi 信号扫描 =====");
    
    // 扫描（阻塞式，最多 1 秒）
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "WiFi扫描失败: %s", esp_err_to_name(ret));
        return;
    }
    
    // 获取扫描结果数量
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG_WIFI, "未扫描到任何 AP");
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        return;
    }
    
    // 限制最多显示 10 个
    if (ap_count > 10) ap_count = 10;
    
    wifi_ap_record_t* ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        ESP_LOGW(TAG_WIFI, "内存不足，无法分配 AP 列表");
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        return;
    }
    
    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "获取 AP 记录失败: %s", esp_err_to_name(ret));
        free(ap_list);
        return;
    }
    
    // 按 RSSI 排序（信号从强到弱）
    for (int i = 0; i < (int)ap_count - 1; i++) {
        for (int j = i + 1; j < (int)ap_count; j++) {
            if (ap_list[j].rssi > ap_list[i].rssi) {
                wifi_ap_record_t tmp = ap_list[i];
                ap_list[i] = ap_list[j];
                ap_list[j] = tmp;
            }
        }
    }
    
    ESP_LOGI(TAG_WIFI, "扫描到 %d 个 AP (按信号强度排序):", ap_count);
    ESP_LOGI(TAG_WIFI, " %-20s  RSSI  CH  Auth", "SSID");
    ESP_LOGI(TAG_WIFI, " --------------------  ----  --  ----");
    
    for (int i = 0; i < (int)ap_count; i++) {
        const char* auth_str = (ap_list[i].authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA";
        ESP_LOGI(TAG_WIFI, " %-20s  %4d  %2d  %s",
                 ap_list[i].ssid, ap_list[i].rssi, ap_list[i].primary, auth_str);
    }
    
    ESP_LOGI(TAG_WIFI, "===============================");
    
    free(ap_list);
}
