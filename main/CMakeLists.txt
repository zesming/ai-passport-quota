idf_component_register(
    SRCS "main.c"
         "quota_logic.c"
         "quota_catalog.c"
         "quota_service.c"
         "quota_portable_service.c"
         "quota_direct.c"
         "quota_direct_logic.c"
         "quota_store.c"
         "quota_portal.c"
         "quota_ui.c"
         "quota_brand_assets.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/quota_font_12.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/quota_font_16.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "portable_setup.html"
    REQUIRES bsp esp_driver_usb_serial_jtag esp_event esp_http_client esp_http_server esp_netif esp_pm esp_timer esp_wifi json nvs_flash mbedtls lwip
)
