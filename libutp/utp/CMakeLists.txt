cmake_minimum_required(VERSION 3.10)

include(cmake/configure_version.cmake)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(libutp VERSION ${UTP_VERSION_STRING} LANGUAGES C CXX)
endif()

include(GNUInstallDirs)

set(UTP_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${UTP_GENERATED_INCLUDE_DIR}")
configure_file(cmake/utp_version.h.in "${UTP_GENERATED_INCLUDE_DIR}/utp_version.h" @ONLY NEWLINE_STYLE LF)

include(CTest)
include(CheckSymbolExists)

option(UTP_BUILD_TESTS "Build libutp tests" OFF)
option(UTP_BUILD_BENCHMARKS "Build libutp benchmarks" OFF)
option(UTP_BUILD_EXAMPLES "Build libutp examples" ON)
option(UTP_BUILD_SHARED "Build libutp as a shared library" OFF)
option(UTP_ENABLE_FORMAT_CHECK "Run the clang-format gate during the default build" OFF)
option(UTP_ENABLE_SANITIZERS "Instrument libutp and its tests with AddressSanitizer and UBSan" OFF)
option(UTP_BUILD_FUZZERS "Build libFuzzer targets for packet-facing parsers" OFF)
set(UTP_SANITIZER_LIST "address,undefined" CACHE STRING "Comma-separated Clang/GCC sanitizers")
set(UTP_NTRS_EXECUTABLE "" CACHE FILEPATH "Path to the Linux NTRS executable used by integration tests")

set(UTP_SANITIZER_FLAGS)
if(UTP_ENABLE_SANITIZERS OR UTP_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "UTP sanitizer and fuzz builds require Clang or GCC; libFuzzer additionally requires Clang")
    endif()
    if(UTP_SANITIZER_LIST STREQUAL "")
        message(FATAL_ERROR "UTP_SANITIZER_LIST must not be empty when sanitizers or fuzzers are enabled")
    endif()
    set(UTP_SANITIZER_FLAGS "-fsanitize=${UTP_SANITIZER_LIST}" -fno-omit-frame-pointer)
endif()
if(UTP_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "UTP_BUILD_FUZZERS requires a Clang C++ compiler with libFuzzer support")
endif()
if(UTP_BUILD_FUZZERS)
    set(UTP_FUZZER_FLAGS "-fsanitize=fuzzer,${UTP_SANITIZER_LIST}" -fno-omit-frame-pointer)
    include(CheckCXXSourceCompiles)
    string(REPLACE ";" " " _utp_fuzzer_check_flags "${UTP_FUZZER_FLAGS}")
    set(_utp_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${_utp_fuzzer_check_flags}")
    check_cxx_source_compiles(
        "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
        UTP_HAS_LIBFUZZER_RUNTIME)
    set(CMAKE_REQUIRED_FLAGS "${_utp_required_flags}")
    if(NOT UTP_HAS_LIBFUZZER_RUNTIME)
        message(FATAL_ERROR "UTP_BUILD_FUZZERS requires a Clang toolchain that provides the libFuzzer runtime")
    endif()
endif()

set(UTP_LIBEVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libevent/libevent_V2_1_10" CACHE PATH
    "Path to the libevent source tree")
if(NOT EXISTS "${UTP_LIBEVENT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libevent source tree not found: ${UTP_LIBEVENT_ROOT}")
endif()
set(EVENT__DISABLE_TESTS ON CACHE BOOL "Disable libevent tests for the libutp build")
set(EVENT__LIBRARY_TYPE STATIC CACHE STRING "Build libevent as a static library")
add_subdirectory("${UTP_LIBEVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent-core" EXCLUDE_FROM_ALL)

set(UTP_BORINGSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/boringssl" CACHE PATH
    "Path to the bundled BoringSSL source tree")
if(NOT EXISTS "${UTP_BORINGSSL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "BoringSSL source tree not found: ${UTP_BORINGSSL_ROOT}")
endif()
set(_utp_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "Disable BoringSSL tests for the libutp build" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build BoringSSL statically for the libutp build" FORCE)
set(INSTALL_ENABLED OFF CACHE BOOL "Disable BoringSSL installation for the libutp build" FORCE)
add_subdirectory("${UTP_BORINGSSL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/boringssl" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_utp_build_testing}" CACHE BOOL "Build CTest targets" FORCE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UTP_SOCKET_BATCH_DEFINITIONS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_utp_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
    list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")
    check_symbol_exists(sendmmsg "sys/socket.h" UTP_HAVE_SENDMMSG)
    check_symbol_exists(recvmmsg "sys/socket.h" UTP_HAVE_RECVMMSG)
    set(CMAKE_REQUIRED_DEFINITIONS "${_utp_required_definitions}")
    if(UTP_HAVE_SENDMMSG)
        list(APPEND UTP_SOCKET_BATCH_DEFINITIONS UTP_HAVE_SENDMMSG=1)
    endif()
    if(UTP_HAVE_RECVMMSG)
        list(APPEND UTP_SOCKET_BATCH_DEFINITIONS UTP_HAVE_RECVMMSG=1)
    endif()
endif()

include(cmake/utp_sources.cmake)
if(UTP_BUILD_SHARED)
    set(UTP_LIBRARY_TYPE SHARED)
else()
    set(UTP_LIBRARY_TYPE STATIC)
endif()
add_library(utp ${UTP_LIBRARY_TYPE} ${UTP_SOURCES} ${UTP_HEADERS})
add_library(eular::utp ALIAS utp)
set_target_properties(utp PROPERTIES OUTPUT_NAME utp)
target_link_libraries(utp PRIVATE $<BUILD_INTERFACE:event_core> $<BUILD_INTERFACE:crypto>)
target_compile_definitions(utp PRIVATE UTP_BUILD ${UTP_SOCKET_BATCH_DEFINITIONS})
if(NOT UTP_BUILD_SHARED)
    target_compile_definitions(utp PUBLIC UTP_STATIC)
endif()
if(UTP_SANITIZER_FLAGS)
    target_compile_options(utp PRIVATE ${UTP_SANITIZER_FLAGS})
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(utp PRIVATE m)
endif()
if(WIN32)
    target_link_libraries(utp PRIVATE ws2_32 iphlpapi)
endif()
target_compile_features(utp PUBLIC c_std_11)
target_include_directories(utp PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../3rd"
    "${UTP_GENERATED_INCLUDE_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
elseif(MSVC)
    target_compile_options(utp PRIVATE /utf-8 /W4 /WX)
endif()

add_subdirectory(test)

if(UTP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

include(cmake/utp_format.cmake)

install(TARGETS utp
    EXPORT libutpTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${UTP_GENERATED_INCLUDE_DIR}/utp_version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/utp")
install(EXPORT libutpTargets
    FILE libutpTargets.cmake
    NAMESPACE eular::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libutp")
