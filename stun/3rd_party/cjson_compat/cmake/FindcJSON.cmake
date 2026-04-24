# cJSON finder for local cjson_compat target.
# Exposes cJSON target expected by mosquitto.

if(TARGET cjson_compat)
    get_target_property(_cjson_includes cjson_compat INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _cjson_includes)
        set(_cjson_includes "${CMAKE_CURRENT_LIST_DIR}/../include")
    endif()

    if(NOT TARGET cJSON)
        add_library(cJSON INTERFACE IMPORTED)
        target_link_libraries(cJSON INTERFACE cjson_compat)
        target_include_directories(cJSON INTERFACE ${_cjson_includes})
    endif()

    set(CJSON_INCLUDE_DIR ${_cjson_includes})
    set(CJSON_INCLUDE_DIRS ${_cjson_includes})
    set(CJSON_LIBRARY cJSON)
    set(CJSON_LIBRARIES cJSON)
    set(cJSON_FOUND TRUE)
    set(CJSON_FOUND TRUE)
    return()
endif()

set(cJSON_FOUND FALSE)
set(CJSON_FOUND FALSE)
