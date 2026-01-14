list(APPEND EXAMPLES
    test_event_loop
    test_event_async
    test_ares_nonblock
    test_dns_resolve
)

add_executable(test_event_loop test_event_loop.cc)
target_include_directories(test_event_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/)
target_include_directories(test_event_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/include)
target_include_directories(test_event_loop PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/)
target_include_directories(test_event_loop PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/include)

if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_event_loop event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_event_loop event_wrapper)
endif()

add_executable(test_event_async test_event_async.cc)
target_include_directories(test_event_async PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/)
target_include_directories(test_event_async PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/include)
target_include_directories(test_event_async PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/)
target_include_directories(test_event_async PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/include)

if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_event_async event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_event_async event_wrapper)
endif()

# test_ares_nonblock
add_executable(test_ares_nonblock test_ares_nonblock.cc)
target_include_directories(test_ares_nonblock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libares_V1_34_1/include)
target_include_directories(test_ares_nonblock PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libares_V1_34_1/)
target_include_directories(test_ares_nonblock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/)
target_include_directories(test_ares_nonblock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/include)
target_include_directories(test_ares_nonblock PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/)
target_include_directories(test_ares_nonblock PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/include)

if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_ares_nonblock event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_ares_nonblock event_wrapper)
endif()


add_executable(test_dns_resolve test_dns_resolve.cc)
if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_dns_resolve event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_dns_resolve event_wrapper)
endif()

add_custom_target(examples DEPENDS ${EXAMPLES})