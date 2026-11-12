add_executable(test_core
    test_core.cpp
)
target_link_libraries(test_core PRIVATE shadowbar_core)
if(SHADOWBAR_SQLITE3_TARGET)
    target_link_libraries(test_core PRIVATE ${SHADOWBAR_SQLITE3_TARGET})
    target_compile_definitions(test_core PRIVATE SHADOWBAR_HAS_SQLITE3=1)
endif()
target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME shadowbar_core_tests COMMAND test_core)

if(BUILD_FCITX5_ADDON)
    find_package(Fcitx5ModuleTestFrontend REQUIRED)
    find_package(Fcitx5ModuleTestIM REQUIRED)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/addon/shadowbar.conf.in"
        "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon/shadowbar.conf"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/addon/shadowbarim.conf.in"
        "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon/shadowbarim.conf"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/inputmethod/shadowbar.conf"
        "${CMAKE_CURRENT_BINARY_DIR}/testdata/inputmethod/shadowbar.conf"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/addon/testfrontend.conf"
        "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon/testfrontend.conf"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/addon/testim.conf"
        "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon/testim.conf"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/addon/testui.conf"
        "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon/testui.conf"
        COPYONLY
    )

    add_executable(test_fcitx5_integration
        test_fcitx5_integration.cpp
    )
    target_link_libraries(test_fcitx5_integration
        PRIVATE
            shadowbar_core
            Fcitx5::Core
            Fcitx5::Utils
            Fcitx5::Module::TestFrontend
            Fcitx5::Module::TestIM
    )
    target_compile_definitions(test_fcitx5_integration
        PRIVATE
            SHADOWBAR_BUILD_DIR="$<TARGET_FILE_DIR:shadowbar>"
            SHADOWBAR_TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/testdata"
            SHADOWBAR_FCITX5_ADDON_DIR="${FCITX_INSTALL_ADDONDIR}"
            SHADOWBAR_FCITX5_DATADIR="${FCITX_INSTALL_PKGDATADIR}"
    )
    target_compile_options(test_fcitx5_integration PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME shadowbar_fcitx5_integration COMMAND test_fcitx5_integration)
endif()

if(BUILD_FLOATING_UI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus)
    add_executable(test_floating_ui_helpers
        test_floating_ui_helpers.cpp
        "${CMAKE_SOURCE_DIR}/floating-ui/hotkey_util.cpp"
        "${CMAKE_SOURCE_DIR}/floating-ui/ty_model_helpers.cpp"
        "${CMAKE_SOURCE_DIR}/floating-ui/clipboard_monitor.cpp"
        "${CMAKE_SOURCE_DIR}/floating-ui/zh_env_fixer.cpp"
    )
    set_target_properties(test_floating_ui_helpers PROPERTIES AUTOMOC ON)
    target_include_directories(test_floating_ui_helpers PRIVATE
        "${CMAKE_SOURCE_DIR}/floating-ui"
    )
    target_link_libraries(test_floating_ui_helpers PRIVATE
        shadowbar_core
        Qt6::Core
        Qt6::Gui
        Qt6::DBus
    )
    target_compile_options(test_floating_ui_helpers PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME shadowbar_floating_ui_helpers COMMAND test_floating_ui_helpers)
endif()
