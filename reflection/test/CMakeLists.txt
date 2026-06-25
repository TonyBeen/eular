add_library(reflection_static_assert_check OBJECT
    reflection_static_assert_test.cc
)

target_link_libraries(reflection_static_assert_check
    PRIVATE
        reflection_mini
)

target_compile_features(reflection_static_assert_check PRIVATE cxx_std_11)

add_executable(reflection_runtime_test
    reflection_runtime_test.cc
)

target_link_libraries(reflection_runtime_test
    PRIVATE
        reflection_mini
)

target_compile_features(reflection_runtime_test PRIVATE cxx_std_11)

add_test(NAME reflection_runtime_test COMMAND reflection_runtime_test)
