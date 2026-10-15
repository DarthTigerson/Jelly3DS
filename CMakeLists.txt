cmake_minimum_required(VERSION 3.16)
project(Jelly3DS VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Portable core: no 3DS headers, unit-tested on the host.
add_library(jelly_core STATIC
    core/src/util.cpp
    core/src/models.cpp
    core/src/config.cpp
    core/src/transcode.cpp
    core/src/client.cpp
    core/src/progress.cpp
    core/src/downloads.cpp
    core/src/sync.cpp
    core/src/cache.cpp
    platform/curl_http.cpp
)
target_include_directories(jelly_core PUBLIC core/include platform ${THIRD_PARTY})

if(CMAKE_SYSTEM_NAME STREQUAL "Nintendo3DS")
    include(cmake/3ds.cmake)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(jelly_core PUBLIC CURL::libcurl)
    target_compile_options(jelly_core PRIVATE -Wall -Wextra)

    enable_testing()
    add_executable(jelly_tests tests/test_core.cpp)
    target_include_directories(jelly_tests PRIVATE tests)
    target_link_libraries(jelly_tests PRIVATE jelly_core)
    add_test(NAME jelly_tests COMMAND jelly_tests)

    add_executable(jelly-cli tools/cli/main.cpp)
    target_link_libraries(jelly-cli PRIVATE jelly_core)
endif()
