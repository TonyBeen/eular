list(APPEND EXAMPLES
    test_event_loop
    test_event_async
    test_event_async_perf
    test_notify_hotpath_perf
    test_single_async_perf
    test_dns_resolve
)

if(EVENT_USE_CARES)
list(APPEND EXAMPLES
    test_ares_nonblock
)
endif()

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

add_executable(test_event_async_perf test_event_async_perf.cc)
target_include_directories(test_event_async_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/)
target_include_directories(test_event_async_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/include)
target_include_directories(test_event_async_perf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/)
target_include_directories(test_event_async_perf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/include)

if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_event_async_perf event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_event_async_perf event_wrapper)
endif()

add_executable(test_single_async_perf test_single_async_perf.cc)
target_include_directories(test_single_async_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/)
target_include_directories(test_single_async_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/include)
target_include_directories(test_single_async_perf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/)
target_include_directories(test_single_async_perf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/include)

if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_single_async_perf event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_single_async_perf event_wrapper)
endif()

add_executable(test_notify_hotpath_perf test_notify_hotpath_perf.cc)
target_include_directories(test_notify_hotpath_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/)
target_include_directories(test_notify_hotpath_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libevent_V2_1_10/include)
target_include_directories(test_notify_hotpath_perf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/)
target_include_directories(test_notify_hotpath_perf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../libevent_V2_1_10/include)

if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_notify_hotpath_perf event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_notify_hotpath_perf event_wrapper)
endif()

if(EVENT_USE_CARES)
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
endif()

add_executable(test_dns_resolve test_dns_resolve.cc)
if(EVENT_STATIC_LIB MATCHES "ON")
target_link_libraries(test_dns_resolve event_wrapper_static)
elseif(EVENT_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(test_dns_resolve event_wrapper)
endif()
