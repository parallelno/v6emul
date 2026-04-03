cmake_minimum_required(VERSION 3.21)
project(v6emul VERSION 0.1.0 LANGUAGES CXX)

# Prevent in-source build
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Create a build directory.")
endif()

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(V6EMUL_VERSION_STRING "$ENV{V6EMUL_VERSION}")

if(NOT V6EMUL_VERSION_STRING)
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --date=format:%Y.%m.%d --format=%cd
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE V6EMUL_GIT_DATE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE V6EMUL_GIT_DATE_RESULT
        )
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE V6EMUL_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE V6EMUL_GIT_HASH_RESULT
        )

        if(V6EMUL_GIT_DATE_RESULT EQUAL 0 AND V6EMUL_GIT_HASH_RESULT EQUAL 0)
            set(V6EMUL_VERSION_STRING "${V6EMUL_GIT_DATE}-${V6EMUL_GIT_HASH}")
        endif()
    endif()
endif()

if(NOT V6EMUL_VERSION_STRING)
    set(V6EMUL_VERSION_STRING "${PROJECT_VERSION}")
endif()

message(STATUS "v6emul version: ${V6EMUL_VERSION_STRING}")

configure_file(
    ${CMAKE_SOURCE_DIR}/app/version.h.in
    ${CMAKE_BINARY_DIR}/generated/v6emul_version.h
    @ONLY
)

# Enable parallel compilation for MSVC
if(MSVC)
    add_compile_options(/MP)
endif()

# ── External dependencies ──────────────────────────────────────────────

include(FetchContent)

# nlohmann::json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# LuaJIT
FetchContent_Declare(
    LuaJIT
    GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
    GIT_TAG v2.1.0-beta3
)
FetchContent_MakeAvailable(LuaJIT)
FetchContent_GetProperties(LuaJIT)

include(ExternalProject)

if(WIN32)
    if(MINGW)
        ExternalProject_Add(
            luajit_build
            SOURCE_DIR ${luajit_SOURCE_DIR}
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ${CMAKE_COMMAND} -E chdir ${luajit_SOURCE_DIR} mingw32-make BUILDMODE=static "TARGET_CFLAGS=-m64"
            INSTALL_COMMAND ""
            BUILD_IN_SOURCE 1
        )
    else()
        ExternalProject_Add(
            luajit_build
            SOURCE_DIR ${luajit_SOURCE_DIR}
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ${CMAKE_COMMAND} -E chdir ${luajit_SOURCE_DIR}/src msvcbuild.bat static
            INSTALL_COMMAND ""
            BUILD_IN_SOURCE 1
        )
    endif()
else()
    ExternalProject_Add(
        luajit_build
        SOURCE_DIR ${luajit_SOURCE_DIR}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${CMAKE_COMMAND} -E chdir ${luajit_SOURCE_DIR} make BUILDMODE=static
        INSTALL_COMMAND ""
        BUILD_IN_SOURCE 1
    )
endif()

# LuaJIT static lib path
if(WIN32)
    if(MINGW)
        set(LUAJIT_STATIC_LIB "${luajit_SOURCE_DIR}/src/libluajit.a")
    else()
        set(LUAJIT_STATIC_LIB "${luajit_SOURCE_DIR}/src/lua51.lib")
    endif()
else()
    set(LUAJIT_STATIC_LIB "${luajit_SOURCE_DIR}/src/libluajit.a")
endif()

add_library(luajit STATIC IMPORTED)
set_target_properties(luajit PROPERTIES
    IMPORTED_LOCATION ${LUAJIT_STATIC_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${luajit_SOURCE_DIR}/src
)

# ── Sub-projects ───────────────────────────────────────────────────────

add_subdirectory(libs/v6utils)
add_subdirectory(libs/v6core)
add_subdirectory(libs/v6ipc)
add_subdirectory(app)
add_subdirectory(tools/test_client)

# ── Testing ────────────────────────────────────────────────────────────

enable_testing()
add_subdirectory(tests)
