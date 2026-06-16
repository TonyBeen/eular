list(APPEND EXAMPLES
    ntrs_natc_multi
    ntrs_peer
    ntrs_hub
    ntrs_node
)

add_executable(ntrs_natc_multi ntrs_natc_multi.cc)
target_compile_options(ntrs_natc_multi PRIVATE -std=c++11)
target_link_libraries(ntrs_natc_multi ntrs)

add_executable(ntrs_peer ntrs_peer.cc)
target_compile_options(ntrs_peer PRIVATE -std=c++11)
target_link_libraries(ntrs_peer ntrs)

add_executable(ntrs_hub ntrs_hub.cc)
target_compile_options(ntrs_hub PRIVATE -std=c++11)
target_link_libraries(ntrs_hub ntrs)

add_executable(ntrs_node ntrs_node.cc)
target_compile_options(ntrs_node PRIVATE -std=c++11)
target_link_libraries(ntrs_node ntrs)
