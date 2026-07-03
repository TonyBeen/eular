cmake_minimum_required(VERSION 3.15)

project(reflection LANGUAGES CXX)

option(REFLECTION_BUILD_EXAMPLES "Build reflection examples" ON)
option(REFLECTION_BUILD_TESTS "Build reflection tests" ON)

add_library(reflection_mini INTERFACE)

target_include_directories(reflection_mini
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(reflection_mini INTERFACE cxx_std_11)

if(REFLECTION_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(REFLECTION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
