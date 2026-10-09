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
         "quota_usb.c"
         "quota_wifi.c"
         "quota_json.c"
         "quota_ui.c"
         "quota_brand_assets.c"
         "quota_wifi_icons.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/quota_font_12.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/quota_font_16.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "setup_page.html"
    REQUIRES bsp
             esp_app_format
             esp_driver_usb_serial_jtag
             esp_event
             esp_http_client
             esp_http_server
             esp_netif
             esp_pm
             esp_timer
             esp_wifi
             json
             nvs_flash
             mbedtls
             lwip
)

# The git revision is for logs only; the version people see is PROJECT_VER. IDF's git_describe()
# also makes CMake re-run when HEAD moves, so a new commit updates the revision.
git_describe(QUOTA_GIT_REV "${CMAKE_CURRENT_LIST_DIR}" --always --dirty --abbrev=12)
if(NOT QUOTA_GIT_REV OR QUOTA_GIT_REV MATCHES "NOTFOUND")
    set(QUOTA_GIT_REV "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE QUOTA_GIT_REV="${QUOTA_GIT_REV}")
