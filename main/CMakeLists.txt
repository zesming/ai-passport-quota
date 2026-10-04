idf_component_register(
    SRCS "main.c"
         "quota_logic.c"
         "quota_service.c"
         "quota_ui.c"
         "quota_brand_assets.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/quota_font_12.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/quota_font_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_netif esp_pm esp_timer esp_wifi json nvs_flash
)
