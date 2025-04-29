cmake_minimum_required(VERSION 3.10)

include(cmake/configure_version.cmake)

project(kcpp LANGUAGES C VERSION ${KCP_VERSION_STRING})

set(CMAKE_C_STANDARD 11)

# default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo") # Release MinSizeRel RelWithDebInfo Debug
endif()

option(KCPP_DYNAMIC_LIB "Build dynamic library" ON)
option(KCPP_STATIC_LIB "Build static library" ON)
option(BUILD_EXAMPLES "Compile sample program" OFF)
option(BUILD_TEST "Compile sample program" OFF)
option(ENABLE_DEBUG "Enable debug symbol" OFF)

if(ENABLE_DEBUG MATCHES "ON")
    set(CMAKE_BUILD_TYPE "Debug")
endif()

find_package(Libevent REQUIRED)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kcp_version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/kcp_version.h
    NEWLINE_STYLE LF
)

file(GLOB HEADER_LIST           "include/*.h")
file(GLOB INTERNAL_HEADER_LIST  "internal/*.h")
file(GLOB 3RD_HEADER_LIST       "3rd_party/*.h")
list(APPEND HEADER_LIST ${INTERNAL_HEADER_LIST} ${3RD_HEADER_LIST})
file(GLOB SRC_LIST              "src/*.c")
file(GLOB 3RD_SRC_LIST          "3rd_party/*.c")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Linux")
    add_definitions(-D_GNU_SOURCE -D__USE_FILE_OFFSET64 -D_LARGEFILE64_SOURCE)
    # add_compile_options(-W -Wall -Werror -Wextra -Wno-deprecated -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
endif()

# 生成静态库
if(KCPP_STATIC_LIB MATCHES "ON")
    add_library(kcpp_static STATIC ${SRC_LIST} ${3RD_SRC_LIST} ${HEADER_LIST})
    add_library(eular::kcpp_static ALIAS kcpp_static)

    target_include_directories(kcpp_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/internal
            ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party
    )

    target_link_libraries(kcpp_static PUBLIC ${Libevent_LIBRARIES} pthread)

    target_compile_options(
        kcpp_static PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties (
        kcpp_static
        PROPERTIES
        LINK_FLAGS "-Wl,-z,now,-z,relro -pie"
    )
    set_property(TARGET kcpp_static PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL )
        install(TARGETS kcpp_static
            RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}" )
    endif()
endif()

# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake)
endif()