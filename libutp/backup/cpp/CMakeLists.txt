cmake_minimum_required(VERSION 3.22)

project(libutp_cpp_workspace LANGUAGES C CXX)

# Tests are registered by the shared root as a subdirectory.  Enable testing
# in this top-level project too so `ctest --test-dir <cpp-build>` discovers
# that subdirectory's CTestTestfile.cmake.
enable_testing()

set(UTP_SHARED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH
    "Path to the libutp shared root containing 3rd/ and cmake/")

if(NOT EXISTS "${UTP_SHARED_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "UTP_SHARED_ROOT does not contain CMakeLists.txt: ${UTP_SHARED_ROOT}")
endif()

# The root owns shared dependencies and defines eular::utp. This wrapper makes
# `cmake -S cpp` a supported entry point without copying 3rd/ into cpp/.
add_subdirectory("${UTP_SHARED_ROOT}" "${CMAKE_BINARY_DIR}/libutp-root")
