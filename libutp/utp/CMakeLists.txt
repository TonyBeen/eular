cmake_minimum_required(VERSION 3.22)

include(cmake/configure_version.cmake)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(libutp_c VERSION ${UTP_C_VERSION_STRING} LANGUAGES C CXX)
endif()

include(GNUInstallDirs)

set(UTP_C_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${UTP_C_GENERATED_INCLUDE_DIR}")
configure_file(cmake/utp_version.h.in "${UTP_C_GENERATED_INCLUDE_DIR}/utp_version.h" @ONLY NEWLINE_STYLE LF)

include(CTest)
include(CheckSymbolExists)

option(UTP_C_BUILD_TESTS "Build libutp C migration tests" OFF)
option(UTP_C_BUILD_BENCHMARKS "Build libutp C migration benchmarks" OFF)
option(UTP_C_BUILD_EXAMPLES "Build libutp C examples" ON)
option(UTP_C_BUILD_SHARED "Build libutp C as a shared library" OFF)
option(UTP_C_ENABLE_FORMAT_CHECK "Run the clang-format gate during the default build" OFF)
option(UTP_C_ENABLE_SANITIZERS "Instrument libutp C and its tests with AddressSanitizer and UBSan" OFF)
option(UTP_C_BUILD_FUZZERS "Build libFuzzer targets for packet-facing parsers" OFF)
set(UTP_C_SANITIZER_LIST "address,undefined" CACHE STRING "Comma-separated Clang/GCC sanitizers")
set(UTP_C_NTRS_EXECUTABLE "" CACHE FILEPATH "Path to the Linux NTRS executable used by integration tests")

set(UTP_C_SANITIZER_FLAGS)
if(UTP_C_ENABLE_SANITIZERS OR UTP_C_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "UTP_C sanitizer and fuzz builds require Clang or GCC; libFuzzer additionally requires Clang")
    endif()
    if(UTP_C_SANITIZER_LIST STREQUAL "")
        message(FATAL_ERROR "UTP_C_SANITIZER_LIST must not be empty when sanitizers or fuzzers are enabled")
    endif()
    set(UTP_C_SANITIZER_FLAGS "-fsanitize=${UTP_C_SANITIZER_LIST}" -fno-omit-frame-pointer)
endif()
if(UTP_C_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "UTP_C_BUILD_FUZZERS requires a Clang C++ compiler with libFuzzer support")
endif()
if(UTP_C_BUILD_FUZZERS)
    set(UTP_C_FUZZER_FLAGS "-fsanitize=fuzzer,${UTP_C_SANITIZER_LIST}" -fno-omit-frame-pointer)
    include(CheckCXXSourceCompiles)
    string(JOIN " " _utp_c_fuzzer_check_flags ${UTP_C_FUZZER_FLAGS})
    set(_utp_c_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${_utp_c_fuzzer_check_flags}")
    check_cxx_source_compiles(
        "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
        UTP_C_HAS_LIBFUZZER_RUNTIME)
    set(CMAKE_REQUIRED_FLAGS "${_utp_c_required_flags}")
    if(NOT UTP_C_HAS_LIBFUZZER_RUNTIME)
        message(FATAL_ERROR "UTP_C_BUILD_FUZZERS requires a Clang toolchain that provides the libFuzzer runtime")
    endif()
endif()

set(UTP_C_LIBEVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libevent/libevent_V2_1_10" CACHE PATH
    "Path to the C libevent source tree")
if(NOT EXISTS "${UTP_C_LIBEVENT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "C libevent source tree not found: ${UTP_C_LIBEVENT_ROOT}")
endif()
set(EVENT__DISABLE_TESTS ON CACHE BOOL "Disable libevent tests for the libutp C build")
set(EVENT__LIBRARY_TYPE STATIC CACHE STRING "Build libevent as a static library")
add_subdirectory("${UTP_C_LIBEVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent-core" EXCLUDE_FROM_ALL)

set(UTP_C_BORINGSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/boringssl" CACHE PATH
    "Path to the bundled BoringSSL source tree")
if(NOT EXISTS "${UTP_C_BORINGSSL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "BoringSSL source tree not found: ${UTP_C_BORINGSSL_ROOT}")
endif()
set(_utp_c_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "Disable BoringSSL tests for the libutp C build" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build BoringSSL statically for the libutp C build" FORCE)
set(INSTALL_ENABLED OFF CACHE BOOL "Disable BoringSSL installation for the libutp C build" FORCE)
add_subdirectory("${UTP_C_BORINGSSL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/boringssl" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_utp_c_build_testing}" CACHE BOOL "Build CTest targets" FORCE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UTP_C_SOCKET_BATCH_DEFINITIONS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_utp_c_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
    list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")
    check_symbol_exists(sendmmsg "sys/socket.h" UTP_C_HAVE_SENDMMSG)
    check_symbol_exists(recvmmsg "sys/socket.h" UTP_C_HAVE_RECVMMSG)
    set(CMAKE_REQUIRED_DEFINITIONS "${_utp_c_required_definitions}")
    if(UTP_C_HAVE_SENDMMSG)
        list(APPEND UTP_C_SOCKET_BATCH_DEFINITIONS UTP_HAVE_SENDMMSG=1)
    endif()
    if(UTP_C_HAVE_RECVMMSG)
        list(APPEND UTP_C_SOCKET_BATCH_DEFINITIONS UTP_HAVE_RECVMMSG=1)
    endif()
endif()

include(cmake/utp_c_sources.cmake)
if(UTP_C_BUILD_SHARED)
    set(UTP_C_LIBRARY_TYPE SHARED)
else()
    set(UTP_C_LIBRARY_TYPE STATIC)
endif()
add_library(utp_c ${UTP_C_LIBRARY_TYPE} ${UTP_C_SOURCES} ${UTP_C_HEADERS})
add_library(eular::utp_c ALIAS utp_c)
set_target_properties(utp_c PROPERTIES OUTPUT_NAME utp)
target_link_libraries(utp_c PRIVATE $<BUILD_INTERFACE:event_core> $<BUILD_INTERFACE:crypto>)
target_compile_definitions(utp_c PRIVATE UTP_C_BUILD ${UTP_C_SOCKET_BATCH_DEFINITIONS})
if(NOT UTP_C_BUILD_SHARED)
    target_compile_definitions(utp_c PUBLIC UTP_C_STATIC)
endif()
if(UTP_C_SANITIZER_FLAGS)
    target_compile_options(utp_c PRIVATE ${UTP_C_SANITIZER_FLAGS})
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(utp_c PRIVATE m)
endif()
if(WIN32)
    target_link_libraries(utp_c PRIVATE ws2_32 iphlpapi)
endif()
target_compile_features(utp_c PUBLIC c_std_11)
target_include_directories(utp_c PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../3rd"
    "${UTP_C_GENERATED_INCLUDE_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp_c PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
elseif(MSVC)
    target_compile_options(utp_c PRIVATE /utf-8 /W4 /WX)
endif()

include(cmake/utp_c_tests.cmake)

set(UTP_C_FORMAT_SOURCES benchmark/bench_containers.c benchmark/bench_concurrent_connections.c examples/echo_client.c examples/echo_server.c ${UTP_C_HEADERS}
    ${UTP_C_SOURCES}
    test/test_core.c test/test_public_api.c test/test_crypto.cc test/test_proto.cc test/test_receive_history.cc
    test/test_packet_out.cc test/test_packet_in.cc test/test_send_ledger.cc test/test_send_control.cc test/test_pending_incoming.cc test/test_congestion.cc test/test_minmax.cc
    test/connection_test_util.h test/test_stream.cc test/test_connection.cc test/test_bw_sampler.cc test/test_bbr.cc test/test_mtu.cc test/test_nat.cc test/test_parser_fuzz.cc
    test/test_transport_integration.cc test/fuzz/packet_parser_fuzz.cc test/socket_batch_fault/socket_batch_hook.c
    test/socket_batch_fault/socket_batch_hook.h)
list(TRANSFORM UTP_C_FORMAT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format
    HINTS "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
    "/Library/Developer/CommandLineTools/usr/bin")
if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(utp_c_format COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${UTP_C_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" COMMENT "Formatting libutp C sources")
    add_custom_target(utp_c_format_check COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file
        ${UTP_C_FORMAT_SOURCES} WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" COMMENT "Checking libutp C source formatting")
    if(UTP_C_ENABLE_FORMAT_CHECK)
        add_custom_target(utp_c_format_gate ALL DEPENDS utp_c_format_check)
    endif()
elseif(UTP_C_ENABLE_FORMAT_CHECK)
    message(FATAL_ERROR "UTP_C_ENABLE_FORMAT_CHECK requires clang-format")
endif()

install(TARGETS utp_c
    EXPORT libutpCTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${UTP_C_GENERATED_INCLUDE_DIR}/utp_version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/utp")
install(EXPORT libutpCTargets
    FILE libutpCTargets.cmake
    NAMESPACE eular::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libutp")
