cmake_minimum_required(VERSION 3.20)

find_package(SQLite3 QUIET)
find_package(ZLIB QUIET)

add_library(shadowbar_core STATIC
    src/utf8_util.cpp
    src/config.cpp
    src/offline_dict_data.cpp
    src/cache.cpp
    src/sentence_splitter.cpp
    src/translator.cpp
    src/session.cpp
    src/c_api.cpp
)

target_include_directories(shadowbar_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(shadowbar_core
    PUBLIC
        CURL::libcurl
        nlohmann_json::nlohmann_json
        Threads::Threads
)

if(SQLite3_FOUND)
    target_link_libraries(shadowbar_core PRIVATE SQLite3::SQLite3)
    target_compile_definitions(shadowbar_core PRIVATE SHADOWBAR_HAS_SQLITE3=1)
endif()

if(ZLIB_FOUND)
    target_link_libraries(shadowbar_core PRIVATE ZLIB::ZLIB)
    target_compile_definitions(shadowbar_core PRIVATE SHADOWBAR_HAS_ZLIB=1)
endif()

target_compile_options(shadowbar_core PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(shadowbar_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
