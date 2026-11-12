add_executable(shadowbar-sim
    main.cpp
)

target_link_libraries(shadowbar-sim
    PRIVATE
        shadowbar_core
)

target_compile_options(shadowbar-sim PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS shadowbar-sim
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
