idf_component_register(
    SRCS "main.c"
         "src/power.c"
         "src/button.c"
         "src/display.c"
         "src/wifi_manager.c"
         "src/weather.c"
         "src/ai_image.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_wifi esp_http_client esp_event nvs_flash spiffs esp_netif esp_rom esp_lcd
)
