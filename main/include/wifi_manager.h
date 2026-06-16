/**
 * @file wifi_manager.h
 * @brief WiFi management module
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "esp_event.h"

// WiFi connection status
typedef enum {
    WIFI_STATUS_IDLE = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONFIG_MODE,
} wifi_status_t;

// WiFi configuration structure (custom, not conflicting with ESP-IDF)
typedef struct {
    char ssid[32];
    char password[64];
    bool valid;
} wifi_manager_config_t;

/**
 * @brief Initialize WiFi module
 */
void wifi_manager_init(void);

/**
 * @brief Connect to saved WiFi networks (tries all saved configs)
 * @return true if connected successfully
 */
bool wifi_connect(void);

/**
 * @brief Disconnect WiFi
 */
void wifi_disconnect(void);

/**
 * @brief Get current WiFi status
 * @return wifi_status_t Current status
 */
wifi_status_t wifi_get_status(void);

/**
 * @brief Check if WiFi is connected
 * @return true if connected
 */
bool wifi_is_connected(void);

/**
 * @brief Get the SSID of the currently connected AP
 * @return SSID string, or NULL if not connected
 */
const char* wifi_get_connected_ssid(void);

/**
 * @brief Save WiFi configuration to NVS
 * @param ssid SSID string
 * @param password Password string
 * @return true if saved successfully
 */
bool wifi_save_config(const char* ssid, const char* password);

/**
 * @brief Load saved WiFi configurations
 * @param configs Array to store configs
 * @param max_count Maximum number of configs to load
 * @return Number of configs loaded
 */
int wifi_load_configs(wifi_manager_config_t* configs, int max_count);

/**
 * @brief Start SmartConfig provisioning
 * @return true if started successfully
 */
bool wifi_start_smartconfig(void);

/**
 * @brief Stop SmartConfig provisioning
 */
void wifi_stop_smartconfig(void);

/**
 * @brief Stop SmartConfig but keep WiFi connection alive
 *
 * Use this after SmartConfig successfully connected to an AP.
 * Unlike wifi_stop_smartconfig(), this does NOT call esp_wifi_stop(),
 * so the ongoing WiFi connection is preserved for subsequent NTP/weather updates.
 */
void wifi_smartconfig_done(void);

/**
 * @brief WiFi event handler (call from app_main)
 * @param arg Event context
 * @param event_base Event base
 * @param event_id Event ID
 * @param event_data Event data
 */
void wifi_event_handler(void* arg, esp_event_base_t event_base,
                       int32_t event_id, void* event_data);

#endif // WIFI_MANAGER_H
