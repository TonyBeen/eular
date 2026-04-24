cmake_minimum_required(VERSION 3.15)

project(log VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LOG_BUILD_STATIC "Build liblog as static library" ON)
option(LOG_BUILD_TESTLOG "Build testlog binary" ON)
option(LOG_BUILD_LOGCAT "Build logcat binary" ON)
option(LOG_BUILD_CALLSTACK_TEST "Build test_callstack binary" OFF)
option(LOG_BUILD_BENCHMARK "Build benchmark binary" ON)
option(SUBMODULE_ENABLE_INSTALL "Whether to install relevant header files and libraries when used as a submodule?" ON)
option(LOG_SUBMODULE_ENABLE_INSTALL "" ${SUBMODULE_ENABLE_INSTALL})

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
set(LOG_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/log/log.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/log/level.h
)
set(LOG_HAVE_CALLSTACK OFF)

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

    add_library(zlog_vendor OBJECT ${ZLOG_SOURCES})
    set_target_properties(zlog_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(zlog_vendor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/zlog/src)

    list(APPEND LOG_SOURCES
        src/log_zlog.cpp
        src/log_format.cpp
    )

    find_library(UNWIND_LIB unwind)
    if(UNWIND_LIB)
        list(APPEND LOG_SOURCES src/callstack.cpp)
        list(APPEND LOG_EXTRA_LIBS unwind)
        list(APPEND LOG_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/log/callstack.h)
        set(LOG_HAVE_CALLSTACK ON)
    else()
        message(WARNING "libunwind not found; callstack dump is disabled")
    endif()
endif()

add_library(log STATIC ${LOG_SOURCES})
add_library(eular::log ALIAS log)

add_library(log_obj OBJECT ${LOG_SOURCES})
add_library(eular::log_obj ALIAS log_obj)

if(TARGET zlog_vendor)
    target_sources(log PRIVATE $<TARGET_OBJECTS:zlog_vendor>)
    target_sources(log_obj PRIVATE $<TARGET_OBJECTS:zlog_vendor>)
endif()

target_compile_features(log PUBLIC cxx_std_11)
set_target_properties(log PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(log_obj PUBLIC cxx_std_11)
set_target_properties(log_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(log
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/log>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(log_obj
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/log>
)

target_include_directories(log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/log)
target_include_directories(log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party)

target_include_directories(log_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(log_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/log)
target_include_directories(log_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party)

target_link_libraries(log
    PUBLIC
        ${LOG_EXTRA_LIBS}
)

target_link_libraries(log_obj
    PUBLIC
        ${LOG_EXTRA_LIBS}
)

if(NOT WIN32 AND UNWIND_LIB)
    target_compile_definitions(log PRIVATE LOG_ENABLE_CALLSTACK=1)
    target_compile_definitions(log_obj PRIVATE LOG_ENABLE_CALLSTACK=1)
endif()

if(LOG_BUILD_TESTLOG)
    add_executable(testlog.out test/testlog.cc)
    target_link_libraries(testlog.out PRIVATE log Threads::Threads)
    if(BUILD_TESTING)
        add_test(NAME log.testlog COMMAND testlog.out)
    endif()
endif()

if(LOG_BUILD_LOGCAT AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(logcat examples/logcat.cc)
    target_include_directories(logcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(logcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/log)
    target_include_directories(logcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party)
    target_link_libraries(logcat PRIVATE log Threads::Threads)
endif()

if(LOG_BUILD_CALLSTACK_TEST AND LOG_HAVE_CALLSTACK)
    add_executable(test_callstack.out test/test_callstack.cc)
    target_link_libraries(test_callstack.out PRIVATE log Threads::Threads)
    if(BUILD_TESTING)
        add_test(NAME log.test_callstack COMMAND test_callstack.out)
    endif()
endif()

if(LOG_BUILD_BENCHMARK)
    add_executable(bench_liblog.out benchmark/bench_liblog.cc)
    target_link_libraries(bench_liblog.out PRIVATE log Threads::Threads)
endif()

if(LOG_SUBMODULE_ENABLE_INSTALL)
    install(TARGETS log
        EXPORT logTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(FILES ${LOG_PUBLIC_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/log)

    install(EXPORT logTargets
        NAMESPACE eular::
        FILE logTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/log)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/logConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/logConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/log)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/logConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/logConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/logConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/log)
endif()
