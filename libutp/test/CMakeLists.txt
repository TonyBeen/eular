set(UTP_TEST_SOURCES
    test_connection_stream_layer.cc
    test_context_cid_alloc.cc
    test_context_integration.cc
    test_context_zero_rtt.cc
    test_frame_ack_decode.cc
    test_frame_crypto.cc
    test_frame_path.cc
    test_frame_session_token.cc
    test_mtu.cc
    test_network_path.cc
    test_packet_in.cc
    test_packet_out.cc
    test_mm.cc
    test_receive_history.cc
    test_stream_impl.cc
)

add_executable(utp_tests ${UTP_TEST_SOURCES})

target_include_directories(utp_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/3rd"
        "${PROJECT_SOURCE_DIR}/test"
        "${UTP_UTILS_ROOT}/include"
        "${UTP_EVENT_ROOT}/include"
)

target_link_libraries(utp_tests PRIVATE utp_static)

add_test(NAME utp_tests COMMAND utp_tests)
