add_executable(stun_unit_tests
    test_main.cc
    ntrs_regression_test.cc
)

target_compile_options(stun_unit_tests PRIVATE -std=c++11)
target_include_directories(stun_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/catch
)
target_link_libraries(stun_unit_tests stun)

add_test(NAME stun_unit_tests COMMAND stun_unit_tests)
