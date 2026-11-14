cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(terminal VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_terminal
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_terminal
)

add_library(HuxerUI::Terminal ALIAS huxerui_terminal)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_terminal PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

enable_language(C)
include(FetchContent)
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()
FetchContent_Declare(terminal_vterm
        URL "https://deb.debian.org/debian/pool/main/libv/libvterm/libvterm_0.3.3.orig.tar.gz"
            "https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz"
        URL_HASH SHA256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
        SOURCE_SUBDIR terminal-no-cmake
)
FetchContent_Declare(terminal_utf8proc
        URL "https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.10.0"
        URL_HASH SHA256=6f4f1b639daa6dca9f80bc5db1233e9cbaa31a67790887106160b33ef743f136
        SOURCE_SUBDIR terminal-no-cmake
)
FetchContent_MakeAvailable(terminal_vterm terminal_utf8proc)
include(cmake/LibVTermScrollback.cmake)

add_library(huxerui_terminal_dependencies OBJECT
        "${terminal_vterm_SOURCE_DIR}/src/encoding.c"
        "${terminal_vterm_SOURCE_DIR}/src/keyboard.c"
        "${terminal_vterm_SOURCE_DIR}/src/mouse.c"
        "${terminal_vterm_SOURCE_DIR}/src/parser.c"
        "${terminal_vterm_SOURCE_DIR}/src/pen.c"
        "${terminal_vterm_patched_dir}/screen.c"
        "${terminal_vterm_patched_dir}/state.c"
        "${terminal_vterm_SOURCE_DIR}/src/unicode.c"
        "${terminal_vterm_SOURCE_DIR}/src/vterm.c"
        "${terminal_utf8proc_SOURCE_DIR}/utf8proc.c"
)
set_target_properties(huxerui_terminal_dependencies PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)
target_include_directories(huxerui_terminal_dependencies PRIVATE
        "${terminal_vterm_patched_dir}" "${terminal_vterm_SOURCE_DIR}/include" "${terminal_vterm_SOURCE_DIR}/src")
target_compile_definitions(huxerui_terminal_dependencies PRIVATE UTF8PROC_STATIC)
target_sources(huxerui_terminal PRIVATE $<TARGET_OBJECTS:huxerui_terminal_dependencies>)
target_include_directories(huxerui_terminal PRIVATE
        "${terminal_vterm_patched_dir}" "${terminal_vterm_SOURCE_DIR}/include" "${terminal_utf8proc_SOURCE_DIR}")
target_compile_definitions(huxerui_terminal PRIVATE UTF8PROC_STATIC)

install(FILES "${terminal_vterm_SOURCE_DIR}/LICENSE"
        DESTINATION share/licenses/Lib-Terminal RENAME libvterm-LICENSE)
install(FILES "${terminal_utf8proc_SOURCE_DIR}/LICENSE.md"
        DESTINATION share/licenses/Lib-Terminal RENAME utf8proc-LICENSE.md)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(huxerui_terminal PRIVATE util)
endif ()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(HUXERUI_TERMINAL_BUILD_TESTS "Build Terminal session and UI tests" OFF)
    if (HUXERUI_TERMINAL_BUILD_TESTS)
        find_package(HuxerUI CONFIG REQUIRED COMPONENTS static testing)
        enable_testing()
        add_executable(terminal_tests tests/terminal.cpp)
        target_link_libraries(terminal_tests PRIVATE HuxerUI::Terminal HuxerUI::testing)
        huxerui_enable_codegen(terminal_tests)
        add_test(NAME terminal.ui COMMAND terminal_tests)
    endif ()
endif ()
