cmake_minimum_required(VERSION 3.10)

cmake_policy(SET CMP0054 NEW)

project(utils VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(UTILS_STATIC_LIB "Build static library" ON)
option(UTILS_DYNAMIC_LIB "Build dynamic library" ON)
option(UTILS_BIMAP_USE_STD_CONTAINER "Use std::map/std::unordered_map as BiMap backend" OFF)
option(UTILS_ENABLE_DEBUG "Enable debug symbol" OFF)
option(UTILS_ENABLE_TEST "Enable test" OFF)
option(UTILS_ENABLE_ASAN "Enable AddressSanitizer for Linux builds" OFF)
option(SUBMODULE_ENABLE_INSTALL "Whether to install relevant header files and libraries when used as a submodule?" ON)
option(UTILS_SUBMODULE_ENABLE_INSTALL "" ${SUBMODULE_ENABLE_INSTALL})

# Keep BUILD_TESTING strictly aligned with UTILS_ENABLE_TEST.
set(BUILD_TESTING ${UTILS_ENABLE_TEST} CACHE BOOL "Enable building tests" FORCE)
include(CTest)

if (NOT UTILS_STATIC_LIB AND NOT UTILS_DYNAMIC_LIB)
    message(FATAL_ERROR "UTILS_STATIC_LIB and UTILS_DYNAMIC_LIB cannot be both OFF")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type (Release MinSizeRel RelWithDebInfo Debug)")
endif()

if(UTILS_ENABLE_DEBUG)
    if(CMAKE_CONFIGURATION_TYPES)
        message(STATUS "UTILS_ENABLE_DEBUG is ON. Please build with Debug configuration for multi-config generators.")
    else()
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Release MinSizeRel RelWithDebInfo Debug)" FORCE)
    endif()
endif()

message(STATUS "UTILS CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "UTILS CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "UTILS CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

set(MACRO_DEFINITIONS)
set(C_WARNING_FLAGS)
set(CXX_WARNING_FLAGS)
set(COMMON_COMPILE_FLAGS)
set(COMMON_LINK_FLAGS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MACRO_DEFINITIONS _GNU_SOURCE __USE_FILE_OFFSET64 _LARGEFILE64_SOURCE)
    set(C_WARNING_FLAGS -W -Wall -Werror -Wextra -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
    set(CXX_WARNING_FLAGS -W -Wall -Werror -Wextra -Wno-deprecated -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
    set(COMMON_COMPILE_FLAGS -fPIC)
    # set(3RD_INC_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/3rd/Linux/include)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(MACRO_DEFINITIONS _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    set(C_WARNING_FLAGS /utf-8)
    set(CXX_WARNING_FLAGS /utf-8)
    # Use libiconv headers from bundled source/build outputs, not 3rd/Windows.
    set(3RD_INC_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd/libiconv_v1_18/include
        ${CMAKE_CURRENT_BINARY_DIR}/3rd/libiconv_v1_18/include)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # 匹配 AppleClang 和 Clang
    set(MACRO_DEFINITIONS _GNU_SOURCE __USE_FILE_OFFSET64 _LARGEFILE64_SOURCE)
    set(C_WARNING_FLAGS -W -Wall -Werror -Wextra -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
    set(CXX_WARNING_FLAGS -W -Wall -Werror -Wextra -Wno-deprecated -Wno-unused-result -Wno-unused-function -Wno-unused-parameter)
    list(APPEND CXX_WARNING_FLAGS -Wno-unused-private-field) # AppleClang 默认开启该警告
    set(COMMON_COMPILE_FLAGS -fPIC)
    # set(3RD_INC_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/3rd/Linux/include)
else()
    message(STATUS "Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    list(APPEND COMMON_COMPILE_FLAGS -fno-omit-frame-pointer -fno-optimize-sibling-calls)

    if(UTILS_ENABLE_ASAN)
        list(APPEND COMMON_COMPILE_FLAGS
            -fsanitize=address
            -fsanitize=leak
            -fsanitize=undefined)
        list(APPEND COMMON_LINK_FLAGS
            -fsanitize=address
            -fsanitize=leak
            -fsanitize=undefined)
    endif()
endif()

if(NOT APPLE)
    add_subdirectory(3rd/libiconv_v1_18)
endif()

file(GLOB HEADER_LIST CONFIGURE_DEPENDS
    "include/utils/*.h"
    "include/utils/*.hpp")

file(GLOB SRC_LIST CONFIGURE_DEPENDS "src/*.cpp")
list(APPEND SRC_LIST        "src/printf.c")
list(APPEND SRC_LIST        "src/sha1.c")

function(utils_apply_common_target_settings target)
    target_compile_features(${target} PUBLIC cxx_std_11)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${3RD_INC_DIR}
    )

    target_link_libraries(${target} PRIVATE iconv charset)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_libraries(${target} PUBLIC Threads::Threads)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_link_libraries(${target}
            PRIVATE
                ws2_32.lib)
    endif()

    target_compile_definitions(${target} PRIVATE ${MACRO_DEFINITIONS})
    if(UTILS_BIMAP_USE_STD_CONTAINER)
        target_compile_definitions(${target} PUBLIC UTILS_BIMAP_USE_STD_CONTAINER=1)
    else()
        target_compile_definitions(${target} PUBLIC UTILS_BIMAP_USE_STD_CONTAINER=0)
    endif()
    target_compile_options(${target}
        PRIVATE
            ${COMMON_COMPILE_FLAGS}
            $<$<COMPILE_LANGUAGE:C>:${C_WARNING_FLAGS}>
            $<$<COMPILE_LANGUAGE:CXX>:${CXX_WARNING_FLAGS}>
    )

    if(COMMON_LINK_FLAGS)
        target_link_options(${target} PRIVATE ${COMMON_LINK_FLAGS})
    endif()
endfunction()

# build the static library
if(UTILS_STATIC_LIB)
    add_library(utils_static STATIC ${SRC_LIST} ${HEADER_LIST})
    add_library(eular::utils_static ALIAS utils_static)
    utils_apply_common_target_settings(utils_static)
    target_compile_definitions(utils_static PRIVATE UTILS_STATIC)

    set_property(TARGET utils_static PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(UTILS_SUBMODULE_ENABLE_INSTALL)
        install(TARGETS utils_static
            EXPORT utilsTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

#build dynamic library
if(UTILS_DYNAMIC_LIB)
    add_library(utils SHARED ${SRC_LIST} ${HEADER_LIST})
    add_library(eular::utils ALIAS utils)
    utils_apply_common_target_settings(utils)
    target_compile_definitions(utils PRIVATE UTILS_EXPORTS)

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.13")
        target_link_options(utils PRIVATE -Wl,-z,now -Wl,-z,relro)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_target_properties (
            utils
            PROPERTIES
            LINK_FLAGS "-Wl,-z,now,-z,relro"
        )
    endif()

    set_property(TARGET utils PROPERTY LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(UTILS_SUBMODULE_ENABLE_INSTALL)
        install(TARGETS utils
            EXPORT utilsTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

# build test
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

if(UTILS_SUBMODULE_ENABLE_INSTALL)
    install(FILES ${HEADER_LIST} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/utils")

    install(EXPORT utilsTargets
        NAMESPACE eular::
        FILE utilsTargets.cmake
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/utils")

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/utilsConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/utilsConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/utils")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/utilsConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/utilsConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/utilsConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/utils")

    # uninstall target
    if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake)
    endif()
endif()
