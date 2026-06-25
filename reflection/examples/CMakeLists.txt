add_executable(reflection_mini_example
    mini_reflect_example.cpp
)

target_link_libraries(reflection_mini_example
    PRIVATE
        reflection_mini
)

target_compile_features(reflection_mini_example PRIVATE cxx_std_11)

add_executable(reflection_mini_debug_example
    mini_reflect_debug_example.cpp
)

target_link_libraries(reflection_mini_debug_example
    PRIVATE
        reflection_mini
)

target_compile_features(reflection_mini_debug_example PRIVATE cxx_std_11)
