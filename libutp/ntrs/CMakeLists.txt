cmake_minimum_required(VERSION 3.22)

project(libutp_ntrs_service LANGUAGES C CXX)

include(CTest)

option(UTP_NTRS_BUILD_TESTS "Build NTRS service unit tests" ON)
option(UTP_NTRS_BUILD_SERVICES "Build NTRS Hub and Node service executables" ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The NTRS Hub and Node service is only supported on Linux")
endif()

set(UTP_NTRS_SERVICE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../c/src/proto/proto.c"
    src/control.c
    src/control_stream.c
    src/hub.c
    src/node_link.c
    src/peer_manager.c
    src/service_util.c
    src/tls_stream.c)

add_library(utp_ntrs_service_core STATIC ${UTP_NTRS_SERVICE_SOURCES})
target_compile_features(utp_ntrs_service_core PUBLIC c_std_11)
target_include_directories(utp_ntrs_service_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(utp_ntrs_service_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../c/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../c/include")

set(UTP_NTRS_LIBEVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libevent/libevent_V2_1_10" CACHE PATH
    "Path to the libevent source tree")
set(UTP_NTRS_BORINGSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/boringssl" CACHE PATH
    "Path to the BoringSSL source tree")
if(NOT EXISTS "${UTP_NTRS_LIBEVENT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libevent source tree not found: ${UTP_NTRS_LIBEVENT_ROOT}")
endif()
if(NOT EXISTS "${UTP_NTRS_BORINGSSL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "BoringSSL source tree not found: ${UTP_NTRS_BORINGSSL_ROOT}")
endif()

set(EVENT__DISABLE_TESTS ON CACHE BOOL "Disable libevent tests for the NTRS service build")
set(EVENT__LIBRARY_TYPE STATIC CACHE STRING "Build libevent as a static library")
set(EVENT__DISABLE_OPENSSL ON CACHE BOOL "Use BoringSSL directly in NTRS instead of libevent system OpenSSL support" FORCE)
add_subdirectory("${UTP_NTRS_LIBEVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent" EXCLUDE_FROM_ALL)

set(_utp_ntrs_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "Disable BoringSSL tests for the NTRS service build" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build BoringSSL statically for the NTRS service build" FORCE)
set(INSTALL_ENABLED OFF CACHE BOOL "Disable BoringSSL installation for the NTRS service build" FORCE)
add_subdirectory("${UTP_NTRS_BORINGSSL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/boringssl" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_utp_ntrs_build_testing}" CACHE BOOL "Build CTest targets" FORCE)

target_include_directories(utp_ntrs_service_core PRIVATE "${UTP_NTRS_BORINGSSL_ROOT}/include")

find_package(Threads REQUIRED)
add_library(utp_ntrs_service_runtime INTERFACE)
target_link_libraries(utp_ntrs_service_runtime INTERFACE event_core event_pthreads Threads::Threads)

add_library(utp_ntrs_node_udp STATIC src/udp_probe.c)
target_compile_features(utp_ntrs_node_udp PUBLIC c_std_11)
target_include_directories(utp_ntrs_node_udp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(utp_ntrs_node_udp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../c/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../c/include")
target_link_libraries(utp_ntrs_node_udp PUBLIC utp_ntrs_service_core utp_ntrs_service_runtime)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(utp_ntrs_service_core PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
    target_compile_options(utp_ntrs_node_udp PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
elseif(MSVC)
    target_compile_options(utp_ntrs_service_core PRIVATE /utf-8 /W4 /WX)
    target_compile_options(utp_ntrs_node_udp PRIVATE /utf-8 /W4 /WX)
endif()

if(UTP_NTRS_BUILD_SERVICES)
    add_executable(nat_detect_hub app/nat_detect_hub.c)
    target_link_libraries(nat_detect_hub PRIVATE
        utp_ntrs_service_core
        utp_ntrs_service_runtime
        -Wl,--start-group
        ssl
        crypto
        -Wl,--end-group)
    target_include_directories(nat_detect_hub PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nat_detect_hub PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
    elseif(MSVC)
        target_compile_options(nat_detect_hub PRIVATE /utf-8 /W4 /WX)
    endif()

    add_executable(nat_detect_node app/nat_detect_node.c)
    target_link_libraries(nat_detect_node PRIVATE
        utp_ntrs_service_runtime
        -Wl,--start-group
        $<TARGET_FILE:utp_ntrs_node_udp>
        $<TARGET_FILE:utp_ntrs_service_core>
        ssl
        crypto
        -Wl,--end-group)
    add_dependencies(nat_detect_node utp_ntrs_node_udp utp_ntrs_service_core)
    target_include_directories(nat_detect_node PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nat_detect_node PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
    elseif(MSVC)
        target_compile_options(nat_detect_node PRIVATE /utf-8 /W4 /WX)
    endif()
endif()

if(BUILD_TESTING AND UTP_NTRS_BUILD_TESTS)
    add_executable(utp_ntrs_service_test test/test_service.c)
    target_link_libraries(utp_ntrs_service_test PRIVATE utp_ntrs_node_udp)
    target_include_directories(utp_ntrs_service_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../c/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../c/include")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(utp_ntrs_service_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -UNDEBUG)
    elseif(MSVC)
        target_compile_options(utp_ntrs_service_test PRIVATE /utf-8 /W4 /WX /UNDEBUG)
    endif()
    add_test(NAME utp_ntrs_service_test COMMAND utp_ntrs_service_test)
endif()
