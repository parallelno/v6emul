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

# ── Testing ────────────────────────────────────────────────────────────

enable_testing()
add_subdirectory(tests)
