cmake_minimum_required(VERSION 3.22)

project(libutp VERSION 1.2.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
else()
    add_compile_options(-fno-omit-frame-pointer)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(UTP_BUILD_TESTS "Build libutp tests" OFF)
option(UTP_BUILD_LSQUIC_ECHO "Build lsquic echo comparison examples" ON)
option(UTP_USE_BUNDLED_BORINGSSL "Use bundled BoringSSL submodule" ON)
option(UTP_ENABLE_FAULT_INJECTION "Enable Linux-only libfiu fault injection hooks" OFF)
option(UTP_USE_SENDMMSG "Use recvmmsg/sendmmsg batching on Linux" ON)

if(UTP_ENABLE_FAULT_INJECTION AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "UTP_ENABLE_FAULT_INJECTION is Linux-only, disabling on ${CMAKE_SYSTEM_NAME}")
    set(UTP_ENABLE_FAULT_INJECTION OFF CACHE BOOL "Enable Linux-only libfiu fault injection hooks" FORCE)
endif()

if(UTP_USE_SENDMMSG AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "UTP_USE_SENDMMSG is Linux-only, disabling on ${CMAKE_SYSTEM_NAME}")
    set(UTP_USE_SENDMMSG OFF CACHE BOOL "Use recvmmsg/sendmmsg batching on Linux" FORCE)
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
    cpp/include/utp/config.h
    cpp/include/utp/connection.h
    cpp/include/utp/errno.h
    cpp/include/utp/logger.h
    cpp/include/utp/platform.h
    cpp/include/utp/stream.h
    cpp/include/utp/types.h
    cpp/include/utp/context.h
    cpp/include/utp/utp.h
)

set(UTP_SOURCES
    3rd/rbtree.c
    cpp/src/congestion/bbr_v1.cpp
    cpp/src/congestion/bw_sampler.cpp
    cpp/src/congestion/cubic.cpp
    cpp/src/congestion/minmax.cpp
    cpp/src/congestion/pacer.cpp
    cpp/src/context/connection_impl.cpp
    cpp/src/context/context_impl.cpp
    cpp/src/context/detail/frame_meta_policy.cpp
    cpp/src/context/detail/packet_editor.cpp
    cpp/src/context/packet_decode_helper.cpp
    cpp/src/context/send_ctl.cpp
    cpp/src/context/stream_impl.cpp
    cpp/src/crypto/aes_gcm_context.cpp
    cpp/src/crypto/base64.cpp
    cpp/src/crypto/resumption_state_codec.cpp
    cpp/src/crypto/traffic_key_schedule.cpp
    cpp/src/crypto/token.cpp
    cpp/src/crypto/x25519_wrapper.cpp
    cpp/src/logger/logger.cpp
    cpp/src/mtu/mtu.cpp
    cpp/src/proto/frame/ack.cpp
    cpp/src/proto/frame/ack_frequency.cpp
    cpp/src/proto/frame/connection_close.cpp
    cpp/src/proto/frame/crypto.cpp
    cpp/src/proto/frame/handshake_delay.cpp
    cpp/src/proto/frame/handshake_done.cpp
    cpp/src/proto/frame/handshake_helper.cpp
    cpp/src/proto/frame/max_data.cpp
    cpp/src/proto/frame/max_stream_data.cpp
    cpp/src/proto/frame/data_blocked.cpp
    cpp/src/proto/frame/stream_data_blocked.cpp
    cpp/src/proto/frame/padding.cpp
    cpp/src/proto/frame/path.cpp
    cpp/src/proto/frame/reset_stream.cpp
    cpp/src/proto/frame/session_token.cpp
    cpp/src/proto/frame/stream.cpp
    cpp/src/proto/frame/transport_params.cpp
    cpp/src/proto/frame/version.cpp
    cpp/src/proto/frame.cpp
    cpp/src/proto/packet_in.cpp
    cpp/src/proto/packet_out.cpp
    cpp/src/socket/address.cpp
    cpp/src/socket/mmsg.cpp
    cpp/src/socket/packet.cpp
    cpp/src/socket/udp.cpp
    cpp/src/socket/util.cpp
    cpp/src/util/ack_info.cpp
    cpp/src/util/errno.cpp
    cpp/src/util/error.cpp
    cpp/src/util/mm.cpp
    cpp/src/util/network_path.cpp
    cpp/src/util/ring_buffer.cpp
    cpp/src/util/receive_history.cpp
    cpp/src/util/send_history.cpp
    cpp/src/util/time.cpp
    cpp/src/util/transport_param.cpp
    cpp/src/util/util.cpp
    cpp/src/context.cpp
)

add_library(utp_static STATIC ${UTP_SOURCES} ${UTP_PUBLIC_HEADERS})
add_library(eular::utp ALIAS utp_static)

target_compile_definitions(utp_static
    PUBLIC
        UTP_STATIC
        $<$<BOOL:${MSVC}>:NOMINMAX>
)

if(UTP_USE_SENDMMSG AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(utp_static PUBLIC USE_SENDMMSG)
endif()

if(UTP_ENABLE_FAULT_INJECTION)
    target_compile_definitions(utp_static PRIVATE UTP_ENABLE_FAULT_INJECTION)
endif()

target_include_directories(utp_static
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/cpp/src"
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

add_subdirectory(cpp/examples)

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
        cpp/test/test_connection_stream_layer.cc
        cpp/test/test_ack_behavior.cc
        cpp/test/test_context_cid_alloc.cc
        cpp/test/test_context_integration.cc
        cpp/test/test_context_zero_rtt.cc
        cpp/test/test_frame_ack_decode.cc
        cpp/test/test_frame_ack_frequency.cc
        cpp/test/test_frame_reset_stream.cc
        cpp/test/test_frame_crypto.cc
        cpp/test/test_frame_handshake_aux.cc
        cpp/test/test_frame_path.cc
        cpp/test/test_frame_session_token.cc
        cpp/test/test_frame_transport_params.cc
        cpp/test/test_frame_flow_control.cc
        cpp/test/test_bbr_config.cc
        cpp/test/test_bbr_new_params.cc
        cpp/test/test_cubic.cc
        cpp/test/test_mtu.cc
        cpp/test/test_network_path.cc
        cpp/test/test_packet_in.cc
        cpp/test/test_packet_out.cc
        cpp/test/test_mm.cc
        cpp/test/test_retrans_repack.cc
        cpp/test/test_receive_history.cc
        cpp/test/test_ring_buffer.cc
        cpp/test/test_stream_impl.cc
        cpp/test/test_stream_zero_copy_views.cc
        cpp/test/test_public_api_errno.cc
        cpp/test/test_status.cc
        cpp/test/test_fault_injection_mmsg.cc
        cpp/test/test_fault_injection_new.cc
    )

    add_executable(utp_tests ${UTP_TEST_SOURCES})
    target_include_directories(utp_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/3rd"
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/test"
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

if(UTP_BUILD_LSQUIC_ECHO)
    add_subdirectory(lsquic)
endif()
