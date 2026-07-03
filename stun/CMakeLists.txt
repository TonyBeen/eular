cmake_minimum_required(VERSION 3.15)

project(stun
    VERSION 1.0.0
    DESCRIPTION "STUN protocol implementation"
    LANGUAGES C CXX
)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE BOOL "Disable package registry writes in local and cross builds." FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "Ignore user package registry during configuration." FORCE)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON CACHE BOOL "Avoid package registry lookups for reproducible builds." FORCE)

enable_testing()

# Options
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build shared library" OFF)
option(STUN_STATIC_EXECUTABLES "Link ntrs_hub/ntrs_node as static executables when possible." OFF)
option(STUN_MQTT_WITH_TLS "Build vendored mosquitto with TLS/OpenSSL support." OFF)
set(STUN_BUILD_EXAMPLES ${BUILD_EXAMPLES})
set(STUN_MUSL_STATIC_DEFAULT OFF)
if(CMAKE_C_COMPILER MATCHES "musl")
    set(STUN_MUSL_STATIC_DEFAULT ON)
endif()
option(STUN_MUSL_STATIC "Build ntrs_hub/ntrs_node for mostly single-file musl static deployment."
       ${STUN_MUSL_STATIC_DEFAULT})
set(STUN_EVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libevent" CACHE PATH "Path to the sibling libevent wrapper project.")
set(STUN_UTILS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../utils" CACHE PATH "Path to the sibling utils project.")
set(STUN_MUSL_ROOT "${STUN_MUSL_ROOT}" CACHE PATH "Path to the musl toolchain root when using the musl toolchain file.")

# C++ standard
set(CMAKE_CXX_STANDARD 11)

find_package(Threads REQUIRED)
if(STUN_MQTT_WITH_TLS)
    if(STUN_MUSL_STATIC OR STUN_STATIC_EXECUTABLES)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
        if(NOT DEFINED OPENSSL_ROOT_DIR AND DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
            set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}" CACHE PATH "Path to the static OpenSSL root." FORCE)
        endif()
    endif()
    find_package(OpenSSL REQUIRED)
endif()
set(STUN_EXTRA_CRYPTO_LIBS)
if(STUN_MQTT_WITH_TLS)
    if(STUN_MUSL_STATIC OR STUN_STATIC_EXECUTABLES)
        set(_stun_saved_find_library_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    find_package(ZLIB REQUIRED)
    set(STUN_ZLIB_TARGET ZLIB::ZLIB)
    if(STUN_MUSL_STATIC OR STUN_STATIC_EXECUTABLES)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_stun_saved_find_library_suffixes})
        set(_stun_saved_find_library_prefixes ${CMAKE_FIND_LIBRARY_PREFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
        find_library(STUN_STATIC_ZLIB_LIBRARY
            NAMES z libz.a
        )
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_stun_saved_find_library_suffixes})
        set(CMAKE_FIND_LIBRARY_PREFIXES ${_stun_saved_find_library_prefixes})
        if(NOT STUN_STATIC_ZLIB_LIBRARY)
            message(FATAL_ERROR "Static TLS builds require a static zlib library (.a).")
        endif()
        add_library(stun_zlib_static STATIC IMPORTED)
        set_target_properties(stun_zlib_static PROPERTIES
            IMPORTED_LOCATION ${STUN_STATIC_ZLIB_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_INCLUDE_DIR}
        )
        set(STUN_ZLIB_TARGET stun_zlib_static)
    endif()
    if(DEFINED OPENSSL_INCLUDE_DIR AND NOT "${OPENSSL_INCLUDE_DIR}" STREQUAL "")
        set(STUN_OPENSSL_INCLUDE_DIR ${OPENSSL_INCLUDE_DIR})
    else()
        get_target_property(STUN_OPENSSL_INCLUDE_DIR OpenSSL::SSL INTERFACE_INCLUDE_DIRECTORIES)
    endif()
    list(APPEND STUN_EXTRA_CRYPTO_LIBS
        OpenSSL::SSL
        OpenSSL::Crypto
        ${STUN_ZLIB_TARGET}
    )
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        # $<$<CONFIG:RELEASE>:-O2 -flto>
    )
endif()

if(NOT EXISTS "${STUN_EVENT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libevent wrapper project not found: ${STUN_EVENT_ROOT}")
endif()
if(NOT EXISTS "${STUN_UTILS_ROOT}/include/utils/sysdef.h")
    message(FATAL_ERROR "utils headers not found: ${STUN_UTILS_ROOT}")
endif()

# Source files
set(SOURCES
    src/ntrs_auth.cpp
    src/ntrs_client.cpp
    src/ntrs_hub_state.cpp
    src/ntrs_io.cpp
    src/socket_address.cpp
    src/stun.cpp
    src/ntrs_codec.cpp
    src/mqtt_client.cpp
)

# Add library/executable
if(BUILD_SHARED_LIBS)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    target_link_libraries(${PROJECT_NAME} PRIVATE stunmsg utils)
else()
    add_library(${PROJECT_NAME} STATIC ${SOURCES})
    target_link_libraries(${PROJECT_NAME} PRIVATE stunmsg)
endif()

# Include directories
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${STUN_UTILS_ROOT}/include>
    $<INSTALL_INTERFACE:include>
)

# 3rd party
add_subdirectory(3rd_party)
target_compile_definitions(stunmsg PRIVATE
    MD5_Init=stunmsg_MD5_Init
    MD5_Update=stunmsg_MD5_Update
    MD5_Final=stunmsg_MD5_Final
    SHA1_Init=stunmsg_SHA1_Init
    SHA1_Update=stunmsg_SHA1_Update
    SHA1_Final=stunmsg_SHA1_Final
)

# Build libevent in the same CMake graph so musl/static builds use the same
# probe results as the final binaries instead of relying on an external build.
set(EVENT_SUBMODULE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EVENT_STATIC_LIB ON CACHE BOOL "" FORCE)
set(EVENT_DYNAMIC_LIB OFF CACHE BOOL "" FORCE)
set(EVENT_USE_CARES OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND STUN_MUSL_STATIC)
    set(EVENT__HAVE_KQUEUE OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_SYS_EVENT_H OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_ARC4RANDOM OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_ARC4RANDOM_BUF OFF CACHE INTERNAL "" FORCE)
    set(EVENT__HAVE_ARC4RANDOM_ADDRANDOM OFF CACHE INTERNAL "" FORCE)
endif()
add_subdirectory("${STUN_EVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent")

# Build libmosquitto only (no broker/tools/tests) from vendored source.
set(WITH_TLS ${STUN_MQTT_WITH_TLS} CACHE BOOL "" FORCE)
set(WITH_TLS_PSK ${STUN_MQTT_WITH_TLS} CACHE BOOL "" FORCE)
if(STUN_MQTT_WITH_TLS)
    set(WITH_WEBSOCKETS ON CACHE BOOL "" FORCE)
else()
    set(WITH_WEBSOCKETS OFF CACHE BOOL "" FORCE)
endif()
set(WITH_BROKER OFF CACHE BOOL "" FORCE)
set(WITH_CLIENTS OFF CACHE BOOL "" FORCE)
set(WITH_APPS OFF CACHE BOOL "" FORCE)
set(WITH_PLUGINS OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_LIB_CPP OFF CACHE BOOL "" FORCE)
set(WITH_STATIC_LIBRARIES ON CACHE BOOL "" FORCE)
set(WITH_CTRL_SHELL OFF CACHE BOOL "" FORCE)
set(WITH_LTO OFF CACHE BOOL "" FORCE)
add_subdirectory(3rd_party/mosquitto EXCLUDE_FROM_ALL)

# Keep third-party sources untouched: patch the static target from the parent project.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT STUN_MQTT_WITH_TLS)
    target_compile_definitions(libmosquitto_common PRIVATE HAVE_GETRANDOM=1)
    target_compile_options(libmosquitto_common PRIVATE -include sys/random.h)
endif()
target_include_directories(libmosquitto_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mosquitto/common
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mosquitto/libcommon
)
if(STUN_MQTT_WITH_TLS)
    target_include_directories(libmosquitto_static PRIVATE
        ${STUN_OPENSSL_INCLUDE_DIR}
    )
endif()
if(WITH_WEBSOCKETS AND WITH_WEBSOCKETS_BUILTIN)
    target_include_directories(libmosquitto_static PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mosquitto/deps/picohttpparser
    )
endif()
if(WITH_BUNDLED_DEPS)
    target_include_directories(libmosquitto_static PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mosquitto/deps
    )
endif()

# Link against vendored static libmosquitto and OpenSSL
target_link_libraries(${PROJECT_NAME} PUBLIC
    event_wrapper_static
    libmosquitto_static
    Threads::Threads
    ${STUN_EXTRA_CRYPTO_LIBS}
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mosquitto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/cjson_compat/include
)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    MD5_Init=stunmsg_MD5_Init
    MD5_Update=stunmsg_MD5_Update
    MD5_Final=stunmsg_MD5_Final
)
target_compile_definitions(${PROJECT_NAME} PUBLIC LIBMOSQUITTO_STATIC)
if(STUN_MUSL_STATIC OR STUN_STATIC_EXECUTABLES)
    target_link_options(${PROJECT_NAME} INTERFACE -static -static-libgcc -static-libstdc++)
endif()

# Testing
if(STUN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

add_subdirectory(test)

# Install
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/ DESTINATION include)

# Export
# install(EXPORT ${PROJECT_NAME}Targets
#     FILE ${PROJECT_NAME}Targets.cmake
#     NAMESPACE ${PROJECT_NAME}::
#     DESTINATION lib/cmake/${PROJECT_NAME}
# )
