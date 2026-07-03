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
set(NTRS_STRIP_SHARED_INTERNAL_SYMBOLS "" CACHE STRING
    "Strip local symbols from the NTRS shared library on ELF platforms: ON, OFF, or empty for Release/MinSizeRel only.")
set_property(CACHE NTRS_STRIP_SHARED_INTERNAL_SYMBOLS PROPERTY STRINGS "" ON OFF)
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

set(NTRS_PUBLIC_API_SYMBOLS
    ntrs_nat_info_init
    ntrs_detect_nat_options_init
    ntrs_connect_control
    ntrs_request_probe_endpoints
    ntrs_auth
    ntrs_register_peer
    ntrs_unregister_peer
    ntrs_create_session
    ntrs_wait_for_signal
    ntrs_try_udp_hole_punch
    ntrs_async_client_create
    ntrs_async_client_destroy
    ntrs_async_client_cancel
    ntrs_async_auth
    ntrs_async_connect_control
    ntrs_async_request_probe_endpoints
    ntrs_async_detect_nat
    ntrs_async_register_peer
    ntrs_async_unregister_peer
    ntrs_async_heartbeat
    ntrs_async_create_session
    ntrs_async_wait_for_signal
    ntrs_async_try_udp_hole_punch
)

function(ntrs_generate_export_files elf_file macos_file)
    file(WRITE "${elf_file}" "{\n    global:\n")
    file(WRITE "${macos_file}" "")
    foreach(symbol IN LISTS NTRS_PUBLIC_API_SYMBOLS)
        file(APPEND "${elf_file}" "        ${symbol};\n")
        file(APPEND "${macos_file}" "_${symbol}\n")
    endforeach()
    file(APPEND "${elf_file}" "    local:\n        *;\n};\n")
endfunction()

if(NOT NTRS_BUILD_STATIC AND NOT NTRS_BUILD_SHARED)
    message(FATAL_ERROR "At least one of NTRS_BUILD_STATIC or NTRS_BUILD_SHARED must be ON.")
endif()

if(NTRS_STRIP_SHARED_INTERNAL_SYMBOLS STREQUAL "")
    set(NTRS_STRIP_SHARED_INTERNAL_SYMBOLS_EFFECTIVE OFF)
    if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
        set(NTRS_STRIP_SHARED_INTERNAL_SYMBOLS_EFFECTIVE ON)
    endif()
else()
    set(NTRS_STRIP_SHARED_INTERNAL_SYMBOLS_EFFECTIVE ${NTRS_STRIP_SHARED_INTERNAL_SYMBOLS})
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
        $<INSTALL_INTERFACE:include>
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${NTRS_UTILS_ROOT}/include
    )
endfunction()

function(ntrs_link_static_dependencies target)
    target_link_libraries(${target} PUBLIC
        event_wrapper_static
        Threads::Threads
    )
endfunction()

function(ntrs_link_shared_dependencies target)
    target_link_libraries(${target} PRIVATE
        event_wrapper_static
        Threads::Threads
    )
endfunction()

if(NTRS_BUILD_STATIC)
    add_library(ntrs_static STATIC ${NTRS_SOURCES})
    ntrs_configure_library(ntrs_static)
    ntrs_link_static_dependencies(ntrs_static)
endif()

if(NTRS_BUILD_SHARED)
    add_library(ntrs_shared SHARED ${NTRS_SOURCES})
    ntrs_configure_library(ntrs_shared)
    ntrs_link_shared_dependencies(ntrs_shared)
    target_compile_definitions(ntrs_shared PRIVATE NTRS_BUILD_SHARED NTRS_EXPORTS)
    target_compile_definitions(ntrs_shared INTERFACE NTRS_BUILD_SHARED)
    set(NTRS_ELF_EXPORTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/ntrs.exports")
    set(NTRS_MACOS_EXPORTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/ntrs.macos.exports")
    ntrs_generate_export_files("${NTRS_ELF_EXPORTS_FILE}" "${NTRS_MACOS_EXPORTS_FILE}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(ntrs_shared PRIVATE
            "LINKER:--version-script=${NTRS_ELF_EXPORTS_FILE}"
        )
        if(NTRS_STRIP_SHARED_INTERNAL_SYMBOLS_EFFECTIVE AND CMAKE_STRIP)
            add_custom_command(TARGET ntrs_shared POST_BUILD
                COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:ntrs_shared>"
                COMMENT "Stripping local symbols from $<TARGET_FILE_NAME:ntrs_shared>"
            )
        endif()
    elseif(APPLE)
        target_link_options(ntrs_shared PRIVATE
            "LINKER:-exported_symbols_list,${NTRS_MACOS_EXPORTS_FILE}"
        )
    endif()
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
