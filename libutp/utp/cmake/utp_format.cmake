set(UTP_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

set(_utp_format_relative_sources
    benchmark/bench_containers.c
    benchmark/bench_concurrent_connections.c
    examples/echo_client.c
    examples/echo_server.c
    test/test_core.c
    test/test_public_api.c
    test/test_crypto.cc
    test/test_proto.cc
    test/test_receive_history.cc
    test/test_packet_out.cc
    test/test_packet_in.cc
    test/test_send_ledger.cc
    test/test_send_control.cc
    test/test_pending_incoming.cc
    test/test_congestion.cc
    test/test_minmax.cc
    test/connection_test_util.h
    test/test_stream.cc
    test/test_connection.cc
    test/test_bw_sampler.cc
    test/test_bbr.cc
    test/test_mtu.cc
    test/test_nat.cc
    test/test_parser_fuzz.cc
    test/test_transport_integration.cc
    test/fuzz/packet_parser_fuzz.cc
    test/socket_batch_fault/socket_batch_hook.c
    test/socket_batch_fault/socket_batch_hook.h)

set(UTP_FORMAT_SOURCES)
foreach(_utp_format_source IN LISTS _utp_format_relative_sources UTP_HEADERS UTP_SOURCES)
    list(APPEND UTP_FORMAT_SOURCES "${UTP_ROOT_DIR}/${_utp_format_source}")
endforeach()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format
    HINTS "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
    "/Library/Developer/CommandLineTools/usr/bin")
if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(utp_format COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${UTP_FORMAT_SOURCES}
        WORKING_DIRECTORY "${UTP_ROOT_DIR}" COMMENT "Formatting libutp sources")
    add_custom_target(utp_format_check COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file
        ${UTP_FORMAT_SOURCES} WORKING_DIRECTORY "${UTP_ROOT_DIR}" COMMENT "Checking libutp source formatting")
    if(UTP_ENABLE_FORMAT_CHECK)
        add_custom_target(utp_format_gate ALL DEPENDS utp_format_check)
    endif()
elseif(UTP_ENABLE_FORMAT_CHECK)
    message(FATAL_ERROR "UTP_ENABLE_FORMAT_CHECK requires clang-format")
endif()
