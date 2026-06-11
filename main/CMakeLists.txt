idf_component_register(
    SRCS "main.c"
         "src/power.c"
         "src/button.c"
         "src/display.c"
         "src/font_chars.c"
         "src/wifi_manager.c"
         "src/weather.c"
         "ui/src/ui/ui.c"
         "ui/src/ui/screens.c"
         "ui/src/ui/styles.c"
         "ui/src/ui/actions.c"
         "ui/src/ui/images.c"
         "ui/src/ui/vars.c"
         "ui/src/ui/fonts/fonts.c"
    INCLUDE_DIRS "include" "ui/src/ui" "ui/src/ui/fonts"
    REQUIRES driver esp_wifi esp_http_client esp_event nvs_flash esp_netif esp_rom esp_lcd mbedtls lvgl spiffs esp_pm
)

# Force UTF-8 encoding for Chinese characters in source files
target_compile_options(${COMPONENT_LIB} PRIVATE -finput-charset=UTF-8)
