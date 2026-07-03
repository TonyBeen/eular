add_executable(variant_example
    variant_example.cpp
)

target_link_libraries(variant_example
    PRIVATE
        variant
)

target_compile_features(variant_example PRIVATE cxx_std_11)
