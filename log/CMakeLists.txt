cmake_minimum_required(VERSION 3.15)

project(log LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LOG_BUILD_STATIC "Build liblog as static library" ON)
option(LOG_BUILD_TESTLOG "Build testlog binary" ON)
option(LOG_BUILD_LOGCAT "Build logcat binary" ON)
option(LOG_BUILD_CALLSTACK_TEST "Build test_callstack binary" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-W>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX>:-O2>
    )
endif()

find_package(Threads REQUIRED)

if(NOT LOG_BUILD_STATIC)
    message(FATAL_ERROR "LOG_BUILD_STATIC must be ON; shared library build is disabled")
endif()

set(LOG_SOURCES)
set(LOG_EXTRA_LIBS Threads::Threads)

if(WIN32)
    list(APPEND LOG_SOURCES
        src/log.cpp
        src/log_main.cpp
        src/log_write.cpp
        src/log_format.cpp
    )
else()
    file(GLOB ZLOG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/zlog/src/*.c")
    list(REMOVE_ITEM ZLOG_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/zlog/src/zlog-chk-conf.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/zlog/src/zlog_win.c"
    )

    add_library(zlog_vendor STATIC ${ZLOG_SOURCES})
    set_target_properties(zlog_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(zlog_vendor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/zlog/src)
    target_link_libraries(zlog_vendor PUBLIC Threads::Threads)

    list(APPEND LOG_SOURCES
        src/log_zlog.cpp
        src/log_format.cpp
    )
    list(APPEND LOG_EXTRA_LIBS zlog_vendor)

    find_library(UNWIND_LIB unwind)
    if(UNWIND_LIB)
        list(APPEND LOG_SOURCES src/callstack.cpp)
        list(APPEND LOG_EXTRA_LIBS ${UNWIND_LIB})
    else()
        message(WARNING "libunwind not found; callstack dump is disabled")
    endif()
endif()

add_library(log STATIC ${LOG_SOURCES})

target_include_directories(log
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/log>
)

target_include_directories(log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party)

target_link_libraries(log
    PUBLIC
        ${LOG_EXTRA_LIBS}
)

if(NOT WIN32 AND UNWIND_LIB)
    target_compile_definitions(log PRIVATE LOG_ENABLE_CALLSTACK=1)
endif()

if(LOG_BUILD_TESTLOG)
    add_executable(testlog.out test/testlog.cc)
    target_link_libraries(testlog.out PRIVATE log Threads::Threads)
endif()

if(LOG_BUILD_LOGCAT AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(logcat examples/logcat.cc)
    target_include_directories(logcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(logcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party)
    target_link_libraries(logcat PRIVATE log Threads::Threads)
endif()

if(LOG_BUILD_CALLSTACK_TEST)
    add_executable(test_callstack.out test/test_callstack.cc)
    target_link_libraries(test_callstack.out PRIVATE log Threads::Threads)
endif()
