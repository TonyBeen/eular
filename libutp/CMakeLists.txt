cmake_minimum_required(VERSION 3.22)

project(libutp VERSION 1.2.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Add /utf-8 and /FS compiler options for MSVC
# /utf-8: support Unicode in fmt library
# /FS: allow multiple CL.EXE processes to write to the same PDB file during parallel compilation
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:/FS> $<$<COMPILE_LANGUAGE:CXX>:/FS>)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /FS")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /FS")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(UTP_BUILD_TESTS "Build libutp tests" OFF)
option(UTP_USE_BUNDLED_BORINGSSL "Use bundled BoringSSL submodule" ON)
option(UTP_ENABLE_FAULT_INJECTION "Enable Linux-only libfiu fault injection hooks" OFF)

if(UTP_ENABLE_FAULT_INJECTION AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "UTP_ENABLE_FAULT_INJECTION is Linux-only, disabling on ${CMAKE_SYSTEM_NAME}")
    set(UTP_ENABLE_FAULT_INJECTION OFF CACHE BOOL "Enable Linux-only libfiu fault injection hooks" FORCE)
endif()

if(UTP_ENABLE_FAULT_INJECTION)
    find_path(FIU_INCLUDE_DIR NAMES fiu.h)
    find_library(FIU_LIBRARY NAMES fiu)
    if(NOT FIU_INCLUDE_DIR OR NOT FIU_LIBRARY)
        message(FATAL_ERROR "UTP_ENABLE_FAULT_INJECTION requires libfiu headers and library (fiu.h, -lfiu)")
    endif()
endif()

set(UTP_UTILS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../utils" CACHE PATH "Path to the sibling utils project")
set(UTP_EVENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libevent" CACHE PATH "Path to the sibling libevent wrapper project")

if(NOT EXISTS "${UTP_UTILS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "utils project not found: ${UTP_UTILS_ROOT}")
endif()

if(NOT EXISTS "${UTP_EVENT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libevent wrapper project not found: ${UTP_EVENT_ROOT}")
endif()

if(UTP_USE_BUNDLED_BORINGSSL AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rd/boringssl/CMakeLists.txt")
    message(FATAL_ERROR "BoringSSL submodule not found: ${CMAKE_CURRENT_SOURCE_DIR}/3rd/boringssl")
endif()

set(SUBMODULE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UTILS_SUBMODULE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UTILS_STATIC_LIB ON CACHE BOOL "" FORCE)
set(UTILS_DYNAMIC_LIB OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${UTP_UTILS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/utils")

set(EVENT_SUBMODULE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EVENT_STATIC_LIB ON CACHE BOOL "" FORCE)
set(EVENT_DYNAMIC_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${UTP_EVENT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libevent")

if(UTP_USE_BUNDLED_BORINGSSL)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(INSTALL_ENABLED OFF CACHE BOOL "" FORCE)
    set(_utp_prev_skip_install_rules ${CMAKE_SKIP_INSTALL_RULES})
    set(CMAKE_SKIP_INSTALL_RULES ON)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rd/boringssl" "${CMAKE_CURRENT_BINARY_DIR}/_deps/boringssl" EXCLUDE_FROM_ALL)
    set(CMAKE_SKIP_INSTALL_RULES ${_utp_prev_skip_install_rules})

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(crypto PRIVATE -Wno-error)
        target_compile_options(ssl PRIVATE -Wno-error)
    endif()
endif()

set(UTP_VERSION_STRING ${PROJECT_VERSION})
set(UTP_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(UTP_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(UTP_VERSION_PATCH ${PROJECT_VERSION_PATCH})

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY
)

set(UTP_PUBLIC_HEADERS
    include/utp/config.h
    include/utp/connection.h
    include/utp/errno.h
    include/utp/logger.h
    include/utp/platform.h
    include/utp/stream.h
    include/utp/types.h
    include/utp/utp.h
)

set(UTP_SOURCES
    3rd/rbtree.c
    src/congestion/bbr_v1.cpp
    src/congestion/bw_sampler.cpp
    src/congestion/cubic.cpp
    src/congestion/minmax.cpp
    src/congestion/pacer.cpp
    src/context/connection_impl.cpp
    src/context/context_impl.cpp
    src/context/detail/frame_meta_policy.cpp
    src/context/detail/packet_editor.cpp
    src/context/packet_decode_helper.cpp
    src/context/send_ctl.cpp
    src/context/stream_impl.cpp
    src/crypto/aes_gcm_context.cpp
    src/crypto/base64.cpp
    src/crypto/resumption_state_codec.cpp
    src/crypto/token.cpp
    src/crypto/x25519_wrapper.cpp
    src/logger/logger.cpp
    src/mtu/mtu.cpp
    src/proto/frame/ack.cpp
    src/proto/frame/ack_frequency.cpp
    src/proto/frame/connection_close.cpp
    src/proto/frame/crypto.cpp
    src/proto/frame/handshake_delay.cpp
    src/proto/frame/handshake_done.cpp
    src/proto/frame/handshake_helper.cpp
    src/proto/frame/padding.cpp
    src/proto/frame/path.cpp
    src/proto/frame/reset_stream.cpp
    src/proto/frame/session_token.cpp
    src/proto/frame/stream.cpp
    src/proto/frame/transport_params.cpp
    src/proto/frame/version.cpp
    src/proto/frame.cpp
    src/proto/packet_in.cpp
    src/proto/packet_out.cpp
    src/socket/address.cpp
    src/socket/mmsg.cpp
    src/socket/packet.cpp
    src/socket/udp.cpp
    src/socket/util.cpp
    src/util/ack_info.cpp
    src/util/errno.cpp
    src/util/error.cpp
    src/util/mm.cpp
    src/util/network_path.cpp
    src/util/ring_buffer.cpp
    src/util/receive_history.cpp
    src/util/send_history.cpp
    src/util/time.cpp
    src/util/transport_param.cpp
    src/util/util.cpp
    src/utp.cpp
)

add_library(utp_static STATIC ${UTP_SOURCES} ${UTP_PUBLIC_HEADERS})
add_library(eular::utp ALIAS utp_static)

target_compile_definitions(utp_static
    PUBLIC
        UTP_STATIC
        $<$<BOOL:${MSVC}>:NOMINMAX>
)

if(UTP_ENABLE_FAULT_INJECTION)
    target_compile_definitions(utp_static PRIVATE UTP_ENABLE_FAULT_INJECTION)
endif()

target_include_directories(utp_static
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rd"
        $<$<BOOL:${UTP_ENABLE_FAULT_INJECTION}>:${FIU_INCLUDE_DIR}>
)

target_link_libraries(utp_static
    PUBLIC
        $<BUILD_INTERFACE:utils_static>
        $<BUILD_INTERFACE:event_wrapper_static>
        $<BUILD_INTERFACE:crypto>
        $<BUILD_INTERFACE:ssl>
)

if(UTP_ENABLE_FAULT_INJECTION)
    target_link_libraries(utp_static PUBLIC $<BUILD_INTERFACE:${FIU_LIBRARY}>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(utp_static PUBLIC $<BUILD_INTERFACE:dl>)
endif()

set_target_properties(utp_static PROPERTIES OUTPUT_NAME utp EXPORT_NAME utp)

add_subdirectory(examples)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libutpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libutpConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libutpConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libutp"
)

install(TARGETS utp_static
    EXPORT libutpTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(FILES ${UTP_PUBLIC_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/utp")

install(EXPORT libutpTargets
    FILE libutpTargets.cmake
    NAMESPACE eular::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libutp"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libutpConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libutpConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libutp"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
    IMMEDIATE @ONLY
)

if(NOT TARGET uninstall)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
    )
endif()

if(UTP_BUILD_TESTS)
    enable_testing()

    set(UTP_TEST_SOURCES
        test/test_connection_stream_layer.cc
        test/test_context_cid_alloc.cc
        test/test_context_integration.cc
        test/test_frame_ack_decode.cc
        test/test_frame_ack_frequency.cc
        test/test_frame_reset_stream.cc
        test/test_frame_crypto.cc
        test/test_frame_handshake_aux.cc
        test/test_frame_path.cc
        test/test_frame_session_token.cc
        test/test_frame_transport_params.cc
        test/test_mtu.cc
        test/test_network_path.cc
        test/test_packet_in.cc
        test/test_packet_out.cc
        test/test_retrans_repack.cc
        test/test_receive_history.cc
        test/test_ring_buffer.cc
        test/test_stream_impl.cc
        test/test_stream_zero_copy_views.cc
        test/test_fault_injection_mmsg.cc
        test/test_fault_injection_new.cc
    )

    add_executable(utp_tests ${UTP_TEST_SOURCES})
    target_include_directories(utp_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/3rd"
            "${CMAKE_CURRENT_SOURCE_DIR}/test"
            "${UTP_UTILS_ROOT}/include"
            "${UTP_EVENT_ROOT}/include"
    )
    target_link_libraries(utp_tests PRIVATE utp_static)

    if(UTP_ENABLE_FAULT_INJECTION)
        target_compile_definitions(utp_tests PRIVATE UTP_ENABLE_FAULT_INJECTION)
    endif()

    add_test(NAME utp_tests COMMAND utp_tests)
    target_link_libraries(utp_tests PRIVATE utp_static)
endif()