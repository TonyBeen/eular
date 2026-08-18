cmake_minimum_required(VERSION 3.22)

project(libutp_c VERSION 1.6.0 LANGUAGES C CXX)

include(CTest)
include(CheckSymbolExists)

option(UTP_C_BUILD_TESTS "Build libutp C migration tests" ON)
option(UTP_C_BUILD_BENCHMARKS "Build libutp C migration benchmarks" OFF)
option(UTP_C_BUILD_EXAMPLES "Build libutp C examples" OFF)
option(UTP_C_ENABLE_FORMAT_CHECK "Run the clang-format gate during the default build" OFF)
option(UTP_C_ENABLE_SANITIZERS "Instrument libutp C and its tests with AddressSanitizer and UBSan" OFF)
option(UTP_C_BUILD_FUZZERS "Build libFuzzer targets for packet-facing parsers" OFF)
set(UTP_C_SANITIZER_LIST "address,undefined" CACHE STRING "Comma-separated Clang/GCC sanitizers")

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

set(UTP_C_SOURCES
    src/util/allocator.c src/proto/ack.c src/socket/address.c src/util/buffer.c src/crypto/crypto.c src/crypto/token.c src/util/error.c
    src/context/event_loop.c src/proto/frame.c src/util/hash.c src/util/log.c src/util/range_set.c src/util/ring.c
    src/status.c src/util/time.c src/socket/udp.c src/proto/proto.c src/context/ack_scheduler.c src/util/send_history.c
    src/util/rtt.c src/util/receive_history.c src/proto/packet_out.c src/proto/packet_in.c src/context/send_ledger.c
    src/context/send_control.c src/context/pending_incoming.c src/congestion/bbr.c src/congestion/bw_sampler.c src/congestion/congestion.c src/congestion/cubic.c
    src/congestion/minmax.c src/congestion/pacer.c src/mtu/mtu.c
    src/connection/stream.c src/connection/connection.c src/context/context.c src/utp.c)
set(UTP_C_HEADERS
    include/utp/connection.h include/utp/context.h include/utp/log.h include/utp/option.h include/utp/status.h include/utp/stream.h include/utp/utp.h src/util/allocator.h src/proto/ack.h src/socket/address.h
    src/util/buffer.h src/crypto/crypto.h src/crypto/token.h src/util/error.h src/context/event_loop.h src/proto/frame.h src/util/hash.h
    src/util/log.h src/util/range_set.h src/util/ring.h src/util/time.h src/socket/udp.h src/proto/proto.h
    src/util/receive_history.h src/context/ack_scheduler.h src/util/send_history.h src/util/rtt.h src/proto/packet_out.h src/proto/packet_in.h
    src/proto/wire.h src/context/send_ledger.h src/context/send_control.h src/context/pending_incoming.h src/congestion/congestion.h
    src/congestion/bbr.h src/congestion/bw_sampler.h src/congestion/cubic.h src/congestion/minmax.h src/congestion/pacer.h src/context/context.h)
list(APPEND UTP_C_HEADERS src/connection/stream.h src/connection/connection.h src/mtu/mtu.h)
add_library(utp_c STATIC ${UTP_C_SOURCES} ${UTP_C_HEADERS})
add_library(eular::utp_c ALIAS utp_c)
set_target_properties(utp_c PROPERTIES OUTPUT_NAME utp)
target_link_libraries(utp_c PRIVATE event_core crypto)
target_compile_definitions(utp_c PRIVATE ${UTP_C_SOCKET_BATCH_DEFINITIONS})
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
    "$<INSTALL_INTERFACE:include>" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../3rd")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp_c PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
elseif(MSVC)
    target_compile_options(utp_c PRIVATE /utf-8 /W4 /WX)
endif()

function(utp_c_configure_c_test target)
    target_link_libraries(${target} PRIVATE utp_c event_core)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(UTP_C_SANITIZER_FLAGS)
        target_compile_options(${target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
        target_link_options(${target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -UNDEBUG)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX /UNDEBUG)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(utp_c_configure_cpp_test target)
    target_link_libraries(${target} PRIVATE utp_c event_core)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../cpp/test"
        "${CMAKE_CURRENT_SOURCE_DIR}/../3rd")
    target_compile_definitions(${target} PRIVATE ${UTP_C_SOCKET_BATCH_DEFINITIONS})
    if(UTP_C_SANITIZER_FLAGS)
        target_compile_options(${target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
        target_link_options(${target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wformat=2 -UNDEBUG)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX /EHsc /UNDEBUG)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

if(BUILD_TESTING AND UTP_C_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_executable(utp_c_api_test test/test_core.c)
    utp_c_configure_c_test(utp_c_api_test)
    add_executable(utp_c_public_api_test test/test_public_api.c)
    utp_c_configure_c_test(utp_c_public_api_test)
    target_include_directories(utp_c_public_api_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../3rd")
    foreach(test_name IN ITEMS proto crypto receive_history packet_out packet_in send_ledger send_control pending_incoming stream connection congestion minmax bw_sampler bbr mtu parser_fuzz transport_integration)
        add_executable(utp_c_${test_name}_test test/test_${test_name}.cc)
        utp_c_configure_cpp_test(utp_c_${test_name}_test)
    endforeach()
    target_link_libraries(utp_c_transport_integration_test PRIVATE Threads::Threads)
    if(APPLE)
        add_subdirectory(test/socket_fault)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND UTP_C_SOCKET_BATCH_DEFINITIONS)
        add_subdirectory(test/socket_batch_fault)
    endif()
endif()

if(UTP_C_BUILD_FUZZERS)
    add_executable(utp_c_packet_parser_fuzzer test/fuzz/packet_parser_fuzz.cc)
    target_link_libraries(utp_c_packet_parser_fuzzer PRIVATE utp_c)
    target_include_directories(utp_c_packet_parser_fuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(utp_c_packet_parser_fuzzer PRIVATE ${UTP_C_FUZZER_FLAGS})
    target_link_options(utp_c_packet_parser_fuzzer PRIVATE ${UTP_C_FUZZER_FLAGS})
    add_custom_target(utp_c_fuzz_packet_parser
        COMMAND $<TARGET_FILE:utp_c_packet_parser_fuzzer> -runs=100000 -max_len=2048
        DEPENDS utp_c_packet_parser_fuzzer
        COMMENT "Running the packet parser libFuzzer target")
endif()

if(UTP_C_BUILD_BENCHMARKS)
    add_executable(utp_c_container_benchmark benchmark/bench_containers.c)
    utp_c_configure_c_test(utp_c_container_benchmark)
    add_executable(utp_c_concurrent_connection_benchmark benchmark/bench_concurrent_connections.c)
    utp_c_configure_c_test(utp_c_concurrent_connection_benchmark)
endif()

if(UTP_C_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

set(UTP_C_FORMAT_SOURCES benchmark/bench_containers.c benchmark/bench_concurrent_connections.c examples/echo_client.c examples/echo_server.c ${UTP_C_HEADERS}
    ${UTP_C_SOURCES}
    test/test_core.c test/test_public_api.c test/test_crypto.cc test/test_proto.cc test/test_receive_history.cc
    test/test_packet_out.cc test/test_packet_in.cc test/test_send_ledger.cc test/test_send_control.cc test/test_pending_incoming.cc test/test_congestion.cc test/test_minmax.cc
    test/test_stream.cc test/test_connection.cc test/test_bw_sampler.cc test/test_bbr.cc test/test_mtu.cc test/test_parser_fuzz.cc
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
