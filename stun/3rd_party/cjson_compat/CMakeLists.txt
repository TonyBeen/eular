cmake_minimum_required(VERSION 3.10)

project(cjson_compat C)

add_library(cjson_compat STATIC
    src/cJSON.c
)

set_target_properties(cjson_compat PROPERTIES
    OUTPUT_NAME cjson
)

target_include_directories(cjson_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
