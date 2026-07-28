# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp)

#target_add_binary_data(${COMPONENT_TARGET} "index.html" TEXT)
#target_add_binary_data(${COMPONENT_TARGET} "sclogo.jpg" TEXT)

set(app_requires
    esp_http_server esp_timer nvs_flash esp_wifi esp_netif
    esp_driver_uart esp_driver_gpio esp_driver_i2c esp_adc
    app_update mdns
    )

# USB Host (for QMX-over-USB CDC-ACM enumeration) and the onboard ST7789 status
# display only exist on the ESP32-S3-USB-OTG board; the Seeed XIAO ESP32-C3
# target has neither a USB-OTG peripheral nor that display.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "esp32s3")
    list(APPEND app_requires usb usb_host_cdc_acm esp_lcd)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "../include"
    REQUIRES ${app_requires}
    EMBED_FILES
        "web/index.htmlgz"
        "web/sclogo.jpg"
        "web/favicon.ico"
        "web/style.cssgz"
        "web/main.jsgz"
        "web/bandprivileges.jsgz"
        "web/spots.jsgz"
        "web/chase.htmlgz"
        "web/chase.jsgz"
        "web/chase_api.jsgz"
        "web/run.htmlgz"
        "web/run.jsgz"
        "web/settings.htmlgz"
        "web/settings.jsgz"
        "web/about.htmlgz"
        "web/about.jsgz"
        "web/qrx.htmlgz"
        "web/qrx.jsgz"
        )

# Apply compile definitions to this component
target_compile_definitions(${COMPONENT_LIB} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_VERBOSE)
