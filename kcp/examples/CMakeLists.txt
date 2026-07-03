list(APPEND EXAMPLES
    kcp_server.out
    kcp_client.out
    ntrs_kcp_client.out
    kcp_sendfile.out
    kcp_file_server.out
)

if(KCPP_ENABLE_NTRS)
    list(APPEND EXAMPLES kcp_peer.out)
endif()

add_executable(kcp_server.out kcp_server.c)
target_link_libraries(kcp_server.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(kcp_client.out kcp_client.c)
target_link_libraries(kcp_client.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(ntrs_kcp_client.out ntrs_kcp_client.cc)
target_compile_options(ntrs_kcp_client.out PRIVATE -std=c++11)
target_link_libraries(ntrs_kcp_client.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

if(KCPP_ENABLE_NTRS)
    add_executable(kcp_peer.out kcp_peer.cc)
    target_compile_options(kcp_peer.out PRIVATE -std=c++11)
    target_link_libraries(kcp_peer.out kcpp_static ntrs ${LIBEVENT_STATIC_LIBRARIES} pthread)
endif()

add_executable(kcp_sendfile.out kcp_sendfile.cc file_info.cc)
target_include_directories(kcp_sendfile.out PRIVATE ${KCPP_UTILS_ROOT}/include)
target_compile_options(kcp_sendfile.out PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-unused-function>)
target_link_libraries(kcp_sendfile.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_executable(kcp_file_server.out kcp_file_server.cc file_info.cc)
target_include_directories(kcp_file_server.out PRIVATE ${KCPP_UTILS_ROOT}/include)
target_compile_options(kcp_file_server.out PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-unused-function>)
target_compile_definitions(kcp_file_server.out PRIVATE DONT_SAVE_FILE)
target_link_libraries(kcp_file_server.out kcpp_static ${LIBEVENT_STATIC_LIBRARIES} pthread)

add_custom_target(examples DEPENDS ${EXAMPLES})
