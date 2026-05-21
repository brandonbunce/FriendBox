# Panel_PCBA5981 — custom LovyanGFX panel driver for the LT7680A + ST7701S
# combination on the PCBA5981 hardware revision. Registered as an IDF
# component so the source compiles under framework = espidf alongside
# the vendored LovyanGFX in components/.

idf_component_register(
    SRCS         "Panel_PCBA5981.cpp"
    INCLUDE_DIRS "."
    REQUIRES     LovyanGFX driver esp_timer freertos
)
