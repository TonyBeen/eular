list(APPEND EXAMPLES
    test_stun
    stunc
)

find_package(Libevent REQUIRED)

add_executable(test_stun test_stun.cc)
target_link_libraries(test_stun stun)

add_executable(stunc stunc.cc)
target_compile_options(stunc PRIVATE -std=c++11)
target_include_directories(stunc PRIVATE ${LIBEVENT_INCLUDE_DIRS})
target_link_libraries(stunc stun ${LIBEVENT_STATIC_LIBRARIES})