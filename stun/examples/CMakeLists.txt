list(APPEND EXAMPLES
    test_stun
    stunc
    ntrs_server
    ntrs_client
    ntrs_hub
    ntrs_node
)

find_package(Libevent REQUIRED)

add_executable(test_stun test_stun.cc)
target_link_libraries(test_stun stun)

add_executable(stunc stunc.cc)
target_compile_options(stunc PRIVATE -std=c++11)
target_include_directories(stunc PRIVATE ${LIBEVENT_INCLUDE_DIRS})
target_link_libraries(stunc stun ${LIBEVENT_STATIC_LIBRARIES})

add_executable(ntrs_server ntrs_server.cc)
target_compile_options(ntrs_server PRIVATE -std=c++11)
target_link_libraries(ntrs_server stun)

add_executable(ntrs_client ntrs_client.cc)
target_compile_options(ntrs_client PRIVATE -std=c++11)
target_link_libraries(ntrs_client stun)

add_executable(ntrs_hub ntrs_hub.cc)
target_compile_options(ntrs_hub PRIVATE -std=c++11)
target_link_libraries(ntrs_hub stun)

add_executable(ntrs_node ntrs_node.cc)
target_compile_options(ntrs_node PRIVATE -std=c++11)
target_link_libraries(ntrs_node stun)
