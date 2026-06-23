# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
FILE(GLOB_RECURSE led_sources ${CMAKE_SOURCE_DIR}/lib/led/*.cpp)
FILE(GLOB_RECURSE reed_sources ${CMAKE_SOURCE_DIR}/lib/reed/*.cpp)
FILE(GLOB_RECURSE espnow_sources ${CMAKE_SOURCE_DIR}/lib/esp_now/*.cpp)
FILE(GLOB_RECURSE voltage_sources ${CMAKE_SOURCE_DIR}/lib/voltage/*.cpp)

idf_component_register(
	SRCS ${app_sources} ${led_sources} ${reed_sources} ${espnow_sources} ${voltage_sources}
	INCLUDE_DIRS "../include" "../lib/led" "../lib/reed" "../lib/esp_now" "../lib/voltage"
)
