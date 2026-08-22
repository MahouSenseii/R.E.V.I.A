cmake_minimum_required(VERSION 3.20)

project(R_E_V_I_A VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REVIA_BUILD_DESKTOP "Build the optional Qt 6 desktop shell" ON)
option(REVIA_REQUIRE_DESKTOP "Fail configuration when the Qt desktop shell cannot be built" OFF)
set(REVIA_QT_ROOT "" CACHE PATH "Explicit Qt 6 kit directory, e.g. C:/Qt/6.8.3/mingw_64")

# Qt discovery must not be pinned to one version at one path on one machine.
# Resolution order: explicit cache variable, REVIA_QT_ROOT environment variable,
# then the newest 6.x mingw_64 kit found in the usual install locations.
function(revia_resolve_qt_root outVariable)
    if(REVIA_QT_ROOT)
        set(${outVariable} "${REVIA_QT_ROOT}" PARENT_SCOPE)
        return()
    endif()
    if(DEFINED ENV{REVIA_QT_ROOT} AND EXISTS "$ENV{REVIA_QT_ROOT}")
        set(${outVariable} "$ENV{REVIA_QT_ROOT}" PARENT_SCOPE)
        return()
    endif()

    set(searchRoots "$ENV{USERPROFILE}/Qt" "C:/Qt" "$ENV{QT_ROOT_DIR}")
    set(candidates "")
    foreach(searchRoot IN LISTS searchRoots)
        if(NOT searchRoot OR NOT IS_DIRECTORY "${searchRoot}")
            continue()
        endif()
        file(GLOB kits "${searchRoot}/6.*/mingw_64")
        list(APPEND candidates ${kits})
    endforeach()

    if(NOT candidates)
        set(${outVariable} "" PARENT_SCOPE)
        return()
    endif()

    # Natural sort so 6.10.0 ranks above 6.9.0 rather than below it.
    list(REMOVE_DUPLICATES candidates)
    list(SORT candidates COMPARE NATURAL ORDER DESCENDING)
    list(GET candidates 0 newest)
    set(${outVariable} "${newest}" PARENT_SCOPE)
endfunction()

if(WIN32 AND REVIA_BUILD_DESKTOP)
    revia_resolve_qt_root(REVIA_RESOLVED_QT_ROOT)
    if(REVIA_RESOLVED_QT_ROOT)
        message(STATUS "Revia: using Qt kit at ${REVIA_RESOLVED_QT_ROOT}")
        list(PREPEND CMAKE_PREFIX_PATH "${REVIA_RESOLVED_QT_ROOT}")
    endif()
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
    target_link_libraries(ReviaFoundation PUBLIC shell32 dxgi sapi ole32 oleaut32 winmm gdiplus gdi32 bcrypt winhttp psapi pdh dbghelp)
endif()

if(MSVC)
    target_compile_options(ReviaFoundation PRIVATE /W4 /permissive-)
else()
    target_compile_options(ReviaFoundation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(R_E_V_I_A "${REVIA_MAIN_SOURCE}")
target_link_libraries(R_E_V_I_A PRIVATE ReviaFoundation)

if(REVIA_BUILD_DESKTOP)
    # SvgWidgets renders the Canvas tab. It ships with the standard Qt kit, so the
    # desktop shell still builds from an ordinary install with no extra component.
    find_package(Qt6 6.8 COMPONENTS Core Gui Widgets Svg SvgWidgets)
    if(Qt6_FOUND)
        add_executable(ReviaDesktop WIN32
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/canvasPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/canvasPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/capabilityPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/capabilityPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/main.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/pipelinePanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/pipelinePanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/resourcePanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/resourcePanel.h"
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
                Qt6::Svg
                Qt6::SvgWidgets
        )

        # A shared Qt build cannot be fully statically linked, but the compiler
        # runtime can be. Without this the shipped executable depends on
        # libgcc_s_seh-1.dll, libstdc++-6.dll, and libwinpthread-1.dll being
        # present on the target machine, which is only true on a dev box.
        if(MINGW)
            target_link_options(ReviaDesktop PRIVATE
                    -static-libgcc
                    -static-libstdc++
                    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive
            )
        endif()

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
        set(REVIA_QT_HELP
                "Qt 6.8 or newer was not found, so ReviaDesktop will NOT be built.\n"
                "  Install it with:  .\\Tools\\InstallQt.ps1\n"
                "  Or point at an existing kit:  -DREVIA_QT_ROOT=C:/Qt/6.8.3/mingw_64\n"
                "  The MinGW kit is required; an MSVC kit is ABI-incompatible with this build.\n"
                "  Configure with -DREVIA_BUILD_DESKTOP=OFF to silence this deliberately.")
        if(REVIA_REQUIRE_DESKTOP)
            message(FATAL_ERROR ${REVIA_QT_HELP})
        endif()
        # A warning, not a status line: silently degrading to a CLI-only build is
        # the single most confusing failure mode this project has.
        message(WARNING ${REVIA_QT_HELP})
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
