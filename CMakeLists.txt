cmake_minimum_required(VERSION 3.20)

project(R_E_V_I_A VERSION 0.2.0 LANGUAGES C CXX)

# Post-build runtime sync. Tools/ is ~3,200 files; rewriting all of them into the build
# tree on every build (twice -- once per executable) churned a sync-watched folder and
# intermittently failed with "Error copying directory" while a file was held. Copying
# only what changed avoids both where CMake supports it.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
    set(REVIA_COPY_DIRECTORY copy_directory_if_different)
else()
    set(REVIA_COPY_DIRECTORY copy_directory)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Template-heavy MinGW objects and repeated foundation links need separate limits.
# Cache values let a measured local build choose a different compile budget.
set(REVIA_COMPILE_JOBS 2 CACHE STRING "Maximum simultaneous Ninja MinGW compiles")
set(REVIA_LINK_JOBS 1 CACHE STRING "Maximum simultaneous Ninja MinGW links")
if(CMAKE_GENERATOR MATCHES "Ninja" AND MINGW)
    foreach(budget REVIA_COMPILE_JOBS REVIA_LINK_JOBS)
        if(NOT "${${budget}}" MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "${budget} must be a positive integer")
        endif()
    endforeach()
    set_property(GLOBAL PROPERTY JOB_POOLS revia_compile=${REVIA_COMPILE_JOBS} revia_link=${REVIA_LINK_JOBS})
    set(CMAKE_JOB_POOL_COMPILE revia_compile)
    set(CMAKE_JOB_POOL_LINK revia_link)
endif()

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

# Once objects are cached, a Debug build is mostly linking: about eighty executables each
# pull in ReviaFoundation, a static library of roughly a gigabyte with debug information.
# GNU ld takes seconds and up to several gigabytes of memory per executable; LLVM's lld
# links the same objects about ten times faster in a fraction of the memory. The objects,
# and so the code under test, are identical with either linker.
#
# Opt-in, because it needs LLVM's ld.lld on PATH. CI turns it on. On Windows the toolchain,
# Qt and build paths must be free of spaces: lld reads GCC's response files for long links
# with Windows quoting, so a spaced path in one fails to open.
# lld also takes a symbol from any archive whatever the order and ignores --start-group,
# so a link line can work with lld and fail with GNU ld. CI relinks a few executables
# with GNU ld to catch that.
option(REVIA_USE_LLD "Link with LLVM lld instead of GNU ld when the compiler can run it" OFF)

function(revia_use_lld)
    if(MSVC)
        message(WARNING "REVIA_USE_LLD applies to GCC and Clang; keeping the default linker.")
        return()
    endif()

    # Ask the compiler which linker it really runs. A GCC configured with an absolute
    # --with-ld (Debian's MinGW cross compiler is one) does not fail when there is no
    # ld.lld beside that path: it quietly links with GNU ld, and the build stays slow.
    execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -fuse-ld=lld -Wl,--version
            RESULT_VARIABLE probeResult
            OUTPUT_VARIABLE probeOutput
            ERROR_VARIABLE probeError)
    # The linker prints its version on stdout ("LLD 20.1.8 ...", or "GNU ld ..."); the
    # driver's command line and any "cannot find 'ld'" error go to stderr.
    string(REGEX MATCH "LLD [0-9][0-9.]*" lldVersion "${probeOutput}")
    if(NOT probeResult EQUAL 0 OR NOT lldVersion)
        string(REGEX MATCH "^[^\n]+" probeText "${probeOutput}")
        if(NOT probeText)
            string(REGEX MATCH "[^\n]*error[^\n]*" probeText "${probeError}")
        endif()
        message(WARNING
                "REVIA_USE_LLD is ON, but ${CMAKE_CXX_COMPILER} does not link with lld, "
                "so the build keeps GNU ld and its slow links. Put LLVM's bin directory, "
                "which holds ld.lld, on PATH and configure again.\n"
                "The compiler's linker reported: ${probeText}")
        return()
    endif()

    set(lldOptions -fuse-ld=lld)
    if(MINGW)
        # lld reserves a 1 MiB stack for the main thread where GNU ld reserves 2 MiB, and
        # threads started without an explicit size get the same reservation. Keep GNU
        # ld's size so deep -O0 call chains behave as they do with the default linker.
        list(APPEND lldOptions -Wl,--stack,2097152)
    endif()

    # A whole program, with its startup code and runtime libraries, has to link too.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${lldOptions})
    check_cxx_source_compiles("int main() { return 0; }" REVIA_LLD_LINKS_A_PROGRAM)
    if(NOT REVIA_LLD_LINKS_A_PROGRAM)
        message(WARNING
                "REVIA_USE_LLD is ON and ${lldVersion} was found, but a test program did "
                "not link with it, so the build keeps GNU ld. The configure log under "
                "CMakeFiles/ has the linker's error.")
        return()
    endif()

    # Directory-wide, so every executable below links with it, the test fixtures too.
    add_link_options(${lldOptions})
    message(STATUS "Revia: linking with ${lldVersion}")
endfunction()

if(REVIA_USE_LLD)
    revia_use_lld()
endif()

include(FetchContent)

# Prevent cpp-httplib from using Windows API calls that MinGW may not expose correctly.
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG 5c1a34e766817cf0865ca8bc5af5edeb0f9b2df4 # v0.18.1; a tag can be moved, a commit cannot
)

FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
        sqlite
        URL https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
        URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# A namespace-aware XML parser for untrusted SVG. It has no resource loader;
# the sanitizer additionally rejects DTDs and all external entity callbacks.
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(EXPAT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        expat
        URL https://github.com/libexpat/libexpat/archive/12cf0b1f25f026a022fe728ad8f7e3d017285b80.tar.gz
        URL_HASH SHA256=0b53c69a66f94a3e6865f81ec353e6f4715554383848e5482d737c212d8dfccb
        SOURCE_SUBDIR expat
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(httplib json sqlite expat)

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
        expat::expat
)

if(WIN32)
    # Media Foundation (mfplat/mf/mfreadwrite/mfuuid) backs camera capture; shlwapi comes
    # with it. Screen capture continues to use GDI+, which also encodes the camera PNG.
    target_link_libraries(ReviaFoundation PUBLIC shell32 dxgi sapi ole32 oleaut32 winmm gdiplus gdi32 bcrypt crypt32 winhttp psapi pdh dbghelp mfplat mf mfreadwrite mfuuid shlwapi advapi32 dwmapi iphlpapi)
endif()

if(MSVC)
    target_compile_options(ReviaFoundation PRIVATE /W4 /permissive-)
else()
    target_compile_options(ReviaFoundation PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    # Template-heavy translation units exceed PE/COFF's ordinary section limit
    # in Debug builds. Select the extended object format explicitly.
    target_compile_options(ReviaFoundation PRIVATE -Wa,-mbig-obj)
endif()

# GNU ar indexes qc by default; CMake's separate ranlib then repeats that work.
# Append every member without indexing and build the full symbol table once.
if(MINGW)
    set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> qcS <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> qS <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcS <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qS <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()

function(revia_deploy_mingw_runtime targetName)
    if(NOT MINGW)
        return()
    endif()
    get_filename_component(compilerBin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(threadRuntime "${compilerBin}/libwinpthread-1.dll")
    if(NOT EXISTS "${threadRuntime}")
        message(FATAL_ERROR "Missing MinGW thread runtime: ${threadRuntime}")
    endif()
    add_custom_command(TARGET ${targetName} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${threadRuntime}" "$<TARGET_FILE_DIR:${targetName}>/libwinpthread-1.dll"
            COMMENT "Deploying MinGW thread runtime for ${targetName}"
            VERBATIM)
endfunction()

function(revia_link_foundation targetName)
    # ReviaFoundation has intentional cross-subsystem references (the session
    # coordinator reaches speech, vision, actions, and visuals). Let GNU ld
    # rescan the foundation for those references without forcing every unrelated
    # member into each executable. The native web host, for example, does not
    # need the desktop-control and voice implementations.
    if(MINGW)
        target_link_libraries(${targetName} PRIVATE
                -Wl,--start-group ReviaFoundation -Wl,--end-group)
    else()
        target_link_libraries(${targetName} PRIVATE ReviaFoundation)
    endif()
endfunction()

add_executable(R_E_V_I_A "${REVIA_MAIN_SOURCE}")
revia_link_foundation(R_E_V_I_A)

if(REVIA_BUILD_DESKTOP)
    # SvgWidgets renders the Canvas tab. It ships with the standard Qt kit, so the
    # desktop shell still builds from an ordinary install with no extra component.
    find_package(Qt6 6.8 COMPONENTS Core Gui Widgets Svg SvgWidgets)
    if(Qt6_FOUND)
        # The "# [sources:X]" lines mark where each part of the desktop redesign adds its files.
        # Unchanged lines sit between them, so the parts merge without touching the same lines.
        add_executable(ReviaDesktop WIN32
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/videoPreviewDialog.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/videoPreviewDialog.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/conversationStatusPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/conversationStatusPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/desktopObjectNames.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsTypes.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/answerFeedbackDialog.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/answerFeedbackDialog.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/attachmentTray.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/attachmentTray.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/documentsDialog.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/documentsDialog.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/canvasPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/chatMarkdown.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/chatMarkdown.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/chatSessionsPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/chatSessionsPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/agentStudioPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/agentStudioPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/companionStudio.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/learningStudioPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/learningStudioPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/developmentStudioPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/developmentStudioPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/audienceStudioPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/audienceStudioPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/canvasPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/capabilityPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/capabilityPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/internetActivityPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/internetActivityPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/main.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/memoryPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/mindPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/visionPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/visionPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/voiceHealthPanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/voiceHealthPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/windowLayout.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/windowShell.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/serialWorker.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/tabNavigation.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/tabNavigation.h"
                # [sources:C]
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/commandPalette.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/commandPalette.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/composerCard.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/composerCard.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/panelWindow.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/panelWindow.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/shellSidebar.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/shellSidebar.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/shortcutSheet.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/shortcutSheet.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/softShadow.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/softShadow.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/toastOverlay.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/toastOverlay.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/mindPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/memoryPanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/pipelinePanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/pipelinePanel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/profilePanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/profilePanel.h"
                # [sources:B1]
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsSessionBackend.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsSessionBackend.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/resourcePanel.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/resourcePanel.h"
                # [sources:B2]
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsCatalog.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsCatalog.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsDialog.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/settingsDialog.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/reviaWindow.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/reviaWindow.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/reviaWindow.ui"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/toggleSwitch.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/toggleSwitch.h"
                # [sources:A]
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/theme.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/theme.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop/reviaResources.qrc"
        )
        set_target_properties(ReviaDesktop PROPERTIES AUTOUIC ON AUTORCC ON)
        target_include_directories(ReviaDesktop PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop"
        )
        revia_link_foundation(ReviaDesktop)
        target_link_libraries(ReviaDesktop PRIVATE
                Qt6::Core
                Qt6::Gui
                Qt6::Widgets
                Qt6::Svg
                Qt6::SvgWidgets
        )

        # A shared Qt build cannot be fully statically linked, but the compiler
        # runtime can be. Without this the shipped executable depends on
        # libgcc_s_seh-1.dll and libstdc++-6.dll being
        # present on the target machine, which is only true on a dev box.
        if(MINGW)
            target_link_options(ReviaDesktop PRIVATE
                    -static-libgcc
                    -static-libstdc++
            )
        endif()

        revia_deploy_mingw_runtime(ReviaDesktop)

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
                COMMAND ${CMAKE_COMMAND} -E ${REVIA_COPY_DIRECTORY}
                        "${CMAKE_CURRENT_SOURCE_DIR}/Config"
                        "$<TARGET_FILE_DIR:ReviaDesktop>/Config"
                COMMAND ${CMAKE_COMMAND} -E ${REVIA_COPY_DIRECTORY}
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

# Deploy the remaining thread DLL independently of the optional desktop target.
# The compiler and C++ standard runtimes remain statically linked.
if(MINGW)
    target_link_options(R_E_V_I_A PRIVATE -static-libgcc -static-libstdc++)
    revia_deploy_mingw_runtime(R_E_V_I_A)
endif()

# Keep runtime configuration synchronized on every build, not only configure.
add_custom_command(TARGET R_E_V_I_A POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E ${REVIA_COPY_DIRECTORY}
                "${CMAKE_CURRENT_SOURCE_DIR}/Config"
                "$<TARGET_FILE_DIR:R_E_V_I_A>/Config"
        COMMAND ${CMAKE_COMMAND} -E ${REVIA_COPY_DIRECTORY}
                "${CMAKE_CURRENT_SOURCE_DIR}/Tools"
                "$<TARGET_FILE_DIR:R_E_V_I_A>/Tools"
        COMMENT "Synchronizing REVIA runtime configuration"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(ReviaSessionIdentityTests Tests/sessionIdentityTests.cpp
            Private/Runtime/sessionIdentity.cpp Private/Runtime/runtimeEvents.cpp)
    target_include_directories(ReviaSessionIdentityTests PRIVATE Public)
    add_test(NAME Revia.SessionIdentity COMMAND ReviaSessionIdentityTests)
    add_executable(ReviaCompanionTests Tests/companionTests.cpp Tests/Fixture/studioPackageTests.cpp)
    target_compile_definitions(ReviaCompanionTests PRIVATE REVIA_STUDIO_TEST_SUITE=RunCompanionTests)
    revia_link_foundation(ReviaCompanionTests)
    add_test(NAME Revia.CompanionIsolationMigration COMMAND ReviaCompanionTests)
    add_executable(ReviaCompanionAuthorityTests Tests/companionAuthorityTests.cpp Tests/Fixture/studioPackageTests.cpp)
    target_compile_definitions(ReviaCompanionAuthorityTests PRIVATE REVIA_STUDIO_TEST_SUITE=RunCompanionAuthorityTests)
    revia_link_foundation(ReviaCompanionAuthorityTests)
    add_test(NAME Revia.CompanionAuthority COMMAND ReviaCompanionAuthorityTests)
    add_executable(ReviaAgentWorkflowTests Tests/agentWorkflowTests.cpp Tests/Fixture/studioPackageTests.cpp)
    target_compile_definitions(ReviaAgentWorkflowTests PRIVATE REVIA_STUDIO_TEST_SUITE=RunAgentWorkflowTests)
    revia_link_foundation(ReviaAgentWorkflowTests)
    add_test(NAME Revia.AgentWorkflow COMMAND ReviaAgentWorkflowTests)
    function(revia_add_studio_check target source suite)
        add_executable(${target} ${source} Tests/Fixture/studioPackageTests.cpp)
        target_compile_definitions(${target} PRIVATE REVIA_STUDIO_TEST_SUITE=${suite})
        revia_link_foundation(${target})
        if(MINGW)
            target_compile_options(${target} PRIVATE -Wa,-mbig-obj)
        endif()
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 120)
    endfunction()
    revia_add_studio_check(ReviaSkillPackageTests Tests/skillPackageTests.cpp RunSkillPackageTests)
    revia_add_studio_check(ReviaLearningReviewTests Tests/learningReviewTests.cpp RunLearningReviewTests)
    revia_add_studio_check(ReviaDatasetExportTests Tests/datasetExportTests.cpp RunDatasetExportTests)
    revia_add_studio_check(ReviaCapabilityGapTests Tests/capabilityGapTests.cpp RunCapabilityGapTests)
    revia_add_studio_check(ReviaAudiencePrivacyTests Tests/audiencePrivacyTests.cpp RunAudiencePrivacyTests)
    revia_add_studio_check(ReviaAudienceInputTests Tests/audienceInputTests.cpp RunAudienceInputTests)
    revia_add_studio_check(ReviaSocialIdentityTests Tests/socialIdentityTests.cpp RunSocialIdentityTests)
    revia_add_studio_check(ReviaSpeakerRecognitionTests Tests/speakerRecognitionTests.cpp RunSpeakerRecognitionTests)
    revia_add_studio_check(ReviaSpeakerEnrollmentDialogueTests Tests/speakerEnrollmentDialogueTests.cpp RunSpeakerEnrollmentDialogueTests)
    revia_add_studio_check(ReviaSpeakerEnrollmentStoreTests Tests/speakerEnrollmentStoreTests.cpp RunSpeakerEnrollmentStoreTests)
    revia_add_studio_check(ReviaSpeakerEnrollmentRuntimeTests Tests/speakerEnrollmentRuntimeTests.cpp RunSpeakerEnrollmentRuntimeTests)
    revia_add_studio_check(ReviaRelationshipEvidenceTests Tests/relationshipEvidenceTests.cpp RunRelationshipEvidenceTests)
    revia_add_studio_check(ReviaConversationGuidanceTests Tests/conversationGuidanceTests.cpp RunConversationGuidanceTests)
    revia_add_studio_check(ReviaReplyFormatTests Tests/replyFormatTests.cpp RunReplyFormatTests)
    revia_add_studio_check(ReviaStructuredReplyTests Tests/structuredReplyTests.cpp RunStructuredReplyTests)
    revia_add_studio_check(ReviaCalculationVerifierTests Tests/calculationVerifierTests.cpp RunCalculationVerifierTests)
    revia_add_studio_check(ReviaCalculationGroundingTests Tests/calculationGroundingTests.cpp RunCalculationGroundingTests)
    revia_add_studio_check(ReviaSpeechAdmissionTests Tests/speechAdmissionTests.cpp RunSpeechAdmissionTests)
    revia_add_studio_check(ReviaPlaybackEnvelopeTests Tests/playbackEnvelopeTests.cpp RunPlaybackEnvelopeTests)
    revia_add_studio_check(ReviaPlaybackPresenceTests Tests/playbackPresenceTests.cpp RunPlaybackPresenceTests)
    revia_add_studio_check(ReviaPerformanceTests Tests/performanceTests.cpp RunPerformanceTests)
    revia_add_studio_check(ReviaSelfDevelopmentTests Tests/selfDevelopmentTests.cpp RunSelfDevelopmentTests)
    revia_add_studio_check(ReviaStudioRuntimeTests Tests/studioRuntimeTests.cpp RunStudioRuntimeTests)
    revia_add_studio_check(ReviaContentDigestTests Tests/contentDigestTests.cpp RunContentDigestTests)
    revia_add_studio_check(ReviaResponseLatencyTests Tests/responseLatencyTests.cpp RunResponseLatencyTests)
    revia_add_studio_check(ReviaResponseLatencyRuntimeTests Tests/responseLatencyRuntimeTests.cpp RunResponseLatencyRuntimeTests)
    revia_add_studio_check(ReviaInvestigationCheckTests Tests/investigationCheckTests.cpp RunInvestigationCheckTests)
    revia_add_studio_check(ReviaAnswerQualityTests Tests/answerQualityTests.cpp RunAnswerQualityTests)
    revia_add_studio_check(ReviaQualityFeedbackTests Tests/qualityFeedbackTests.cpp RunQualityFeedbackTests)
    revia_add_studio_check(ReviaMemoryRevisionTests Tests/memoryRevisionTests.cpp RunMemoryRevisionTests)
    revia_add_studio_check(ReviaMemoryCorrectionTests Tests/memoryCorrectionTests.cpp RunMemoryCorrectionTests)
    revia_add_studio_check(ReviaMemoryCorrectionAgentTests Tests/memoryCorrectionAgentTests.cpp RunMemoryCorrectionAgentTests)
    revia_add_studio_check(ReviaMemoryCorrectionRuntimeTests Tests/memoryCorrectionRuntimeTests.cpp RunMemoryCorrectionRuntimeTests)
    revia_add_studio_check(ReviaSemanticDuplicateTests Tests/semanticDuplicateTests.cpp RunSemanticDuplicateTests)
    revia_add_studio_check(ReviaSemanticDuplicateAgentTests Tests/semanticDuplicateAgentTests.cpp RunSemanticDuplicateAgentTests)
    revia_add_studio_check(ReviaConversationArchiveHybridTests Tests/conversationArchiveHybridTests.cpp RunConversationArchiveHybridTests)
    revia_add_studio_check(ReviaArchiveEmbeddingBackfillTests Tests/archiveEmbeddingBackfillTests.cpp RunArchiveEmbeddingBackfillTests)
    revia_add_studio_check(ReviaQualityFeedbackRuntimeTests Tests/qualityFeedbackRuntimeTests.cpp RunQualityFeedbackRuntimeTests)
    revia_add_studio_check(ReviaImprovementAgentTests Tests/improvementAgentTests.cpp RunImprovementAgentTests)
    revia_add_studio_check(ReviaContextFittingTests Tests/contextFittingTests.cpp RunContextFittingTests)
    set_tests_properties(ReviaContextFittingTests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    revia_add_studio_check(ReviaCloudModelTests Tests/cloudModelTests.cpp RunCloudModelTests)
    set_tests_properties(ReviaCloudModelTests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    revia_add_studio_check(ReviaRoutingProductionTests Tests/routingProductionTests.cpp RunRoutingProductionTests)
    revia_add_studio_check(ReviaProcessExecutionTests Tests/processExecutionTests.cpp RunProcessExecutionTests)
    revia_add_studio_check(ReviaProcessOwnerTests Tests/processOwnerTests.cpp RunProcessOwnerTests)
    revia_add_studio_check(ReviaOperatorUpgradeTests Tests/operatorUpgradeTests.cpp RunOperatorUpgradeTests)
    revia_add_studio_check(ReviaProcessCapabilityTests Tests/processCapabilityTests.cpp RunProcessCapabilityTests)
    revia_add_studio_check(ReviaCapabilityProjectionTests Tests/capabilityProjectionTests.cpp RunCapabilityProjectionTests)
    revia_add_studio_check(ReviaTaskAcceptanceTests Tests/taskAcceptanceTests.cpp RunTaskAcceptanceTests)
    revia_add_studio_check(ReviaMemoryScopeTests Tests/memoryScopeTests.cpp RunMemoryScopeTests)
    revia_add_studio_check(ReviaScopedMemoryQueueTests Tests/scopedMemoryQueueTests.cpp RunScopedMemoryQueueTests)
    revia_add_studio_check(ReviaRelationshipTests Tests/relationshipTests.cpp RunRelationshipTests)
    revia_add_studio_check(ReviaBrowserContractTests Tests/browserContractTests.cpp RunBrowserContractTests)
    revia_add_studio_check(ReviaVisionContextTests Tests/visionContextTests.cpp RunVisionContextTests)
    revia_add_studio_check(ReviaAgentToolBudgetTests Tests/agentToolBudgetTests.cpp RunAgentToolBudgetTests)
    revia_add_studio_check(ReviaAgentToolWorkerTests Tests/agentToolWorkerTests.cpp RunAgentToolWorkerTests)
    revia_add_studio_check(ReviaAgentToolActionTests Tests/agentToolActionTests.cpp RunAgentToolActionTests)
    revia_add_studio_check(ReviaToolLoopTests Tests/toolLoopTests.cpp RunToolLoopTests)
    # Chat delegation drives whole chat turns against a llama-server stand-in that holds its workers.
    revia_add_studio_check(ReviaDelegationTests Tests/delegationTests.cpp RunDelegationTests)
    # The MCP suite drives a real stdio server, built below, and reads Config/settings.json.
    revia_add_studio_check(ReviaMcpClientTests Tests/mcpClientTests.cpp RunMcpClientTests)
    revia_add_studio_check(ReviaLocalApiTests Tests/localApiTests.cpp RunLocalApiTests)
    revia_add_studio_check(ReviaInitiativeModeTests Tests/initiativeModeTests.cpp RunInitiativeModeTests)
    revia_add_studio_check(ReviaSavedFileObservationTests Tests/savedFileObservationTests.cpp RunSavedFileObservationTests)
    revia_add_studio_check(ReviaEditorAwarenessTests Tests/editorAwarenessTests.cpp RunEditorAwarenessTests)
    revia_add_studio_check(ReviaProactiveStateTests Tests/proactiveStateTests.cpp RunProactiveStateTests)
    revia_add_studio_check(ReviaSourceDiagnosticTests Tests/sourceDiagnosticTests.cpp RunSourceDiagnosticTests)
    revia_add_studio_check(ReviaSourceDiagnosticReplyTests Tests/sourceDiagnosticReplyTests.cpp RunSourceDiagnosticReplyTests)
    revia_add_studio_check(ReviaAttachmentTests Tests/attachmentTests.cpp RunAttachmentTests)
    target_compile_definitions(ReviaAttachmentTests PRIVATE REVIA_ATTACHMENT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Attachments")
    revia_add_studio_check(ReviaMediaSelectionTests Tests/mediaSelectionTests.cpp RunMediaSelectionTests)
    revia_add_studio_check(ReviaVideoSelectionTests Tests/videoSelectionTests.cpp RunVideoSelectionTests)
    revia_add_studio_check(ReviaConversationStyleDataTests Tests/conversationStyleDataTests.cpp RunConversationStyleDataTests)
    revia_add_studio_check(ReviaReminderIntentTests Tests/reminderIntentTests.cpp RunReminderIntentTests)
    find_package(Threads REQUIRED)
    add_executable(ReviaVideoPublicationLaneTests Tests/videoPublicationLaneTests.cpp)
    target_link_libraries(ReviaVideoPublicationLaneTests PRIVATE Threads::Threads)
    add_test(NAME Revia.VideoPublicationLane COMMAND ReviaVideoPublicationLaneTests)
    set_tests_properties(Revia.VideoPublicationLane PROPERTIES TIMEOUT 10)

    revia_add_studio_check(ReviaVideoFrameReplyTests Tests/videoFrameReplyTests.cpp RunVideoFrameReplyTests)
    revia_add_studio_check(ReviaGeneratedArtifactContinuityTests Tests/generatedArtifactContinuityTests.cpp RunGeneratedArtifactContinuityTests)
    revia_add_studio_check(ReviaGeneratedMediaRuntimeTests Tests/generatedMediaRuntimeTests.cpp RunGeneratedMediaRuntimeTests)
    add_executable(ReviaGeneratedVideoImportTests Tests/generatedVideoImportTests.cpp)
    target_compile_definitions(ReviaGeneratedVideoImportTests PRIVATE REVIA_GENERATED_VIDEO_IMPORT_STANDALONE)
    revia_link_foundation(ReviaGeneratedVideoImportTests)
    add_executable(ReviaVideoGeneratorTests Tests/videoGeneratorTests.cpp)
    revia_link_foundation(ReviaVideoGeneratorTests)
    # These two manual checks require the separately installed local decoder tools.
    add_executable(ReviaVideoCoverageTests Tests/videoCoverageTests.cpp)
    target_compile_definitions(ReviaVideoCoverageTests PRIVATE REVIA_VIDEO_COVERAGE_TEST_MAIN)
    revia_link_foundation(ReviaVideoCoverageTests)
    add_test(NAME ReviaVideoCoverageTests COMMAND ReviaVideoCoverageTests)
    set_tests_properties(ReviaVideoCoverageTests PROPERTIES TIMEOUT 30)
    add_executable(ReviaVideoDecoderTests Tests/videoDecoderTests.cpp)
    target_compile_definitions(ReviaVideoDecoderTests PRIVATE REVIA_VIDEO_DECODER_STANDALONE)
    revia_link_foundation(ReviaVideoDecoderTests)
    add_test(NAME ReviaVideoDecoderTests COMMAND ReviaVideoDecoderTests)
    set_tests_properties(ReviaVideoDecoderTests PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
    revia_add_studio_check(ReviaVideoAttachmentTests Tests/videoAttachmentTests.cpp RunVideoAttachmentTests)
    revia_add_studio_check(ReviaFileTranscriptionTests Tests/fileTranscriptionTests.cpp RunFileTranscriptionTests)
    add_executable(ReviaFileTranscriptionFixture Tests/fileTranscriptionTests.cpp)
    target_compile_definitions(ReviaFileTranscriptionFixture PRIVATE REVIA_FILE_TRANSCRIPTION_FIXTURE)
    target_include_directories(ReviaFileTranscriptionFixture PRIVATE Public Tests)
    target_link_libraries(ReviaFileTranscriptionFixture PRIVATE nlohmann_json::nlohmann_json)
    if(MINGW)
        target_link_options(ReviaFileTranscriptionFixture PRIVATE -static)
    endif()
    add_dependencies(ReviaFileTranscriptionTests ReviaFileTranscriptionFixture)
    revia_add_studio_check(ReviaImageTokenSettingsTests Tests/imageTokenSettingsTests.cpp RunImageTokenSettingsTests)
    revia_add_studio_check(ReviaPromptBatchSettingsTests Tests/promptBatchSettingsTests.cpp RunPromptBatchSettingsTests)
    set_tests_properties(ReviaLocalApiTests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(ReviaMcpClientTests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(ReviaTaskContractTests Tests/taskContractTests.cpp)
    revia_link_foundation(ReviaTaskContractTests)
    add_test(NAME Revia.TaskContracts COMMAND ReviaTaskContractTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/TaskContracts")
    set_tests_properties(Revia.TaskContracts PROPERTIES LABELS "FND-01;T-FND-01-A;T-FND-01-B" TIMEOUT 30)
    add_executable(ReviaTaskContractIntegrationTests Tests/taskContractIntegrationTests.cpp)
    revia_link_foundation(ReviaTaskContractIntegrationTests)
    add_test(NAME Revia.TaskContractActions COMMAND ReviaTaskContractIntegrationTests)
    set_tests_properties(Revia.TaskContractActions PROPERTIES LABELS "FND-01;T-FND-01-B" TIMEOUT 30)
    add_executable(ReviaTaskContractSessionTests Tests/taskContractSessionTests.cpp)
    revia_link_foundation(ReviaTaskContractSessionTests)
    add_test(NAME Revia.TaskContractSession COMMAND ReviaTaskContractSessionTests)
    set_tests_properties(Revia.TaskContractSession PROPERTIES LABELS "FND-01;FND-03;T-FND-01-A;T-FND-03-B" TIMEOUT 30)
    add_executable(ReviaEvaluationCampaignTests Tests/evaluationCampaignTests.cpp)
    target_compile_definitions(ReviaEvaluationCampaignTests PRIVATE REVIA_CAMPAIGN_STANDALONE
        REVIA_CAMPAIGN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Evaluation")
    revia_link_foundation(ReviaEvaluationCampaignTests)
    add_test(NAME Revia.EvaluationCampaignIdentity COMMAND ReviaEvaluationCampaignTests)
    set_tests_properties(Revia.EvaluationCampaignIdentity PROPERTIES LABELS "FND-02;T-FND-02-B" TIMEOUT 30)
    revia_add_studio_check(ReviaEvidenceJournalTests Tests/evidenceJournalTests.cpp RunEvidenceJournalTests)
    set_tests_properties(ReviaEvidenceJournalTests PROPERTIES LABELS "FND-03;T-FND-03-A;T-FND-03-B;T-FND-03-C" TIMEOUT 120)
    revia_add_studio_check(ReviaRuntimeEvidenceBridgeTests Tests/runtimeEvidenceBridgeTests.cpp RunRuntimeEvidenceBridgeTests)
    set_tests_properties(ReviaRuntimeEvidenceBridgeTests PROPERTIES LABELS "FND-03;T-FND-03-B" TIMEOUT 30)
    find_program(REVIA_POWERSHELL_EXECUTABLE NAMES powershell.exe pwsh)
    if(REVIA_POWERSHELL_EXECUTABLE)
        add_test(NAME Revia.EvaluationCampaignRunner COMMAND "${REVIA_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/evaluationCampaign.test.ps1")
        set_tests_properties(Revia.EvaluationCampaignRunner PROPERTIES LABELS "FND-02;T-FND-02-A" TIMEOUT 90)
        add_test(NAME Revia.CognitionCampaignReceipts COMMAND "${REVIA_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/cognitionLiveCampaign.test.ps1")
        set_tests_properties(Revia.CognitionCampaignReceipts PROPERTIES LABELS "COG-01;artifact-retention" TIMEOUT 90)
        if(WIN32)
            add_test(NAME Revia.EditorObservationScripts COMMAND "${REVIA_POWERSHELL_EXECUTABLE}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/editorObservationScripts.test.ps1")
            set_tests_properties(Revia.EditorObservationScripts PROPERTIES TIMEOUT 100)
        endif()
    endif()
    add_executable(ReviaCognitionEvaluationTests Tests/cognitionEvaluationTests.cpp)
    revia_link_foundation(ReviaCognitionEvaluationTests)
    add_test(NAME Revia.CognitionWrongAnswers COMMAND ReviaCognitionEvaluationTests A)
    add_test(NAME Revia.CognitionFrozenAccounting COMMAND ReviaCognitionEvaluationTests B "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Cognition")
    set_tests_properties(Revia.CognitionWrongAnswers PROPERTIES LABELS "COG-01;T-COG-01-A" TIMEOUT 30)
    set_tests_properties(Revia.CognitionFrozenAccounting PROPERTIES LABELS "COG-01;T-COG-01-B" TIMEOUT 30)
    revia_add_studio_check(ReviaCognitionLiveTests Tests/cognitionLiveTests.cpp RunCognitionLiveTests)
    set_tests_properties(ReviaCognitionLiveTests PROPERTIES LABELS "COG-01;artifact-retention" TIMEOUT 30)
    add_executable(ReviaObservedLocalModelTests Tests/observedLocalModelTests.cpp)
    revia_link_foundation(ReviaObservedLocalModelTests)
    add_test(NAME Revia.ObservedLocalModel COMMAND ReviaObservedLocalModelTests)
    add_executable(ReviaBrowserExecutionTests Tests/browserExecutionTests.cpp Tests/Fixture/studioPackageTests.cpp)
    target_compile_definitions(ReviaBrowserExecutionTests PRIVATE REVIA_STUDIO_TEST_SUITE=RunBrowserExecutionTests)
    revia_link_foundation(ReviaBrowserExecutionTests)
    add_executable(ReviaBrowserOwnerTests Tests/browserOwnerTests.cpp Tests/Fixture/studioPackageTests.cpp)
    target_compile_definitions(ReviaBrowserOwnerTests PRIVATE REVIA_STUDIO_TEST_SUITE=RunBrowserOwnerTests)
    revia_link_foundation(ReviaBrowserOwnerTests)

    find_program(REVIA_NODE_EXECUTABLE NAMES node node.exe)
    if(REVIA_NODE_EXECUTABLE)
        add_test(NAME Revia.InteractiveBrowser COMMAND "${REVIA_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Browser/interactiveNative.live.test.mjs" "$<TARGET_FILE:ReviaBrowserExecutionTests>")
        set_tests_properties(Revia.InteractiveBrowser PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME Revia.InteractiveBrowserOwner COMMAND "${REVIA_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Browser/interactiveNative.live.test.mjs" "$<TARGET_FILE:ReviaBrowserOwnerTests>")
        set_tests_properties(Revia.InteractiveBrowserOwner PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_executable(ReviaProcessFixture Tests/Fixture/processFixture.cpp)
    if(MINGW)
        target_link_options(ReviaProcessFixture PRIVATE -static)
    endif()
    add_executable(ReviaMcpServerFixture Tests/Fixture/mcpServerFixture.cpp)
    target_link_libraries(ReviaMcpServerFixture PRIVATE nlohmann_json::nlohmann_json)
    if(MINGW)
        target_link_options(ReviaMcpServerFixture PRIVATE -static)
    endif()
    add_dependencies(ReviaMcpClientTests ReviaMcpServerFixture)
    set_tests_properties(ReviaMcpClientTests PROPERTIES
        ENVIRONMENT "REVIA_MCP_SERVER_FIXTURE=$<TARGET_FILE:ReviaMcpServerFixture>")
    add_dependencies(ReviaProcessExecutionTests ReviaProcessFixture)
    add_dependencies(ReviaProcessOwnerTests ReviaProcessFixture)
    add_dependencies(ReviaAgentToolActionTests ReviaProcessFixture)
    add_dependencies(ReviaStudioRuntimeTests ReviaProcessFixture)

    add_executable(ReviaDrawingIntentTests Tests/drawingRequestPolicyTests.cpp Private/Visual/drawingRequestPolicy.cpp)
    target_include_directories(ReviaDrawingIntentTests PRIVATE Public)
    add_test(NAME Revia.DrawingIntent COMMAND ReviaDrawingIntentTests)
    add_executable(ReviaVideoRequestPolicyTests Tests/videoRequestPolicyTests.cpp Private/Visual/videoRequestPolicy.cpp)
    target_include_directories(ReviaVideoRequestPolicyTests PRIVATE Public)
    add_test(NAME Revia.VideoRequestPolicy COMMAND ReviaVideoRequestPolicyTests)
    if(WIN32)
        add_executable(ReviaImageArtifactTests Tests/imageArtifactTests.cpp
            Private/Visual/imageArtifact.cpp Private/Audit/contentDigest.cpp)
        target_include_directories(ReviaImageArtifactTests PRIVATE Public)
        target_compile_definitions(ReviaImageArtifactTests PRIVATE NOMINMAX)
        target_link_libraries(ReviaImageArtifactTests PRIVATE gdiplus shlwapi)
        add_test(NAME Revia.ImageArtifact COMMAND ReviaImageArtifactTests)

        function(revia_add_image_check target source)
            add_executable(${target} ${source} Private/Visual/imageGenerator.cpp
                Private/Visual/imageArtifact.cpp Private/Audit/contentDigest.cpp
                Private/Core/localApiKey.cpp Private/Core/logger.cpp)
            target_include_directories(${target} PRIVATE Public)
            target_compile_definitions(${target} PRIVATE NOMINMAX)
            target_link_libraries(${target} PRIVATE httplib::httplib nlohmann_json::nlohmann_json gdiplus shlwapi bcrypt ws2_32 crypt32)
        endfunction()
        revia_add_image_check(ReviaImageGeneratorTests Tests/imageGeneratorTests.cpp)
        revia_add_image_check(ReviaImageGenerationLive Tests/imageGeneration.live.cpp)
        add_executable(ReviaImageActionTests Tests/imageActionTests.cpp)
        revia_link_foundation(ReviaImageActionTests)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME Revia.ImageGenerator COMMAND ReviaImageGeneratorTests
                "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixture/imageWorkerFixture.py")
            set_tests_properties(Revia.ImageGenerator PROPERTIES TIMEOUT 45)
            add_test(NAME Revia.ImageActions COMMAND ReviaImageActionTests
                "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixture/imageWorkerFixture.py")
            set_tests_properties(Revia.ImageActions PROPERTIES TIMEOUT 45)
        endif()
    endif()
    file(GLOB_RECURSE REVIA_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp"
    )
    # The disposable Windows fixture is its own GUI program with its own entry point.
    # It lives under Tests/ because that is what it is for, but it must not be linked
    # into the test binary.
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/Fixture/")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/(sessionIdentityTests|companionRuntimeTests)\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/studioPanelTests\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/videoStoreLive\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/(generatedVideoImportTests|videoGeneratorTests|videoRequestPolicyTests|videoPublicationLaneTests)\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/observedLocalModelTests\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/(memoryRerankerTests|memoryRetrievalPipelineTests|memoryNliWorkerTests)\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/(taskContractTests|taskContractIntegrationTests|taskContractSessionTests|evaluationCampaignTests|evidenceJournalTests|cognitionEvaluationTests)\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/developmentStudioLiveTests\\.cpp$")
    list(FILTER REVIA_TEST_SOURCES EXCLUDE REGEX "/Tests/(drawingRequestPolicyTests|imageArtifactTests|imageGeneratorTests|imageActionTests|imageGeneration\\.live)\\.cpp$")
    add_executable(ReviaCompanionRuntimeTests Tests/companionRuntimeTests.cpp)
    revia_link_foundation(ReviaCompanionRuntimeTests)
    add_test(NAME Revia.CompanionRuntime COMMAND ReviaCompanionRuntimeTests)
    add_executable(ReviaTests ${REVIA_TEST_SOURCES})
    add_executable(ReviaStudioLive Tests/studioLive.cpp Tests/Fixture/studioLiveMain.cpp)
    add_executable(ReviaAnswerQualityLive Tools/Quality/answerQualityLive.cpp)
    revia_link_foundation(ReviaAnswerQualityLive)
    # Opt-in, disposable-runtime qualification; never launches a model in CTest.
    add_executable(ReviaCompanionAwarenessLive Tools/Quality/companionAwarenessLive.cpp)
    revia_link_foundation(ReviaCompanionAwarenessLive)
    add_executable(ReviaCompanionVisionLive Tools/Quality/companionVisionLive.cpp)
    revia_link_foundation(ReviaCompanionVisionLive)
    add_executable(ReviaVideoStoreLive Tests/videoStoreLive.cpp)
    revia_link_foundation(ReviaVideoStoreLive)
    add_executable(ReviaCompanionImageLive Tools/Quality/companionImageLive.cpp)
    revia_link_foundation(ReviaCompanionImageLive)
    add_executable(ReviaMainVisionSetting Tools/Quality/mainVisionSetting.cpp)
    revia_link_foundation(ReviaMainVisionSetting)
    if(MINGW)
        target_compile_options(ReviaCompanionAwarenessLive PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaCompanionAwarenessLive PRIVATE -static-libgcc -static-libstdc++)
        target_compile_options(ReviaCompanionVisionLive PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaCompanionVisionLive PRIVATE -static-libgcc -static-libstdc++)
        target_compile_options(ReviaCompanionImageLive PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaCompanionImageLive PRIVATE -static-libgcc -static-libstdc++)
        target_compile_options(ReviaMainVisionSetting PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaMainVisionSetting PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if(MINGW)
        target_compile_options(ReviaAnswerQualityLive PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaAnswerQualityLive PRIVATE -static-libgcc -static-libstdc++)
    endif()
    # The answer-quality gate needs a running model, so it is built here but never
    # registered with CTest. docs/EVALUATION_GATE.md describes how it is run.
    add_executable(ReviaAnswerGateLive Tools/Quality/answerGateLive.cpp)
    revia_link_foundation(ReviaAnswerGateLive)
    if(MINGW)
        target_compile_options(ReviaAnswerGateLive PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaAnswerGateLive PRIVATE -static-libgcc -static-libstdc++)
    endif()
    target_include_directories(ReviaStudioLive PRIVATE Tests)
    revia_link_foundation(ReviaStudioLive)
    if(MINGW)
        target_compile_options(ReviaStudioLive PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaStudioLive PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_executable(ReviaDevelopmentStudioTests Tests/developmentStudioLiveTests.cpp)
    target_include_directories(ReviaDevelopmentStudioTests PRIVATE Tests)
    revia_link_foundation(ReviaDevelopmentStudioTests)
    if(MINGW)
        target_compile_options(ReviaDevelopmentStudioTests PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaDevelopmentStudioTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME Revia.DevelopmentStudioAdmission COMMAND ReviaDevelopmentStudioTests)
    set_tests_properties(Revia.DevelopmentStudioAdmission PROPERTIES TIMEOUT 30)
    if(MINGW)
        # Link wrappers coordinate real SQLite calls without a production test seam.
        add_executable(ReviaArchiveWriterTests
                Tests/Fixture/archiveWriterTests.cpp
                Private/Memory/conversationArchive.cpp
                Private/Memory/hybridRanking.cpp
                Private/Memory/memoryScope.cpp
                Private/Memory/sensitiveContent.cpp
                Private/Memory/temporalQuery.cpp)
        target_include_directories(ReviaArchiveWriterTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Public")
        target_link_libraries(ReviaArchiveWriterTests PRIVATE sqlite3 nlohmann_json::nlohmann_json)
        target_compile_options(ReviaArchiveWriterTests PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(ReviaArchiveWriterTests PRIVATE -static-libgcc -static-libstdc++
                -Wl,--wrap=sqlite3_prepare_v2 -Wl,--wrap=sqlite3_step)
        add_test(NAME Revia.ArchiveWriter COMMAND ReviaArchiveWriterTests)
        set_tests_properties(Revia.ArchiveWriter PROPERTIES TIMEOUT 20)
    endif()
    add_executable(ReviaWebGuestTests Tests/Fixture/webGuestTests.cpp)
    target_include_directories(ReviaWebGuestTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Tests")
    revia_link_foundation(ReviaWebGuestTests)
    if(MINGW)
        target_link_options(ReviaWebGuestTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME revia_web_guest COMMAND ReviaWebGuestTests)
    set_tests_properties(revia_web_guest PROPERTIES TIMEOUT 60)
    add_executable(ReviaWebGuestPrivacyTests Tests/Fixture/webGuestPrivacyTests.cpp)
    target_include_directories(ReviaWebGuestPrivacyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Tests")
    revia_link_foundation(ReviaWebGuestPrivacyTests)
    if(MINGW)
        # Over 32k sections; the default COFF object format stops at 32767.
        target_compile_options(ReviaWebGuestPrivacyTests PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaWebGuestPrivacyTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME revia_web_guest_privacy COMMAND ReviaWebGuestPrivacyTests)
    set_tests_properties(revia_web_guest_privacy PROPERTIES TIMEOUT 90)
    add_executable(ReviaWebGuestHost Tests/Fixture/webGuestHost.cpp)
    revia_link_foundation(ReviaWebGuestHost)
    if(MINGW)
        target_link_options(ReviaWebGuestHost PRIVATE -static-libgcc -static-libstdc++)
    endif()
    # Split test suites include testSupport.h from the Tests directory.
    target_include_directories(ReviaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Tests")
    target_compile_definitions(ReviaTests PRIVATE REVIA_ATTACHMENT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Attachments")
    revia_link_foundation(ReviaTests)
    if(MINGW)
        target_compile_options(ReviaTests PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if(MSVC)
        target_compile_options(ReviaTests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(ReviaTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    # The disposable fixture the native desktop tests drive. Windows only, and built
    # only when tests are enabled: nothing in the shipping runtime depends on it.
    if(WIN32)
        add_executable(ReviaCliLoaderGate Tests/Fixture/cliLoaderGate.cpp)
        if(MINGW)
            target_compile_options(ReviaCliLoaderGate PRIVATE -municode)
            target_link_options(ReviaCliLoaderGate PRIVATE -static -municode)
        endif()
        add_test(NAME Revia.CliLoaderDeployment COMMAND ReviaCliLoaderGate "$<TARGET_FILE:R_E_V_I_A>")
        set_tests_properties(Revia.CliLoaderDeployment PROPERTIES TIMEOUT 40)
        add_executable(ReviaDesktopFixture WIN32
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixture/desktopFixture.cpp")
        target_link_libraries(ReviaDesktopFixture PRIVATE comctl32 user32 gdi32)
        if(MINGW)
            # -municode selects the wide entry point. Without it MinGW's startup looks
            # for WinMain and the fixture's wWinMain never links.
            target_compile_options(ReviaDesktopFixture PRIVATE -municode)
            target_link_options(ReviaDesktopFixture PRIVATE -static-libgcc -static-libstdc++ -municode)
        endif()
        # The native tests launch it by path, so it has to exist before they run.
        add_dependencies(ReviaTests ReviaDesktopFixture)
    endif()

    add_test(NAME Revia.Foundation COMMAND ReviaTests)
    add_executable(ReviaOutputBoundaryTests
            Tests/Fixture/outputBoundaryTests.cpp
            Tests/reviewDeliveryTests.cpp
            Tests/utf8GuidanceTests.cpp
            Tests/utf8PersistenceTests.cpp)
    target_include_directories(ReviaOutputBoundaryTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Tests")
    revia_link_foundation(ReviaOutputBoundaryTests)
    if(MINGW)
        target_compile_options(ReviaOutputBoundaryTests PRIVATE -Wa,-mbig-obj)
        target_link_options(ReviaOutputBoundaryTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME Revia.OutputBoundaries COMMAND ReviaOutputBoundaryTests)
    set_tests_properties(Revia.OutputBoundaries PROPERTIES TIMEOUT 90)
    # The only deterministic owner suite the default run does not reach. It
    # disables periodic saving, so it must own its process to prove the final
    # save alone persists identity. RunNativeDesktopTests stays unregistered on
    # purpose: it synthesizes real keyboard and mouse input.
    add_test(NAME Revia.IdentityFinalSave COMMAND ReviaTests --identity-final-save)
    # A reentrant command lock must fail on a bounded timeout instead of hanging CI.
    add_test(NAME Revia.OperatorSession COMMAND ReviaTests --operator-session)
    set_tests_properties(Revia.OperatorSession PROPERTIES TIMEOUT 15)
    if(WIN32)
        # Its own process: it types into a console, and a console belongs to a process.
        add_test(NAME Revia.TerminalInput COMMAND ReviaTests --terminal-input)
        set_tests_properties(Revia.TerminalInput PROPERTIES TIMEOUT 30)
    endif()
    if(WIN32)
        find_program(REVIA_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME Revia.BuildDrivers COMMAND "${REVIA_POWERSHELL_EXECUTABLE}"
                -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/buildDrivers.test.ps1"
                -EvidenceDirectory "${CMAKE_CURRENT_BINARY_DIR}/BuildDriverFixtures")
        set_tests_properties(Revia.BuildDrivers PROPERTIES TIMEOUT 90)
    endif()
    find_program(REVIA_NODE_EXECUTABLE NAMES node node.exe)
    if(REVIA_NODE_EXECUTABLE)
        if(WIN32)
            add_test(
                    NAME Revia.SetupScriptPolicy
                    COMMAND "${REVIA_NODE_EXECUTABLE}" --test
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/setupScripts.test.mjs"
            )
        endif()
        add_test(
                NAME Revia.BrowserWorkerPolicy
                COMMAND "${REVIA_NODE_EXECUTABLE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Browser/browserHost.test.mjs"
        )
        add_test(
                NAME Revia.DiscordVoiceAdapter
                COMMAND "${REVIA_NODE_EXECUTABLE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Presence/DiscordVoice/adapter.test.mjs"
        )
        add_test(
                NAME Revia.Live2DBridge
                COMMAND "${REVIA_NODE_EXECUTABLE}" --test
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/live2dBridge.test.mjs"
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/live2dRenderer.live.test.mjs"
        )
        set_tests_properties(Revia.Live2DBridge PROPERTIES TIMEOUT 30)
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_executable(ReviaMemoryRerankerTests Tests/memoryRerankerTests.cpp)
        revia_link_foundation(ReviaMemoryRerankerTests)
        add_test(NAME Revia.MemoryReranker COMMAND ReviaMemoryRerankerTests
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixture/memoryRerankerFixture.py")
        set_tests_properties(Revia.MemoryReranker PROPERTIES TIMEOUT 45)
        add_executable(ReviaMemoryNliWorkerTests Tests/memoryNliWorkerTests.cpp)
        revia_link_foundation(ReviaMemoryNliWorkerTests)
        add_test(NAME Revia.MemoryNliWorker COMMAND ReviaMemoryNliWorkerTests
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixture/memoryNliFixture.py")
        set_tests_properties(Revia.MemoryNliWorker PROPERTIES TIMEOUT 45)
        add_executable(ReviaMemoryRetrievalPipelineTests Tests/memoryRetrievalPipelineTests.cpp)
        revia_link_foundation(ReviaMemoryRetrievalPipelineTests)
        add_test(NAME Revia.MemoryRetrievalPipeline COMMAND ReviaMemoryRetrievalPipelineTests
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixture/memoryRerankerFixture.py")
        set_tests_properties(Revia.MemoryRetrievalPipeline PROPERTIES TIMEOUT 60)
        add_test(NAME Revia.MemoryRerankerWorker COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Workers" -p test_memory_reranker.py)
        add_test(NAME Revia.MemoryNliPythonWorker COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Workers" -p test_memory_nli.py)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy"
            RESULT_VARIABLE REVIA_RETRIEVAL_NUMPY OUTPUT_QUIET ERROR_QUIET TIMEOUT 10)
        if(REVIA_RETRIEVAL_NUMPY EQUAL 0)
            add_test(NAME Revia.RetrievalBenchmarkContracts COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality/retrievalBenchmarkTests.py")
        endif()
        add_test(NAME Revia.GuestPreparation COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/guestSandbox.test.py")
        add_test(NAME Revia.ContextBenchmarkContract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/contextBenchmark.test.py")
        add_test(NAME Revia.AnswerGateSessionContract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality/answerGateSessionTests.py")
        add_test(NAME Revia.NliDedupBenchmarkContract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/nliDedupBenchmark.test.py")
        add_test(
                NAME Revia.QwenTtsServicePolicy
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/qwenTtsService.test.py"
        )
        add_test(
                NAME Revia.ImageServicePolicy
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/reviaImageService.test.py"
        )
        add_test(NAME Revia.VideoServicePolicy COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/reviaVideoWorker.test.py")
        add_test(NAME Revia.TrainingWorkerPolicy COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/reviaTrainingWorker.test.py")
        add_test(NAME Revia.TrainingAssetInstaller COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/trainingAssetInstaller.test.py")
        add_test(NAME Revia.AdapterExportPolicy COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/reviaAdapterExport.test.py")
        add_test(NAME Revia.AdapterEvaluationPolicy COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/reviaAdapterEvaluation.test.py")
    endif()
    if(TARGET ReviaDesktop)
        add_executable(ReviaStudioPanelTests Tests/studioPanelTests.cpp
                Desktop/learningStudioPanel.cpp Desktop/developmentStudioPanel.cpp Desktop/audienceStudioPanel.cpp)
        target_compile_definitions(ReviaStudioPanelTests PRIVATE REVIA_STUDIO_PANEL_STANDALONE)
        target_include_directories(ReviaStudioPanelTests PRIVATE Desktop Tests)
        revia_link_foundation(ReviaStudioPanelTests)
        target_link_libraries(ReviaStudioPanelTests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
        if(MINGW)
            target_link_options(ReviaStudioPanelTests PRIVATE -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME Revia.StudioPanels COMMAND ReviaStudioPanelTests)
        set_tests_properties(Revia.StudioPanels PROPERTIES TIMEOUT 30
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
        get_target_property(REVIA_DESKTOP_TEST_SOURCES ReviaDesktop SOURCES)
        list(FILTER REVIA_DESKTOP_TEST_SOURCES EXCLUDE REGEX "/Desktop/main\\.cpp$")
        # Every desktop Qt suite is a --suite=<name> mode of this one binary, so a suite costs no
        # extra executable or link. Each suite appends its source under its own marker.
        set(REVIA_DESKTOP_SUITE_SOURCES Tests/Fixture/desktopStopTests.cpp)
        list(APPEND REVIA_DESKTOP_SUITE_SOURCES Tests/Fixture/videoPreviewTests.cpp)
        # [suite:settingsBackend] (B1: the Settings dialog's backend over a real session)
        list(APPEND REVIA_DESKTOP_SUITE_SOURCES Tests/Fixture/settingsBackendTests.cpp)

        # [suite:theme]
        list(APPEND REVIA_DESKTOP_SUITE_SOURCES Tests/Fixture/themeTests.cpp)

        # [suite:settings]
        list(APPEND REVIA_DESKTOP_SUITE_SOURCES Tests/Fixture/settingsDialogTests.cpp)

        # [suite:shell]
        list(APPEND REVIA_DESKTOP_SUITE_SOURCES Tests/Fixture/shellWidgetTests.cpp)

        add_executable(ReviaDesktopStopTests ${REVIA_DESKTOP_SUITE_SOURCES} ${REVIA_DESKTOP_TEST_SOURCES})
        set_target_properties(ReviaDesktopStopTests PROPERTIES AUTOUIC ON AUTORCC ON)
        target_include_directories(ReviaDesktopStopTests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/Desktop" "${CMAKE_CURRENT_SOURCE_DIR}/Tests")
        revia_link_foundation(ReviaDesktopStopTests)
        target_link_libraries(ReviaDesktopStopTests PRIVATE
                Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::SvgWidgets)
        if(MINGW)
            target_link_options(ReviaDesktopStopTests PRIVATE -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME Revia.DesktopStopPolicy COMMAND ReviaDesktopStopTests)
        add_test(NAME Revia.VideoPreview COMMAND ReviaDesktopStopTests --suite=video-preview)
        set_tests_properties(Revia.VideoPreview PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME Revia.Theme COMMAND ReviaDesktopStopTests --suite=theme)
        add_test(NAME Revia.SettingsDialog COMMAND ReviaDesktopStopTests --suite=settings)
        add_test(NAME Revia.ShellWidgets COMMAND ReviaDesktopStopTests --suite=shell)
        add_test(NAME Revia.SettingsBackend COMMAND ReviaDesktopStopTests --suite=settingsBackend)
        # REVIA_THEME=light keeps every capture the same whatever the runner's Windows colour setting.
        set_tests_properties(Revia.DesktopStopPolicy Revia.VideoPreview Revia.Theme Revia.SettingsDialog Revia.ShellWidgets Revia.SettingsBackend PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>;REVIA_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/TestLogs;REVIA_THEME=light"
                TIMEOUT 30)
        add_executable(ReviaSvgRendererTests
                Tests/Fixture/svgRendererTests.cpp
                Private/Visual/svgCanvas.cpp
                Private/Visual/svgValidation.cpp)
        target_include_directories(ReviaSvgRendererTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Public")
        target_link_libraries(ReviaSvgRendererTests PRIVATE
                Qt6::Core Qt6::Gui Qt6::Svg expat::expat httplib::httplib)
        if(MINGW)
            target_compile_options(ReviaSvgRendererTests PRIVATE -Wa,-mbig-obj)
        endif()
        add_test(NAME Revia.SvgRenderer COMMAND ReviaSvgRendererTests)
        # windeployqt deploys the desktop's Windows platform plugin, not offscreen.
        # Resolve the test plugin from the same imported Qt kit explicitly.
        set_tests_properties(Revia.SvgRenderer PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
                TIMEOUT 15)
        add_test(NAME Revia.DesktopSmoke COMMAND ReviaDesktop --ui-smoke-test)
        set_tests_properties(Revia.DesktopSmoke PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=windows"
                TIMEOUT 15
        )
        set_property(TEST Revia.DesktopSmoke APPEND PROPERTY
                ENVIRONMENT "REVIA_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/TestLogs")
    endif()
    # Tests run from the build output, which is also the owner's runtime root. Without
    # this their fixture goals and smoke-test sessions were appended to the real
    # Logs/revia.log and session ledger, where they read as things Revia did.
    set_property(TEST revia_web_guest revia_web_guest_privacy Revia.Foundation
            Revia.IdentityFinalSave Revia.OperatorSession APPEND PROPERTY
            ENVIRONMENT "REVIA_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/TestLogs")
endif()
