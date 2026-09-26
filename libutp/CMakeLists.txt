cmake_minimum_required(VERSION 3.22)

project(libutp VERSION 1.2.0 LANGUAGES C CXX)

include(CTest)

option(UTP_BUILD_LIBRARY "Build the libutp library" ON)
option(UTP_BUILD_SHARED "Build libutp as a shared library" OFF)
option(UTP_BUILD_NTRS "Build the NTRS service executables" ON)

if(UTP_BUILD_LIBRARY)
    set(UTP_BUILD_SHARED "${UTP_BUILD_SHARED}" CACHE BOOL "Build libutp as a shared library" FORCE)
    add_subdirectory(utp)
    if(NOT TARGET eular::utp)
        add_library(eular::utp ALIAS utp)
    endif()
endif()

if(UTP_BUILD_NTRS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(ntrs)
elseif(UTP_BUILD_NTRS)
    message(WARNING "UTP_BUILD_NTRS is enabled, but NTRS services are Linux-only; skipping ntrs")
endif()
