cmake_minimum_required(VERSION 3.16)
project(cc_scout_gui LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.6)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.4)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources)
if(NOT GLIB_COMPILE_RESOURCES)
    message(FATAL_ERROR "glib-compile-resources is required to bundle the device preset")
endif()

set(CC_SCOUT_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cc-scout"
    CACHE PATH "Path to the CC Scout core checkout")
if(NOT EXISTS "${CC_SCOUT_CORE_DIR}/include/cc_scout.h")
    message(FATAL_ERROR "Set CC_SCOUT_CORE_DIR to the CC Scout core checkout")
endif()
add_subdirectory("${CC_SCOUT_CORE_DIR}" cc-scout-core)

set(PRESET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/presets")
set(PRESET_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/preset-resource.c")
add_custom_command(OUTPUT "${PRESET_RESOURCE}"
    COMMAND "${GLIB_COMPILE_RESOURCES}" --generate-source
        --sourcedir "${PRESET_DIR}" --target "${PRESET_RESOURCE}"
        "${PRESET_DIR}/presets.gresource.xml"
    DEPENDS "${PRESET_DIR}/presets.gresource.xml" "${PRESET_DIR}/akai-mpk-mini-iv.yaml"
    VERBATIM)

add_executable(cc-scout-gui
    src/main.c src/device_profile.c src/midi_learn.c src/pad_grid.c
    "${PRESET_RESOURCE}")
target_link_libraries(cc-scout-gui PRIVATE
    CCScout::core PkgConfig::GTK4 PkgConfig::ADWAITA PkgConfig::YAML)
