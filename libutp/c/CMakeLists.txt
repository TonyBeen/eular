cmake_minimum_required(VERSION 3.22)

project(libutp_c VERSION 1.0.0 LANGUAGES C)

include(CTest)

option(UTP_C_BUILD_TESTS "Build libutp C migration tests" ON)
option(UTP_C_BUILD_BENCHMARKS "Build libutp C migration benchmarks" OFF)
option(UTP_C_ENABLE_FORMAT_CHECK "Run the clang-format gate during the default build" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(utp_c STATIC
    src/allocator.c
    src/buffer.c
    src/error.c
    src/hash.c
    src/log.c
    src/range_set.c
    src/ring.c
    src/status.c
    src/utp.c
    include/utp/log.h
    include/utp/status.h
    include/utp/utp.h
    src/internal/allocator.h
    src/internal/buffer.h
    src/internal/error.h
    src/internal/hash.h
    src/internal/log.h
    src/internal/range_set.h
    src/internal/ring.h
)

add_library(eular::utp_c ALIAS utp_c)

target_compile_features(utp_c PUBLIC c_std_11)

target_include_directories(utp_c
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp_c PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
    )
elseif(MSVC)
    target_compile_options(utp_c PRIVATE /W4 /WX)
endif()

if(BUILD_TESTING AND UTP_C_BUILD_TESTS)
    add_executable(utp_c_api_test test/test_core.c)
    target_link_libraries(utp_c_api_test PRIVATE utp_c)
    target_include_directories(utp_c_api_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(utp_c_api_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
        )
    elseif(MSVC)
        target_compile_options(utp_c_api_test PRIVATE /W4 /WX)
    endif()
    add_test(NAME utp_c_api_test COMMAND utp_c_api_test)
    add_executable(utp_c_public_api_test test/test_public_api.c)
    target_link_libraries(utp_c_public_api_test PRIVATE utp_c)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(utp_c_public_api_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
        )
    elseif(MSVC)
        target_compile_options(utp_c_public_api_test PRIVATE /W4 /WX)
    endif()
    add_test(NAME utp_c_public_api_test COMMAND utp_c_public_api_test)
endif()

if(UTP_C_BUILD_BENCHMARKS)
    add_executable(utp_c_container_benchmark benchmark/bench_containers.c)
    target_link_libraries(utp_c_container_benchmark PRIVATE utp_c)
    target_include_directories(utp_c_container_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(utp_c_container_benchmark PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
        )
    elseif(MSVC)
        target_compile_options(utp_c_container_benchmark PRIVATE /W4 /WX)
    endif()
endif()

set(UTP_C_FORMAT_SOURCES
    benchmark/bench_containers.c
    include/utp/log.h
    include/utp/status.h
    include/utp/utp.h
    src/allocator.c
    src/buffer.c
    src/error.c
    src/hash.c
    src/log.c
    src/range_set.c
    src/ring.c
    src/status.c
    src/utp.c
    src/internal/allocator.h
    src/internal/buffer.h
    src/internal/error.h
    src/internal/hash.h
    src/internal/log.h
    src/internal/range_set.h
    src/internal/ring.h
    test/test_core.c
    test/test_public_api.c
)
list(TRANSFORM UTP_C_FORMAT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format
    HINTS
        "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
        "/Library/Developer/CommandLineTools/usr/bin"
)
if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(utp_c_format
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${UTP_C_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Formatting libutp C sources"
    )
    add_custom_target(utp_c_format_check
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file ${UTP_C_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking libutp C source formatting"
    )
    if(UTP_C_ENABLE_FORMAT_CHECK)
        add_custom_target(utp_c_format_gate ALL DEPENDS utp_c_format_check)
    endif()
elseif(UTP_C_ENABLE_FORMAT_CHECK)
    message(FATAL_ERROR "UTP_C_ENABLE_FORMAT_CHECK requires clang-format")
endif()
