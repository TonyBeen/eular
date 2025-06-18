list(APPEND EXAMPLES
    test_stun
)

add_executable(test_stun test_stun.cc)

target_link_libraries(test_stun stun)
