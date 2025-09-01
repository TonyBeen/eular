cmake_minimum_required (VERSION 3.15)

project (variant LANGUAGES CXX)

set(RTTR_3RD_PARTY_DIR "${CMAKE_SOURCE_DIR}/3rd_party")

include(GNUInstallDirs)

option(BUILD_TEST "Build the unit tests of RTTR" ON)
option(BUILD_EXAMPLES "Enable this to build the examples" OFF)

if (BUILD_TEST)
    add_subdirectory(test)
endif()

if (BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# file(GLOB_RECURSE HEADERS "variant/*.h" "variant/*.hpp")

add_library(variant INTERFACE)
target_include_directories(variant INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/variant/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/variant"
    PATTERN "*.h"
    PATTERN "*.hpp"
)

# uninstall target
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake)
endif()
