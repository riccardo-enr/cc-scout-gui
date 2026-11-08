cmake_minimum_required(VERSION 3.16)
project(cc_scout_gui LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.6)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.4)

set(CC_SCOUT_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cc-scout"
    CACHE PATH "Path to the CC Scout core checkout")
if(NOT EXISTS "${CC_SCOUT_CORE_DIR}/include/cc_scout.h")
    message(FATAL_ERROR "Set CC_SCOUT_CORE_DIR to the CC Scout core checkout")
endif()
add_subdirectory("${CC_SCOUT_CORE_DIR}" cc-scout-core)

add_executable(cc-scout-gui src/main.c)
target_link_libraries(cc-scout-gui PRIVATE
    CCScout::core PkgConfig::GTK4 PkgConfig::ADWAITA)
