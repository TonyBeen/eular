set(UTP_EXAMPLE_COMMON_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(utp_echo_server
    echo_server.cpp
)
target_include_directories(utp_echo_server PRIVATE ${UTP_EXAMPLE_COMMON_INCLUDE})
target_link_libraries(utp_echo_server PRIVATE utp_static)

add_executable(utp_echo_client
    echo_client.cpp
)
target_include_directories(utp_echo_client PRIVATE ${UTP_EXAMPLE_COMMON_INCLUDE})
target_link_libraries(utp_echo_client PRIVATE utp_static)
