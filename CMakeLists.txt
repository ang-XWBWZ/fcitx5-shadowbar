cmake_minimum_required(VERSION 3.20)
project(fcitx5-shadowbar VERSION 0.1.3 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(BUILD_FCITX5_ADDON "Build the Fcitx5 module addon (Linux)" ON)
option(BUILD_CLI_SIM "Build the standalone terminal interactive simulator" ON)
option(BUILD_FLOATING_UI "Build native Qt6 floating translator and settings UI" ON)
option(BUILD_TESTS "Build unit and integration tests" ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 QUIET)
find_package(ZLIB QUIET)

set(SHADOWBAR_SQLITE3_TARGET "")
if(TARGET SQLite3::SQLite3)
    set(SHADOWBAR_SQLITE3_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(SHADOWBAR_SQLITE3_TARGET SQLite::SQLite3)
endif()

add_subdirectory(core)

if(BUILD_FCITX5_ADDON)
    find_package(Fcitx5Core 5.0.20 REQUIRED)
    find_package(Fcitx5Config REQUIRED)
    find_package(Fcitx5Utils REQUIRED)
    add_subdirectory(fcitx5-addon)
endif()

if(BUILD_CLI_SIM)
    add_subdirectory(cli-sim)
endif()

if(BUILD_FLOATING_UI)
    add_subdirectory(floating-ui)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/config/config.toml.example"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/INSTALL.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/INSTALL.zh-CN.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/PolyForm-Noncommercial-1.0.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/CC-BY-NC-SA-4.0.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/LICENSES"
)

set(CPACK_PACKAGE_NAME "fcitx5-shadowbar")
set(CPACK_PACKAGE_VENDOR "ang (ang-XWBWZ)")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Floating real-time translation shadow bar addon, input method, and Qt6 floating card for Fcitx5")
set(CPACK_PACKAGE_DESCRIPTION
    "ShadowBar adds a live companion translation bar inside Fcitx5 with OneShot (;;), Sticky (Ctrl+Shift+T), global shortcuts, 20-language routing, Wayland/X11 background clipboard monitoring, multi-format offline dictionaries, local Hunyuan HY-MT1.5 (1.25bit/2bit) GGUF support, and a built-in Chinese environment & IME fixer.")
set(CPACK_PACKAGE_CONTACT "ang <ang-XWBWZ@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "fcitx5 (>= 5.0.20)")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "wl-clipboard, fcitx5-chinese-addons, fonts-noto-cjk")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
include(CPack)
