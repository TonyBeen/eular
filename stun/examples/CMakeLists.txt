list(APPEND EXAMPLES
    test_stun
    stunc
    stun_nat_detect
    stun_peer
    stun_hub
    stun_node
)

add_executable(test_stun test_stun.cc)
target_link_libraries(test_stun stun)

add_executable(stunc stunc.cc)
target_compile_options(stunc PRIVATE -std=c++11)
target_link_libraries(stunc stun)

add_executable(stun_nat_detect ntrs_nat_detect.cc)
target_compile_options(stun_nat_detect PRIVATE -std=c++11)
target_link_libraries(stun_nat_detect stun)

add_executable(stun_peer ntrs_peer.cc)
target_compile_options(stun_peer PRIVATE -std=c++11)
target_link_libraries(stun_peer stun)

add_executable(stun_hub ntrs_hub.cc)
target_compile_options(stun_hub PRIVATE -std=c++11)
target_link_libraries(stun_hub stun)

add_executable(stun_node ntrs_node.cc)
target_compile_options(stun_node PRIVATE -std=c++11)
target_link_libraries(stun_node stun)
