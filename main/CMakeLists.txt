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

# Force UTF-8 encoding for Chinese characters in source files
target_compile_options(${COMPONENT_LIB} PRIVATE -finput-charset=UTF-8)
