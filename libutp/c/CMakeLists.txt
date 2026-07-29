cmake_minimum_required(VERSION 3.22)

project(libutp_c VERSION 1.0.0 LANGUAGES C CXX)

include(CTest)

option(UTP_C_BUILD_TESTS "Build libutp C migration tests" ON)
option(UTP_C_BUILD_BENCHMARKS "Build libutp C migration benchmarks" OFF)
option(UTP_C_ENABLE_FORMAT_CHECK "Run the clang-format gate during the default build" OFF)

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

add_library(utp_c STATIC
    src/allocator.c src/ack.c src/address.c src/buffer.c src/crypto.c src/error.c src/event_loop.c src/frame.c
    src/hash.c src/log.c src/range_set.c src/ring.c src/status.c src/time.c src/udp.c src/proto.c src/ack_scheduler.c src/send_history.c src/rtt.c
    src/receive_history.c src/utp.c
    include/utp/log.h include/utp/status.h include/utp/utp.h
    src/internal/allocator.h src/internal/ack.h src/internal/address.h src/internal/buffer.h src/internal/crypto.h
    src/internal/error.h src/internal/event_loop.h src/internal/frame.h src/internal/hash.h src/internal/log.h
    src/internal/range_set.h src/internal/ring.h src/internal/time.h src/internal/udp.h src/internal/proto.h
    src/internal/receive_history.h src/internal/ack_scheduler.h src/internal/send_history.h src/internal/rtt.h src/internal/wire.h
)

add_library(eular::utp_c ALIAS utp_c)
target_link_libraries(utp_c PRIVATE event_core crypto)
target_compile_features(utp_c PUBLIC c_std_11)
target_include_directories(utp_c PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp_c PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
elseif(MSVC)
    target_compile_options(utp_c PRIVATE /W4 /WX)
endif()

function(utp_c_configure_c_test target)
    target_link_libraries(${target} PRIVATE utp_c)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -UNDEBUG)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /UNDEBUG)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(utp_c_configure_cpp_test target)
    target_link_libraries(${target} PRIVATE utp_c)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../cpp/test")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wformat=2 -UNDEBUG)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /UNDEBUG)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

if(BUILD_TESTING AND UTP_C_BUILD_TESTS)
    add_executable(utp_c_api_test test/test_core.c)
    utp_c_configure_c_test(utp_c_api_test)
    add_executable(utp_c_public_api_test test/test_public_api.c)
    utp_c_configure_c_test(utp_c_public_api_test)
    add_executable(utp_c_proto_test test/test_proto.cc)
    utp_c_configure_cpp_test(utp_c_proto_test)
    add_executable(utp_c_crypto_test test/test_crypto.cc)
    utp_c_configure_cpp_test(utp_c_crypto_test)
    add_executable(utp_c_receive_history_test test/test_receive_history.cc)
    utp_c_configure_cpp_test(utp_c_receive_history_test)
endif()

if(UTP_C_BUILD_BENCHMARKS)
    add_executable(utp_c_container_benchmark benchmark/bench_containers.c)
    utp_c_configure_c_test(utp_c_container_benchmark)
endif()

set(UTP_C_FORMAT_SOURCES
    benchmark/bench_containers.c include/utp/log.h include/utp/status.h include/utp/utp.h
    src/allocator.c src/ack.c src/address.c src/buffer.c src/crypto.c src/error.c src/event_loop.c src/frame.c
    src/hash.c src/log.c src/range_set.c src/ring.c src/status.c src/time.c src/udp.c src/proto.c src/ack_scheduler.c src/send_history.c src/rtt.c
    src/receive_history.c src/utp.c
    src/internal/allocator.h src/internal/ack.h src/internal/address.h src/internal/buffer.h src/internal/crypto.h
    src/internal/error.h src/internal/event_loop.h src/internal/frame.h src/internal/hash.h src/internal/log.h
    src/internal/range_set.h src/internal/ring.h src/internal/time.h src/internal/udp.h src/internal/proto.h
    src/internal/receive_history.h src/internal/ack_scheduler.h src/internal/send_history.h src/internal/rtt.h src/internal/wire.h
    test/test_core.c test/test_public_api.c test/test_crypto.cc test/test_proto.cc test/test_receive_history.cc
)
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
