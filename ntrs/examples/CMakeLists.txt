list(APPEND EXAMPLES
    ntrs_natc
    ntrs_natc_multi
    ntrs_peer
    ntrs_hub
    ntrs_node
)

add_executable(ntrs_natc ntrs_natc.cc)
target_compile_options(ntrs_natc PRIVATE -std=c++11 ${NTRS_PRIVATE_COMPILE_OPTIONS})
target_link_libraries(ntrs_natc eular::ntrs)
target_include_directories(ntrs_natc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(ntrs_natc_multi ntrs_natc_multi.cc)
target_compile_options(ntrs_natc_multi PRIVATE -std=c++11 ${NTRS_PRIVATE_COMPILE_OPTIONS})
target_link_libraries(ntrs_natc_multi eular::ntrs)
target_include_directories(ntrs_natc_multi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(ntrs_peer ntrs_peer.cc)
target_compile_options(ntrs_peer PRIVATE -std=c++11 ${NTRS_PRIVATE_COMPILE_OPTIONS})
target_link_libraries(ntrs_peer eular::ntrs)
target_include_directories(ntrs_peer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(ntrs_hub ntrs_hub.cc)
target_compile_options(ntrs_hub PRIVATE -std=c++11 ${NTRS_PRIVATE_COMPILE_OPTIONS})
target_link_libraries(ntrs_hub eular::ntrs)
target_include_directories(ntrs_hub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(ntrs_node ntrs_node.cc)
target_compile_options(ntrs_node PRIVATE -std=c++11 ${NTRS_PRIVATE_COMPILE_OPTIONS})
target_link_libraries(ntrs_node eular::ntrs)
target_include_directories(ntrs_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
