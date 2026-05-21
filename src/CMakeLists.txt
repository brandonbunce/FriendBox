# Friendbox "main" component — IDF entry point + all firmware sources.
# Note: lib/Panel_PCBA5981 and components/LovyanGFX are separate IDF
# components registered via the top-level CMakeLists EXTRA_COMPONENT_DIRS.

set(SRCS
    "main.cpp"
    "audio.cpp"
    "audio_i2s.cpp"
    "canvas.cpp"
    "display.cpp"
    "fbox_source.cpp"
    "io.cpp"
    "network.cpp"
    "ui/ui_core.cpp"
    "ui/ui_screen_canvas_menu.cpp"
    "ui/ui_screen_file_browser.cpp"
    "ui/ui_screen_send.cpp"
)

set(INCLUDE_DIRS
    "."
    "../include"
)

set(REQUIRES
    LovyanGFX
    Panel_PCBA5981
    ArduinoJson
    driver
    esp_event
    esp_http_client
    esp_netif
    esp_psram
    esp_rom
    esp_system
    esp_timer
    esp_wifi
    esp-tls
    fatfs
    freertos
    log
    mbedtls
    nvs_flash
    sdmmc
    spi_flash
    vfs
)

idf_component_register(
    SRCS         ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES     ${REQUIRES}
)
