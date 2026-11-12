configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shadowbar.conf.in"
    "${CMAKE_CURRENT_BINARY_DIR}/shadowbar.conf"
    @ONLY
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shadowbarim.conf.in"
    "${CMAKE_CURRENT_BINARY_DIR}/shadowbarim.conf"
    @ONLY
)

string(REGEX MATCHALL "[0-9]+" _shadowbar_fcitx_ver "${Fcitx5Core_VERSION}")
set(SHADOWBAR_FCITX5_MAJOR 5)
set(SHADOWBAR_FCITX5_MINOR 0)
set(SHADOWBAR_FCITX5_PATCH 0)
list(LENGTH _shadowbar_fcitx_ver _shadowbar_fcitx_ver_len)
if(_shadowbar_fcitx_ver_len GREATER 0)
    list(GET _shadowbar_fcitx_ver 0 SHADOWBAR_FCITX5_MAJOR)
endif()
if(_shadowbar_fcitx_ver_len GREATER 1)
    list(GET _shadowbar_fcitx_ver 1 SHADOWBAR_FCITX5_MINOR)
endif()
if(_shadowbar_fcitx_ver_len GREATER 2)
    list(GET _shadowbar_fcitx_ver 2 SHADOWBAR_FCITX5_PATCH)
endif()

add_library(shadowbar MODULE
    shadowbar_addon.cpp
)

set_target_properties(shadowbar PROPERTIES
    PREFIX "lib"
)

target_link_libraries(shadowbar
    PRIVATE
        shadowbar_core
        Fcitx5::Core
        Fcitx5::Config
        Fcitx5::Utils
)

target_compile_definitions(shadowbar
    PRIVATE
        SHADOWBAR_FCITX5_MAJOR=${SHADOWBAR_FCITX5_MAJOR}
        SHADOWBAR_FCITX5_MINOR=${SHADOWBAR_FCITX5_MINOR}
        SHADOWBAR_FCITX5_PATCH=${SHADOWBAR_FCITX5_PATCH}
)

target_compile_options(shadowbar PRIVATE -Wall -Wextra -Wpedantic)

add_library(shadowbarim MODULE
    shadowbar_im.cpp
)

set_target_properties(shadowbarim PROPERTIES
    PREFIX "lib"
)

target_link_libraries(shadowbarim
    PRIVATE
        Fcitx5::Core
        Fcitx5::Config
        Fcitx5::Utils
)

target_compile_options(shadowbarim PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS shadowbar shadowbarim
    LIBRARY DESTINATION "${FCITX_INSTALL_ADDONDIR}"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/shadowbar.conf"
    "${CMAKE_CURRENT_BINARY_DIR}/shadowbarim.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/inputmethod-shadowbar.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod"
    RENAME "shadowbar.conf"
)
