# The M5Stack CardputerZero build: main/'s portable EchoLink client, UI and
# web config over compat/ (the ESP-IDF and FreeRTOS calls they make, on
# POSIX), with src/'s Linux board, display, keyboard, audio and network in
# place of the ESP32 ones. Built in docker/'s Debian trixie arm64 container by
# scripts/build-cardputer-zero.sh; see docs/cardputer-zero.md.
cmake_minimum_required(VERSION 3.25)
project(scheff_echolink LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MAIN "${REPO}/main")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPO}/VERSION")
file(READ "${REPO}/VERSION" EL_VERSION)
string(STRIP "${EL_VERSION}" EL_VERSION)
string(REGEX REPLACE "^v" "" EL_VERSION "${EL_VERSION}")

# LVGL as on the ESP32 builds (main/idf_component.yml), with lv_conf.h here.
include(FetchContent)
FetchContent_Declare(lvgl
  URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.3.0.tar.gz
  SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(lvgl)

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${lvgl_SOURCE_DIR}/src/*.c")
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC "${lvgl_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

# libgsm, unmodified (components/gsm/CMakeLists.txt).
file(GLOB GSM_SOURCES "${REPO}/components/gsm/src/*.c")
add_library(gsm STATIC ${GSM_SOURCES})
target_include_directories(gsm PUBLIC "${REPO}/components/gsm/include" PRIVATE "${REPO}/components/gsm/private")
target_compile_definitions(gsm PRIVATE SASR NeedFunctionPrototypes=1)
target_compile_options(gsm PRIVATE -O2 -w)

add_library(el_compat STATIC
  compat/esp_compat.c
  compat/freertos_compat.c
  compat/nvs_compat.c
  compat/console_compat.c
  compat/httpd_compat.cpp)
target_include_directories(el_compat PUBLIC compat)
target_compile_definitions(el_compat PUBLIC _GNU_SOURCE)
find_package(Threads REQUIRED)
target_link_libraries(el_compat PUBLIC Threads::Threads)

# The web config page; index_html.S .incbin's it.
set_source_files_properties(src/index_html.S PROPERTIES
  COMPILE_DEFINITIONS "INDEX_HTML=\"${MAIN}/web/index.html\""
  OBJECT_DEPENDS "${MAIN}/web/index.html")

set(PORTABLE_SOURCES
  app_main.c console_cmds.c el_directory.c el_qso.c file_store.c onboard.c
  server_list.c settings.c ui.c ui_home.c ui_chat.c ui_wifi.c ui_account.c
  web_config.c xml.c)
list(TRANSFORM PORTABLE_SOURCES PREPEND "${MAIN}/")

add_executable(scheff-echolink
  ${PORTABLE_SOURCES}
  src/index_html.S
  src/main.c
  src/board_cp0.c
  src/display_cp0.c
  src/keys_cp0.c
  src/audio_cp0.c
  src/net_cp0.c
  src/sdcard_cp0.c
  src/ptt_cp0.c)
target_include_directories(scheff-echolink PRIVATE "${MAIN}" src)
target_compile_definitions(scheff-echolink PRIVATE EL_VERSION="${EL_VERSION}")
# ESP-IDF's headers bring sdkconfig.h into every file; compat's don't all.
target_compile_options(scheff-echolink PRIVATE -include sdkconfig.h)
target_compile_options(scheff-echolink PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function
  -Wno-format-truncation -Wno-missing-braces -Wno-deprecated-declarations)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
pkg_check_modules(EXPAT REQUIRED IMPORTED_TARGET expat)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
target_link_libraries(scheff-echolink PRIVATE lvgl gsm el_compat PkgConfig::PULSE PkgConfig::EXPAT
  PkgConfig::CJSON m)

# The package: cpack -G DEB in build/cp0 gives
# scheff-echolink-cardputer-zero-v<version>_arm64.deb, laid out for the
# CardputerZero's launcher (APPLaunch), which notices the .desktop file
# within 3 s, so there are no maintainer scripts.
install(TARGETS scheff-echolink RUNTIME DESTINATION share/APPLaunch/bin)
install(FILES packaging/scheff-echolink.desktop DESTINATION share/APPLaunch/applications)
install(FILES packaging/icon.png DESTINATION share/APPLaunch/share/images RENAME scheff-echolink.png)
install(FILES packaging/copyright DESTINATION share/doc/scheff-echolink)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME scheff-echolink)
set(CPACK_PACKAGE_VERSION "${EL_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "scheff-echolink-cardputer-zero-v${EL_VERSION}_arm64")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
# The store matches this email to the publisher's GitHub account.
set(CPACK_PACKAGE_CONTACT "oumike <michael.cojocari@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Scheff EchoLink: an EchoLink client")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
  "An EchoLink client for licensed amateur radio operators: browse the\nEchoLink directory, connect to repeaters, links and conferences, and talk\nwith the Space bar, through the speaker or a USB headset.")
set(CPACK_DEBIAN_PACKAGE_SECTION APPLaunch)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/oumike/scheff-echolink")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libpulse0, libexpat1, libcjson1")
set(CPACK_STRIP_FILES ON)
include(CPack)
