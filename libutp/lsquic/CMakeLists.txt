set(UTP_LSQUIC_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

find_path(LSQUIC_INCLUDE_DIR
    NAMES lsquic/lsquic.h
    PATHS /usr/local/include /usr/include
)
find_library(LSQUIC_LIBRARY
    NAMES lsquic
    PATHS /usr/local/lib /usr/lib
)
find_path(LSQUIC_SOURCE_ROOT
    NAMES bin/prog.c src/liblsquic/lsquic_hash.h
    PATHS /home/eular/Github/lsquic
)
find_path(LSQUIC_BUILD_BIN_INCLUDE_DIR
    NAMES test_config.h
    PATHS
        /home/eular/Github/lsquic/build/bin
        ${LSQUIC_SOURCE_ROOT}/build/bin
)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

if(NOT LSQUIC_INCLUDE_DIR OR NOT LSQUIC_LIBRARY OR NOT LSQUIC_SOURCE_ROOT OR NOT LSQUIC_BUILD_BIN_INCLUDE_DIR)
    message(WARNING "lsquic library, headers, or local source helpers not found, skipping lsquic echo examples")
    set(UTP_BUILD_LSQUIC_ECHO OFF CACHE BOOL "Build lsquic echo comparison examples" FORCE)
    return()
endif()

set(LSQUIC_BIN_DIR "${LSQUIC_SOURCE_ROOT}/bin")
set(LSQUIC_INTERNAL_INCLUDE_DIR "${LSQUIC_SOURCE_ROOT}/src/liblsquic")

add_executable(lsquic_echo_server
    "${LSQUIC_BIN_DIR}/prog.c"
    "${LSQUIC_BIN_DIR}/test_common.c"
    test_cert.c
    lsquic_echo_server.c
)

add_executable(lsquic_echo_client
    "${LSQUIC_BIN_DIR}/prog.c"
    "${LSQUIC_BIN_DIR}/test_common.c"
    test_cert.c
    lsquic_echo_client.c
)

target_include_directories(lsquic_echo_server
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LSQUIC_INCLUDE_DIR}"
        "${LSQUIC_INCLUDE_DIR}/lsquic"
        "${LSQUIC_BIN_DIR}"
        "${LSQUIC_INTERNAL_INCLUDE_DIR}"
        "${LSQUIC_BUILD_BIN_INCLUDE_DIR}"
)
target_include_directories(lsquic_echo_client
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LSQUIC_INCLUDE_DIR}"
        "${LSQUIC_INCLUDE_DIR}/lsquic"
        "${LSQUIC_BIN_DIR}"
        "${LSQUIC_INTERNAL_INCLUDE_DIR}"
        "${LSQUIC_BUILD_BIN_INCLUDE_DIR}"
)

target_link_libraries(lsquic_echo_server
    PRIVATE
        "${LSQUIC_LIBRARY}"
        event_static
        ssl
        crypto
        ZLIB::ZLIB
        Threads::Threads
)
target_link_libraries(lsquic_echo_client
    PRIVATE
        "${LSQUIC_LIBRARY}"
        event_static
        ssl
        crypto
        ZLIB::ZLIB
        Threads::Threads
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(_target IN ITEMS lsquic_echo_server lsquic_echo_client)
        target_link_options(${_target} PRIVATE -static-libstdc++ -static-libgcc)
    endforeach()
endif()
