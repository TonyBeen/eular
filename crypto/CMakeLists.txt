cmake_minimum_required(VERSION 3.0)

project(utils VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

option(UTILS_STATIC_LIB "Build static library" ON)
option(UTILS_DYNAMIC_LIB "Build dynamic library" ON)
option(ENABLE_DEBUG "Enable debug symbol" ON)

option(ENABLE_GNUTLS "Enable use of GnuTLS" ON)
option(ENABLE_MBEDTLS "Enable use of mbed TLS" ON)
option(ENABLE_OPENSSL "Enable use of OpenSSL" ON)
option(ENABLE_WINDOWS_CRYPTO "Enable use of Windows cryptography libraries" ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo") # Release MinSizeRel RelWithDebInfo Debug
endif()

if(ENABLE_DEBUG MATCHES "ON")
    set(CMAKE_BUILD_TYPE "Debug")
endif()

if(ENABLE_GNUTLS)
    find_package(Nettle 3.0)
    find_package(GnuTLS)
endif()

if(ENABLE_MBEDTLS)
    find_package(MbedTLS 1.0)
endif()

if(ENABLE_OPENSSL)
    find_package(OpenSSL)
endif()

if(WIN32)
    if(ENABLE_WINDOWS_CRYPTO)
        set(WINDOWS_CRYPTO_FOUND TRUE)
    endif()
endif()

# Windows 默认使用 Windows Cryptography API
# Linux 默认使用 OpenSSL
if (WINDOWS_CRYPTO_FOUND)
    set(HAVE_CRYPTO 1)
    set(HAVE_WINDOWS_CRYPTO 1)
    add_compile_definitions(HAVE_WINDOWS_CRYPTO)
elseif (OPENSSL_FOUND)
    set(HAVE_CRYPTO 1)
    set(HAVE_OPENSSL 1)
    add_compile_definitions(HAVE_OPENSSL)
elseif (GNUTLS_FOUND AND NETTLE_FOUND)
    set(HAVE_CRYPTO 1)
    set(HAVE_GNUTLS 1)
    add_compile_definitions(HAVE_GNUTLS)
elseif (MBEDTLS_FOUND)
    set(HAVE_CRYPTO 1)
    set(HAVE_MBEDTLS 1)
    add_compile_definitions(HAVE_MBEDTLS)
endif()

if ((ENABLE_GNUTLS OR ENABLE_MBEDTLS OR ENABLE_OPENSSL OR ENABLE_WINDOWS_CRYPTO) AND NOT HAVE_CRYPTO)
    message(FATAL_ERROR "neither GnuTLS, mbed TLS, OpenSSL, nor Windows Cryptography found;")
endif()

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(_CRT_NONSTDC_NO_DEPRECATE)
endif(MSVC)

message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

set(INSTALL_LIB_DIR "${CMAKE_INSTALL_PREFIX}/lib" CACHE PATH "Installation directory for libraries")
set(INSTALL_INC_DIR "${CMAKE_INSTALL_PREFIX}/include/" CACHE PATH "Installation directory for headers")

file(GLOB HEADER_LIST       "include/utils/*.h")
file(GLOB HPP_HEADER_LIST   "include/utils/*.hpp")
list(APPEND HEADER_LIST ${HPP_HEADER_LIST})

file(GLOB SRC_LIST          "src/*.cpp")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Linux")
    add_definitions(-D_GNU_SOURCE -D__USE_FILE_OFFSET64 -D_LARGEFILE64_SOURCE)
    add_compile_options(-W -Wall -Werror -Wextra -Wno-deprecated -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
endif()

# link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rd)

# build the static library
if(UTILS_STATIC_LIB MATCHES "ON")
    add_library(utils_static STATIC ${SRC_LIST} ${HEADER_LIST})
    add_library(eular::utils_static ALIAS utils_static)

    target_include_directories(utils_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(utils_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/3rd/libiconv.a pthread)

    target_compile_options(
        utils_static PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties (
        utils_static
        PROPERTIES
        LINK_FLAGS "-Wl,-z,now,-z,relro -pie"
    )
    set_property(TARGET utils_static PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL )
        install(TARGETS utils_static
            RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}" )
    endif()
endif()

#build dynamic library
if(UTILS_DYNAMIC_LIB MATCHES "ON")
    add_library(utils SHARED ${SRC_LIST} ${HEADER_LIST})
    add_library(eular::utils ALIAS utils)

    target_include_directories(utils
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/3rd/libiconv.a pthread)

    target_compile_options(
        utils PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties (
        utils
        PROPERTIES
        LINK_FLAGS "-Wl,-z,now,-z,relro"
    )

    set_property(TARGET utils PROPERTY LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL )
        install(TARGETS utils
            RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}" )
    endif()
endif()

if(NOT SKIP_INSTALL_HEADERS AND NOT SKIP_INSTALL_ALL)
    install(FILES ${HEADER_LIST} DESTINATION "${INSTALL_INC_DIR}/utils")
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
