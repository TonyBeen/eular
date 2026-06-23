add_executable(ntrs_unit_tests
    sha256_test.cc
    test_main.cc
)

target_compile_options(ntrs_unit_tests PRIVATE -std=c++11 ${NTRS_PRIVATE_COMPILE_OPTIONS})
target_include_directories(ntrs_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/catch
)
target_link_libraries(ntrs_unit_tests ntrs)

add_test(NAME ntrs_unit_tests COMMAND ntrs_unit_tests)
