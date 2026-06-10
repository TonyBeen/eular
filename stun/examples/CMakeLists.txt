list(APPEND EXAMPLES
    test_stun
    stunc
    ntrs_nat_detect
    ntrs_peer
    ntrs_hub
    ntrs_node
)

add_executable(test_stun test_stun.cc)
target_link_libraries(test_stun stun)

add_executable(stunc stunc.cc)
target_compile_options(stunc PRIVATE -std=c++11)
target_link_libraries(stunc stun)

add_executable(ntrs_nat_detect ntrs_nat_detect.cc)
target_compile_options(ntrs_nat_detect PRIVATE -std=c++11)
target_link_libraries(ntrs_nat_detect stun)

add_executable(ntrs_peer ntrs_peer.cc)
target_compile_options(ntrs_peer PRIVATE -std=c++11)
target_link_libraries(ntrs_peer stun)

add_executable(ntrs_hub ntrs_hub.cc)
target_compile_options(ntrs_hub PRIVATE -std=c++11)
target_link_libraries(ntrs_hub stun)

add_executable(ntrs_node ntrs_node.cc)
target_compile_options(ntrs_node PRIVATE -std=c++11)
target_link_libraries(ntrs_node stun)
