set(FCONTEXT_TEST_TARGET fcontext_tests)

add_executable(${FCONTEXT_TEST_TARGET}
    test_main.cc
    test_fcontext.cc)

target_compile_features(${FCONTEXT_TEST_TARGET} PRIVATE cxx_std_11)
target_include_directories(${FCONTEXT_TEST_TARGET}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(${FCONTEXT_TEST_TARGET} PRIVATE fcontext)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${FCONTEXT_TEST_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME ${FCONTEXT_TEST_TARGET} COMMAND ${FCONTEXT_TEST_TARGET})
