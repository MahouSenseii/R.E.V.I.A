cmake_minimum_required(VERSION 3.20)

project(R_E_V_I_A)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Prevent cpp-httplib from using Windows API calls that MinGW may not expose correctly.
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.18.1
)

FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(httplib json)

file(GLOB_RECURSE REVIA_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h"
)

file(GLOB_RECURSE REVIA_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp"
)

add_executable(R_E_V_I_A
        ${REVIA_HEADERS}
        ${REVIA_SOURCES}
)

target_include_directories(R_E_V_I_A PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Public"
)

target_compile_definitions(R_E_V_I_A PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
)

target_link_libraries(R_E_V_I_A PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
)

file(COPY Config DESTINATION ${CMAKE_BINARY_DIR})