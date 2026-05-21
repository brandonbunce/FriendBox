cmake_minimum_required(VERSION 3.16.0)

# Extra IDF component dirs.
#   components/       — vendored LovyanGFX (1.2.7) and ArduinoJson (7.4.2)
#   lib/Panel_PCBA5981 — custom LT7680A panel driver as IDF component
list(APPEND EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
    ${CMAKE_CURRENT_LIST_DIR}/lib/Panel_PCBA5981
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(FriendBox)
