cmake_minimum_required(VERSION 3.10)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
if (${CMAKE_VERSION} VERSION_LESS "3.17.0")
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake-compat)
endif()

project(cryptopp VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

option(CRYPTOPP_STATIS_LIB "Build static library" ON)
option(CRYPTOPP_DYNAMIC_LIB "Build dynamic library" OFF)
option(BUILD_EXAMPLE "Build example" ON)
option(ENABLE_DEBUG "Enable debug symbol" OFF)

option(ENABLE_GNUTLS "Enable use of GnuTLS" OFF)
option(ENABLE_MBEDTLS "Enable use of mbed TLS" ON)
option(ENABLE_OPENSSL "Enable use of OpenSSL" OFF)
option(ENABLE_WINDOWS_CRYPTO "Enable use of Windows cryptography libraries" ON)

if(NOT CMAKE_BUILD_TYPE)
    if(ENABLE_DEBUG MATCHES "ON")
        set(CMAKE_BUILD_TYPE "Debug")
    else()
        set(CMAKE_BUILD_TYPE "RelWithDebInfo") # Release MinSizeRel RelWithDebInfo Debug
    endif()
endif()

include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckCSourceRuns)
include(CheckCSourceCompiles)
include(CheckStructHasMember)
include(TestBigEndian)
include(GNUInstallDirs)
include(CTest)

add_subdirectory(3rd_party/base64)

if(ENABLE_GNUTLS)
    find_package(Nettle 3.0)
endif()

message(STATUS "Nettle_LIBRARIES: ${Nettle_LIBRARIES}")

if(ENABLE_MBEDTLS)
    find_package(MbedTLS)
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
elseif (NETTLE_FOUND)
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

set(HEADER_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/aes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/base64.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/crc32.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/md5.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/rsa.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/sha.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crypto/xxhash.h
)

set(SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/src/base64.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crc32.cpp
)

set(SRC_OPENSSL_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/aes_openssl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/md5_openssl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/rsa_openssl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/sha_openssl.cpp
)

set(SRC_GNUTLS_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gnutls/aes_gnutls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gnutls/md5_gnutls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gnutls/rsa_gnutls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gnutls/sha_gnutls.cpp
)

set(SRC_MBEDTLS_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/aes_mbedtls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/md5_mbedtls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/rsa_mbedtls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/sha_mbedtls.cpp
)

set(SRC_WINDOWS_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win/sha_win.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Linux")
    add_definitions(-D_GNU_SOURCE -D__USE_FILE_OFFSET64 -D_LARGEFILE64_SOURCE)
    add_compile_options(-W -Wall -Werror -Wextra -Wno-deprecated -Wno-error=deprecated-declarations -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
endif()

if(BUILD_EXAMPLE MATCHES "ON")
    add_subdirectory(examples)
endif()

# build the static library
if(CRYPTOPP_STATIS_LIB MATCHES "ON")
    add_library(cryptopp_static STATIC ${SRC_LIST})
    add_library(eular::cryptopp_static ALIAS cryptopp_static)

    if(HAVE_WINDOWS_CRYPTO)
        target_sources(cryptopp_static PRIVATE ${SRC_WINDOWS_LIST})
        target_link_libraries(cryptopp_static PUBLIC bcrypt)
    elseif(HAVE_GNUTLS)
        target_sources(cryptopp_static PRIVATE ${SRC_GNUTLS_LIST})
        target_link_libraries(cryptopp_static PUBLIC Nettle::Nettle Nettle::Hogweed Nettle::Gmp)
    elseif(HAVE_OPENSSL)
        target_sources(cryptopp_static PRIVATE ${SRC_OPENSSL_LIST})
        target_link_libraries(cryptopp_static PUBLIC OpenSSL::Crypto)
    elseif(HAVE_MBEDTLS)
        target_sources(cryptopp_static PRIVATE ${SRC_MBEDTLS_LIST})
        target_link_libraries(cryptopp_static PUBLIC MbedTLS::Static)
    endif()
    target_link_libraries(cryptopp_static PUBLIC base64)

    target_include_directories(cryptopp_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/crypto>
            $<INSTALL_INTERFACE:include/crypto>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
    )

    target_compile_options (
        cryptopp_static PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties (
        cryptopp_static
        PROPERTIES
        LINK_FLAGS "-Wl,-z,now,-z,relro"
    )
    set_property(TARGET cryptopp_static PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if (NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL)
        install(TARGETS cryptopp_static
                RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
                ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
                LIBRARY DESTINATION "${INSTALL_LIB_DIR}")
    endif()
endif()

#build dynamic library
if(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
    add_library(cryptopp SHARED ${SRC_LIST})
    add_library(eular::cryptopp ALIAS cryptopp)

    target_include_directories(cryptopp
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/crypto>
            $<INSTALL_INTERFACE:include/crypto>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
    )

    if(HAVE_WINDOWS_CRYPTO)
        target_sources(cryptopp PRIVATE ${SRC_WINDOWS_LIST})
        target_link_libraries(cryptopp PUBLIC bcrypt)
    elseif(HAVE_GNUTLS)
        target_sources(cryptopp PRIVATE ${SRC_GNUTLS_LIST})
        target_link_libraries(cryptopp PUBLIC Nettle::Nettle Nettle::Hogweed Nettle::Gmp)
    elseif(HAVE_OPENSSL)
        target_sources(cryptopp PRIVATE ${SRC_OPENSSL_LIST})
        target_link_libraries(cryptopp PUBLIC OpenSSL::Crypto)
    elseif(HAVE_MBEDTLS)
        target_sources(cryptopp PRIVATE ${SRC_MBEDTLS_LIST})
        target_link_libraries(cryptopp PUBLIC MbedTLS::Shared)
    endif()
    target_link_libraries(cryptopp PUBLIC base64)

    target_compile_options(
        cryptopp PRIVATE
        ${CXX_COMPILER_FLAGS}
    )

    set_target_properties (
        cryptopp
        PROPERTIES
        LINK_FLAGS "-Wl,-z,now,-z,relro"
    )

    set_property(TARGET cryptopp PROPERTY LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL )
        install(TARGETS cryptopp
            RUNTIME DESTINATION "${INSTALL_BIN_DIR}"
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}" )
    endif()
endif()

if(NOT SKIP_INSTALL_HEADERS AND NOT SKIP_INSTALL_ALL)
    install(FILES ${HEADER_LIST} DESTINATION "${INSTALL_INC_DIR}/crypto")
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
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
