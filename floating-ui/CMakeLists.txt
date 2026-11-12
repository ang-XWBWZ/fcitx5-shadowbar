find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets DBus)

set(CMAKE_AUTOMOC ON)

add_executable(shadowbar-float
    main.cpp
    hotkey_util.cpp
    window_pin_util.cpp
    clipboard_monitor.cpp
    floating_orb.cpp
    ty_model_helpers.cpp
    ty_model_manager.cpp
    zh_env_fixer.cpp
    settings_view.cpp
    floating_window.cpp
)

target_link_libraries(shadowbar-float
    PRIVATE
        shadowbar_core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::DBus
)

target_compile_options(shadowbar-float PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS shadowbar-float
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-shadowbar-float.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
