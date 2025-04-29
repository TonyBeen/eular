list(APPEND EXAMPLES
    kcp_server.out
    kcp_client.out
)

add_executable(kcp_server.out kcp_server.c)
target_link_libraries(kcp_server.out ${KCPP_LIBRARIE} ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(kcp_client.out kcp_client.c)
target_link_libraries(kcp_client.out ${KCPP_LIBRARIE} ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_custom_target(examples DEPENDS ${EXAMPLES})
