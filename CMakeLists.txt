cmake_minimum_required(VERSION 3.20)

project(R_E_V_I_A VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REVIA_BUILD_DESKTOP "Build the optional Qt 6 desktop shell" ON)
if(WIN32 AND EXISTS "$ENV{USERPROFILE}/Qt/6.8.3/mingw_64")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{USERPROFILE}/Qt/6.8.3/mingw_64")
endif()

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

FetchContent_Declare(
        sqlite
        URL https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
        URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(httplib json sqlite)

add_library(sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_FTS5)

file(GLOB_RECURSE REVIA_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h"
)

file(GLOB_RECURSE REVIA_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp"
)

set(REVIA_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Private/main.cpp")
list(REMOVE_ITEM REVIA_SOURCES "${REVIA_MAIN_SOURCE}")

add_library(ReviaFoundation STATIC ${REVIA_HEADERS} ${REVIA_SOURCES})

target_include_directories(ReviaFoundation PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Public"
)

# L11: Windows-only definitions are no longer applied unconditionally.
if(WIN32)
    target_compile_definitions(ReviaFoundation PUBLIC
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            NOMINMAX
    )
endif()

target_link_libraries(ReviaFoundation PUBLIC
        httplib::httplib
        nlohmann_json::nlohmann_json
        sqlite3
)

if(WIN32)
    target_link_libraries(ReviaFoundation PUBLIC shell32 dxgi sapi ole32 oleaut32 winmm gdiplus gdi32 bcrypt)
endif()

if(MSVC)
    target_compile_options(ReviaFoundation PRIVATE /W4 /permissive-)
else()
    target_compile_options(ReviaFoundation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(R_E_V_I_A "${REVIA_MAIN_SOURCE}")
target_link_libraries(R_E_V_I_A PRIVATE ReviaFoundation)

if(REVIA_BUILD_DESKTOP)
    find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
        add_executable(ReviaDesktop WIN32
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/main.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/reviaWindow.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/reviaWindow.h"
        )
        target_include_directories(ReviaDesktop PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop"
        )
        target_link_libraries(ReviaDesktop PRIVATE
                ReviaFoundation
                Qt6::Core
                Qt6::Gui
                Qt6::Widgets
        )

        get_target_property(REVIA_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(REVIA_QT_BIN_DIR "${REVIA_QMAKE_EXECUTABLE}" DIRECTORY)
        find_program(REVIA_WINDEPLOYQT_EXECUTABLE
                NAMES windeployqt
                HINTS "${REVIA_QT_BIN_DIR}"
                NO_DEFAULT_PATH
        )
        if(REVIA_WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ReviaDesktop POST_BUILD
                    COMMAND "${REVIA_WINDEPLOYQT_EXECUTABLE}"
                            --no-translations
                            "$<TARGET_FILE:ReviaDesktop>"
                    COMMENT "Deploying Qt runtime for ReviaDesktop"
            )
        endif()

        add_custom_command(TARGET ReviaDesktop POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${CMAKE_CURRENT_SOURCE_DIR}/Config"
                        "$<TARGET_FILE_DIR:ReviaDesktop>/Config"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tools"
                        "$<TARGET_FILE_DIR:ReviaDesktop>/Tools"
                COMMENT "Synchronizing ReviaDesktop runtime configuration"
        )
    else()
        message(STATUS "Qt 6.8 was not found; building the CLI fallback only.")
    endif()
endif()

# Produce a directly runnable Windows executable instead of requiring CLion's
# MinGW runtime directory to be added to PATH at launch time.
if(MINGW)
    target_link_options(R_E_V_I_A PRIVATE -static)
endif()

# Keep runtime configuration synchronized on every build, not only configure.
add_custom_command(TARGET R_E_V_I_A POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/Config"
                "$<TARGET_FILE_DIR:R_E_V_I_A>/Config"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/Tools"
                "$<TARGET_FILE_DIR:R_E_V_I_A>/Tools"
        COMMENT "Synchronizing REVIA runtime configuration"
)

include(CTest)
if(BUILD_TESTING)
    file(GLOB_RECURSE REVIA_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp"
    )
    add_executable(ReviaTests ${REVIA_TEST_SOURCES})
    target_link_libraries(ReviaTests PRIVATE ReviaFoundation)
    if(MINGW)
        target_link_options(ReviaTests PRIVATE -static)
    endif()
    if(MSVC)
        target_compile_options(ReviaTests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(ReviaTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME Revia.Foundation COMMAND ReviaTests)
    if(TARGET ReviaDesktop)
        add_test(NAME Revia.DesktopSmoke COMMAND ReviaDesktop --ui-smoke-test)
        set_tests_properties(Revia.DesktopSmoke PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=windows"
                TIMEOUT 15
        )
    endif()
endif()
