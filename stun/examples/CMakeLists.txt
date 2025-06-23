list(APPEND EXAMPLES
    test_stun
    stunc
)

add_executable(test_stun test_stun.cc)
target_link_libraries(test_stun stun)

add_executable(stunc stunc.cc)
target_link_libraries(stunc stun event)