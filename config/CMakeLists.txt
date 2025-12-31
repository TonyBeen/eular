cmake_minimum_required(VERSION 3.8)

project(config VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo") # Release MinSizeRel RelWithDebInfo Debug
endif()

option(CONFIG_DYNAMIC_LIB "Build dynamic library" ON)
option(CONFIG_STATIC_LIB "Build static library" ON)
option(BUILD_EXAMPLES "Compile sample program" OFF)
option(BUILD_TEST "Compile sample program" OFF)
option(ENABLE_DEBUG "Enable debug symbol" OFF)

if (CONFIG_DYNAMIC_LIB MATCHES "OFF" AND CONFIG_STATIC_LIB MATCHES "OFF")
    message(FATAL_ERROR "CONFIG_DYNAMIC_LIB and CONFIG_STATIC_LIB cannot be both OFF")
endif()

if(ENABLE_DEBUG MATCHES "ON")
    set(CMAKE_BUILD_TYPE "Debug")
endif()

find_package(gtest QUIET)
if (NOT gtest_FOUND)
    message(STATUS "gtest not found, will not build test")
    set(BUILD_TEST OFF)
endif()

set(SOURCES
    src/dns_resolve.cpp
    src/event_async.cpp
    src/event_base.cpp
    src/event_loop.cpp
    src/event_poll.cpp
    src/event_signal.cpp
    src/event_timer.cpp
)

set(INTERNAL_HEADERS
    include/internal/rwmutex.h
    include/internal/tinyxml2.h
)

set(HEADERS
    include/config/IniConfig.h
    include/config/type_cast.h
    include/config/XmlConfig.h
    include/config/YamlConfig.h
)

if (BUILD_EXAMPLES MATCHES "ON")
    add_subdirectory(examples)
endif()

if (BUILD_TEST MATCHES "ON")
    add_subdirectory(test)
endif()
