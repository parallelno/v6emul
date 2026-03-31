add_library(v6utils STATIC
    src/json_utils.cpp
    src/str_utils.cpp
    src/utils.cpp
    src/args_parser.cpp
)

target_include_directories(v6utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(v6utils
    PUBLIC
        nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(v6utils PRIVATE user32)
endif()
