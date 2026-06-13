# Friendbox "main" component — IDF entry point + all firmware sources.
# Note: lib/Panel_PCBA5981 and components/LovyanGFX are separate IDF
# components registered via the top-level CMakeLists EXTRA_COMPONENT_DIRS.

set(SRCS
    "main.cpp"
    "audio.cpp"
    "audio_i2s.cpp"
    "auth.cpp"
    "canvas.cpp"
    "display.cpp"
    "fbox_source.cpp"
    "io.cpp"
    "mem.cpp"
    "network.cpp"
    "lt_assets.cpp"
    "lt_glyph_data.cpp"
    "lt_splash_data.cpp"
    "ui/ui.cpp"
    "ui/ui_widgets.cpp"
    "ui/ui_anim.cpp"
    "ui/ui_theme.cpp"
    "ui/ui_sfx.cpp"
    "ui/ui_screens.cpp"
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
