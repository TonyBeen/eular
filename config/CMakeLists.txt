cmake_minimum_required(VERSION 3.10)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(config VERSION 1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CONFIG_BUILD_TESTS "Build config Catch tests" ON)
option(CONFIG_BUILD_SHARED "Build shared config library" ON)
option(CONFIG_BUILD_STATIC "Build static config library" ON)

if(NOT CONFIG_BUILD_SHARED AND NOT CONFIG_BUILD_STATIC)
    message(FATAL_ERROR "At least one of CONFIG_BUILD_SHARED or CONFIG_BUILD_STATIC must be ON")
endif()

add_subdirectory(3rd_party)

find_package(Threads REQUIRED)

set(CONFIG_SOURCE_FILES
    src/IniConfig.cpp
    src/XmlConfig.cpp
    src/json.cpp
    src/json_node.cpp
    src/rwmutex.cpp
    src/tinyxml2.cpp
    src/yaml.cpp
    src/yaml_node.cpp
)

set(CONFIG_VENDOR_SOURCE_FILES
    3rd_party/libyaml/src/api.c
    3rd_party/libyaml/src/reader.c
    3rd_party/libyaml/src/scanner.c
    3rd_party/libyaml/src/parser.c
    3rd_party/libyaml/src/loader.c
    3rd_party/libyaml/src/writer.c
    3rd_party/libyaml/src/emitter.c
    3rd_party/libyaml/src/dumper.c
    3rd_party/yyjson/src/yyjson.c
)

set(CONFIG_LIBYAML_VERSION_MAJOR 0)
set(CONFIG_LIBYAML_VERSION_MINOR 2)
set(CONFIG_LIBYAML_VERSION_PATCH 5)
set(CONFIG_LIBYAML_VERSION_STRING "${CONFIG_LIBYAML_VERSION_MAJOR}.${CONFIG_LIBYAML_VERSION_MINOR}.${CONFIG_LIBYAML_VERSION_PATCH}")
set(CONFIG_VENDOR_COMPILE_DEFINITIONS
    YAML_DECLARE_STATIC
    YAML_VERSION_MAJOR=${CONFIG_LIBYAML_VERSION_MAJOR}
    YAML_VERSION_MINOR=${CONFIG_LIBYAML_VERSION_MINOR}
    YAML_VERSION_PATCH=${CONFIG_LIBYAML_VERSION_PATCH}
    YAML_VERSION_STRING="${CONFIG_LIBYAML_VERSION_STRING}"
)

set(CONFIG_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(CONFIG_PRIVATE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/libyaml/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/libyaml/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/yyjson/src
)
set(CONFIG_C4CORE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/c4core/src)
set(CONFIG_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

if(CONFIG_BUILD_SHARED)
    add_library(config_shared SHARED ${CONFIG_SOURCE_FILES} ${CONFIG_VENDOR_SOURCE_FILES})
    set_target_properties(config_shared PROPERTIES OUTPUT_NAME config EXPORT_NAME shared)
    target_compile_definitions(config_shared PRIVATE CONFIG_EXPORTS)
    target_compile_definitions(config_shared PRIVATE ${CONFIG_VENDOR_COMPILE_DEFINITIONS})
    target_include_directories(config_shared
        PUBLIC
            $<BUILD_INTERFACE:${CONFIG_PUBLIC_INCLUDE_DIR}>
            $<BUILD_INTERFACE:${CONFIG_C4CORE_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CONFIG_PRIVATE_INCLUDE_DIRS}
    )
    target_link_libraries(config_shared
        PUBLIC
            c4core
        PRIVATE
            Threads::Threads
    )
    add_library(config::shared ALIAS config_shared)
endif()

if(CONFIG_BUILD_STATIC)
    add_library(config_static STATIC ${CONFIG_SOURCE_FILES} ${CONFIG_VENDOR_SOURCE_FILES})
    set_target_properties(config_static PROPERTIES OUTPUT_NAME config EXPORT_NAME static)
    target_compile_definitions(config_static PUBLIC CONFIG_STATIC)
    target_compile_definitions(config_static PRIVATE ${CONFIG_VENDOR_COMPILE_DEFINITIONS})
    target_include_directories(config_static
        PUBLIC
            $<BUILD_INTERFACE:${CONFIG_PUBLIC_INCLUDE_DIR}>
            $<BUILD_INTERFACE:${CONFIG_C4CORE_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CONFIG_PRIVATE_INCLUDE_DIRS}
    )
    target_link_libraries(config_static
        PUBLIC
            c4core
            Threads::Threads
    )
    add_library(config::static ALIAS config_static)
endif()

set(CONFIG_INSTALL_TARGETS)

if(TARGET config_shared)
    list(APPEND CONFIG_INSTALL_TARGETS config_shared)
endif()
if(TARGET config_static)
    list(APPEND CONFIG_INSTALL_TARGETS config_static)
endif()

foreach(CONFIG_DEP_TARGET c4core)
    if(TARGET ${CONFIG_DEP_TARGET})
        list(APPEND CONFIG_INSTALL_TARGETS ${CONFIG_DEP_TARGET})
    endif()
endforeach()

list(REMOVE_DUPLICATES CONFIG_INSTALL_TARGETS)

install(TARGETS ${CONFIG_INSTALL_TARGETS}
    EXPORT configTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/c4core/src/c4
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/yyjson/src/yyjson.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/configConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/configConfig.cmake
    INSTALL_DESTINATION ${CONFIG_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/configConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT configTargets
    FILE configTargets.cmake
    NAMESPACE config::
    DESTINATION ${CONFIG_INSTALL_CMAKEDIR}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/configConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/configConfigVersion.cmake
    DESTINATION ${CONFIG_INSTALL_CMAKEDIR}
)

if(CONFIG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()