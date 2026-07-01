cmake_minimum_required(VERSION 3.15)

project(ntrs
    VERSION 0.2.0
    DESCRIPTION "Private NTRS NAT traversal library"
    LANGUAGES C CXX
)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE BOOL "Disable package registry writes in local and cross builds." FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "Ignore user package registry during configuration." FORCE)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON CACHE BOOL "Avoid package registry lookups for reproducible builds." FORCE)

enable_testing()

option(NTRS_BUILD_EXAMPLES "Build NTRS examples" ON)
option(NTRS_BUILD_STATIC "Build static NTRS library" ON)
option(NTRS_BUILD_SHARED "Build shared NTRS library" ON)
option(NTRS_STATIC_EXECUTABLES "Link ntrs_hub/ntrs_node as static executables when possible." OFF)
set(NTRS_EVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libevent" CACHE PATH "Path to the sibling libevent wrapper project.")
set(NTRS_UTILS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../utils" CACHE PATH "Path to the sibling utils project.")

set(CMAKE_CXX_STANDARD 11)

find_package(Threads REQUIRED)

option(NTRS_BUILD_TESTS "Build NTRS tests" ON)

if(NOT EXISTS "${NTRS_EVENT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libevent wrapper project not found: ${NTRS_EVENT_ROOT}")
endif()
if(NOT EXISTS "${NTRS_UTILS_ROOT}/include/utils/sysdef.h")
    message(FATAL_ERROR "utils headers not found: ${NTRS_UTILS_ROOT}")
endif()
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(NTRS_PRIVATE_COMPILE_OPTIONS)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND NTRS_PRIVATE_COMPILE_OPTIONS -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER MATCHES "musl")
    set(NTRS_MUSL_STATIC ON)
else()
    set(NTRS_MUSL_STATIC OFF)
endif()

set(NTRS_SOURCES
    src/md5.c
    src/sha256.c
    src/ntrs_auth.cpp
    src/ntrs_binary_protocol.cpp
    src/ntrs_client.cpp
    src/ntrs_codec.cpp
    src/ntrs_hub_state.cpp
    src/ntrs_io.cpp
    src/socket_address.cpp
)

if(NOT NTRS_BUILD_STATIC AND NOT NTRS_BUILD_SHARED)
    message(FATAL_ERROR "At least one of NTRS_BUILD_STATIC or NTRS_BUILD_SHARED must be ON.")
endif()

set(EVENT_SUBMODULE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EVENT_STATIC_LIB ON CACHE BOOL "" FORCE)
set(EVENT_DYNAMIC_LIB OFF CACHE BOOL "" FORCE)
set(EVENT_USE_CARES OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NTRS_MUSL_STATIC)
    set(EVENT__HAVE_KQUEUE OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_SYS_EVENT_H OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_ARC4RANDOM OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_ARC4RANDOM_BUF OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_ARC4RANDOM_ADDRANDOM OFF CACHE INTERNAL "" FORCE)
endif()
add_subdirectory("${NTRS_EVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent")

function(ntrs_configure_library target)
    target_compile_options(${target} PRIVATE ${NTRS_PRIVATE_COMPILE_OPTIONS})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ntrs
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${NTRS_UTILS_ROOT}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(${target} PUBLIC
        event_wrapper_static
        Threads::Threads
    )
endfunction()

if(NTRS_BUILD_STATIC)
    add_library(ntrs_static STATIC ${NTRS_SOURCES})
    ntrs_configure_library(ntrs_static)
endif()

if(NTRS_BUILD_SHARED)
    add_library(ntrs_shared SHARED ${NTRS_SOURCES})
    ntrs_configure_library(ntrs_shared)
    target_compile_definitions(ntrs_shared PRIVATE NTRS_BUILD_SHARED NTRS_EXPORTS)
    target_compile_definitions(ntrs_shared INTERFACE NTRS_BUILD_SHARED)
endif()

if(NTRS_BUILD_STATIC)
    add_library(eular::ntrs ALIAS ntrs_static)
elseif(NTRS_BUILD_SHARED)
    add_library(eular::ntrs ALIAS ntrs_shared)
endif()

if(NTRS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(NTRS_BUILD_TESTS)
    add_subdirectory(test)
endif()
