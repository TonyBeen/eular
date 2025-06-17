# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Project name and version
project(stun VERSION 1.0 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Set output directories for binaries and libraries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.c")

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Add executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Link libraries (if any)
# target_link_libraries(${PROJECT_NAME} <library_name>)

# Enable warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
endif()