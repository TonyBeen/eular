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

add_executable(utp_session_resume_server
    session_resume_server.cpp
)
target_include_directories(utp_session_resume_server PRIVATE ${UTP_EXAMPLE_COMMON_INCLUDE})
target_link_libraries(utp_session_resume_server PRIVATE utp_static)

add_executable(utp_session_resume_client
    session_resume_client.cpp
)
target_include_directories(utp_session_resume_client PRIVATE ${UTP_EXAMPLE_COMMON_INCLUDE})
target_link_libraries(utp_session_resume_client PRIVATE utp_static)

add_executable(utp_mtu_keepalive_server
    mtu_keepalive_server.cpp
)
target_include_directories(utp_mtu_keepalive_server PRIVATE ${UTP_EXAMPLE_COMMON_INCLUDE})
target_link_libraries(utp_mtu_keepalive_server PRIVATE utp_static)

add_executable(utp_mtu_keepalive_client
    mtu_keepalive_client.cpp
)
target_include_directories(utp_mtu_keepalive_client PRIVATE ${UTP_EXAMPLE_COMMON_INCLUDE})
target_link_libraries(utp_mtu_keepalive_client PRIVATE utp_static)
