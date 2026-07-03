find_package(lsquic QUIET)
if(NOT lsquic_FOUND)
    message(WARNING "lsquic library not found via find_package(lsquic), skipping lsquic echo examples")
    set(UTP_BUILD_LSQUIC_ECHO OFF CACHE BOOL "Build lsquic echo comparison examples" FORCE)
    return()
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_executable(lsquic_echo_server
    prog.c
    test_cert.c
    lsquic_echo_server.c
)

add_executable(lsquic_echo_client
    prog.c
    lsquic_echo_client.c
)

target_include_directories(lsquic_echo_server
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(lsquic_echo_client
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(lsquic_echo_server
    PRIVATE
        lsquic::lsquic
        event_static
        ssl
        crypto
        ZLIB::ZLIB
        Threads::Threads
)
target_link_libraries(lsquic_echo_client
    PRIVATE
        lsquic::lsquic
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
