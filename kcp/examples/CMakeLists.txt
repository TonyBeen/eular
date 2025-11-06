list(APPEND EXAMPLES
    kcp_server.out
    kcp_client.out
    kcp_sendfile.out
    kcp_file_server.out
)

add_executable(kcp_server.out kcp_server.c)
target_link_libraries(kcp_server.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(kcp_client.out kcp_client.c)
target_link_libraries(kcp_client.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(kcp_sendfile.out kcp_sendfile.cc file_info.cc)
target_link_libraries(kcp_sendfile.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(kcp_file_server.out kcp_file_server.cc file_info.cc)
target_compile_definitions(kcp_file_server.out PRIVATE DONT_SAVE_FILE)
target_link_libraries(kcp_file_server.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_custom_target(examples DEPENDS ${EXAMPLES})
