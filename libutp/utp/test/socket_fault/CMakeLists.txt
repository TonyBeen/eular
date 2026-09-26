if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

add_library(utp_socket_send_hook SHARED socket_send_hook.c fishhook/fishhook.c)
target_include_directories(utp_socket_send_hook PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fishhook")

target_include_directories(utp_transport_integration_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(utp_transport_integration_test PRIVATE utp_socket_send_hook)
