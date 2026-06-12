cmake_minimum_required(VERSION 3.10)

include(cmake/configure_version.cmake)

project(kcpp LANGUAGES C CXX VERSION ${KCP_VERSION_STRING})

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

include(CheckFunctionExists)
include(CheckSymbolExists)

check_function_exists(sendmmsg HAVE_SENDMMSG)

# default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo") # Release MinSizeRel RelWithDebInfo Debug
endif()

option(KCPP_DYNAMIC_LIB "Build dynamic library" ON)
option(KCPP_STATIC_LIB "Build static library" ON)
option(BUILD_EXAMPLES "Compile sample program" ON)
option(BUILD_TEST_TOOLS "Compile test helper programs" ON)
option(USE_SENDMMSG "Use sendmmsg" ON)
option(ENABLE_DEBUG "Enable debug symbol" OFF)
option(KCPP_ENABLE_NTRS "Build optional NTRS/STUN bridge" OFF)
set(KCPP_EVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libevent" CACHE PATH "Path to the sibling libevent wrapper project.")
set(KCPP_UTILS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../utils" CACHE PATH "Path to the sibling utils project.")
set(KCPP_STUN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../stun" CACHE PATH "Path to the sibling stun project.")

if(ENABLE_DEBUG MATCHES "ON")
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(_kcpp_build_examples "${BUILD_EXAMPLES}")
set(_kcpp_build_test_tools "${BUILD_TEST_TOOLS}")
if(KCPP_ENABLE_NTRS)
    if(NOT EXISTS "${KCPP_STUN_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "stun project not found: ${KCPP_STUN_ROOT}")
    endif()
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(STUN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(STUN_MQTT_WITH_TLS OFF CACHE BOOL "" FORCE)
    set(STUN_EVENT_ROOT "${KCPP_EVENT_ROOT}" CACHE PATH "" FORCE)
    set(STUN_UTILS_ROOT "${KCPP_UTILS_ROOT}" CACHE PATH "" FORCE)
    add_subdirectory("${KCPP_STUN_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/stun")
else()
    if(NOT EXISTS "${KCPP_EVENT_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "libevent wrapper project not found: ${KCPP_EVENT_ROOT}")
    endif()
    set(EVENT_SUBMODULE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(EVENT_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(EVENT_DYNAMIC_LIB OFF CACHE BOOL "" FORCE)
    set(EVENT_USE_CARES OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TEST OFF CACHE BOOL "" FORCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER MATCHES "musl")
        set(EVENT__HAVE_KQUEUE OFF CACHE INTERNAL "" FORCE)
        set(EVENT__HAVE_SYS_EVENT_H OFF CACHE INTERNAL "" FORCE)
        set(EVENT__HAVE_ARC4RANDOM OFF CACHE INTERNAL "" FORCE)
        set(EVENT__HAVE_ARC4RANDOM_BUF OFF CACHE INTERNAL "" FORCE)
        set(EVENT__HAVE_ARC4RANDOM_ADDRANDOM OFF CACHE INTERNAL "" FORCE)
    endif()
    add_subdirectory("${KCPP_EVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent")
endif()
set(BUILD_EXAMPLES "${_kcpp_build_examples}" CACHE BOOL "Compile sample program" FORCE)
set(BUILD_TEST_TOOLS "${_kcpp_build_test_tools}" CACHE BOOL "Compile test helper programs" FORCE)
unset(_kcpp_build_examples)
unset(_kcpp_build_test_tools)

set(LIBEVENT_STATIC_LIBRARIES eular::event_wrapper_static)
set(LIBEVENT_SHARED_LIBRARIES "")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kcp_version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/kcp_version.h
    NEWLINE_STYLE LF
)

if(USE_SENDMMSG MATCHES "ON" AND HAVE_SENDMMSG)
    add_definitions(-DUSE_SENDMMSG)
endif()

file(GLOB HEADER_LIST           "include/*.h")
file(GLOB INTERNAL_HEADER_LIST  "internal/*.h")
file(GLOB 3RD_HEADER_LIST       "3rd_party/*.h")
list(APPEND HEADER_LIST         ${INTERNAL_HEADER_LIST})
list(APPEND HEADER_LIST         ${3RD_HEADER_LIST})

file(GLOB SRC_LIST              "src/*.c")
file(GLOB 3RD_SRC_LIST          "3rd_party/*.c")
list(APPEND SRC_LIST            "${CMAKE_CURRENT_SOURCE_DIR}/src/kcp_ntrs_stub.c")
list(REMOVE_DUPLICATES SRC_LIST)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-D_GNU_SOURCE -DXXH_INLINE_ALL -D__USE_FILE_OFFSET64 -D_LARGEFILE64_SOURCE)
    add_compile_options(-W -Wall -Werror -Wextra -Wno-deprecated)
    # -Wno-unused-result -Wno-unused-function -Wno-unused-parameter
endif()

if(BUILD_EXAMPLES MATCHES "ON")
    add_subdirectory(examples)
endif()

if(BUILD_TEST_TOOLS MATCHES "ON")
    add_subdirectory(test)
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

    target_link_libraries(kcpp_static PUBLIC ${LIBEVENT_STATIC_LIBRARIES})

    target_compile_options(
        kcpp_static PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties(
        kcpp_static
        PROPERTIES
        LINK_FLAGS "-Wl,-z,now,-z,relro"
    )
    set_property(TARGET kcpp_static PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    set(KCPP_LIBRARIE kcpp_static CACHE INTERNAL "link kcpp librarie")

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL )
        install(TARGETS kcpp_static
            RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}")
    endif()
endif()

# 动态库
if(KCPP_DYNAMIC_LIB MATCHES "ON")
    add_library(kcpp SHARED ${SRC_LIST} ${3RD_SRC_LIST} ${HEADER_LIST})
    add_library(eular::kcpp ALIAS kcpp)

    target_include_directories(kcpp
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/internal
            ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party
    )

    target_link_libraries(kcpp PUBLIC ${LIBEVENT_STATIC_LIBRARIES})

    target_compile_options(
        kcpp PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties(
        kcpp
        PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        LINK_FLAGS "-Wl,-z,now,-z,relro"
    )
    set_property(TARGET kcpp PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    unset(KCPP_LIBRARIE)
    set(KCPP_LIBRARIE kcpp CACHE INTERNAL "link kcpp librarie")

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL )
        install(TARGETS kcpp
            RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}")
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
