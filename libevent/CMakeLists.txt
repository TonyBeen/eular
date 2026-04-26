cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(event_wrapper VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 11)

include(GNUInstallDirs)


option(EVENT_DYNAMIC_LIB "Build dynamic library" ON)
option(EVENT_STATIC_LIB "Build static library" ON)
option(EVENT_USE_CARES "Use c-ares backend for DNSResolver" OFF)
option(BUILD_EXAMPLES "Compile sample program" OFF)
option(BUILD_TEST "Compile sample program" OFF)
option(ENABLE_DEBUG "Enable debug symbol" OFF)
option(SUBMODULE_ENABLE_INSTALL "Whether to install relevant header files and libraries when used as a submodule?" ON)
option(EVENT_SUBMODULE_ENABLE_INSTALL "" ${SUBMODULE_ENABLE_INSTALL})

if (NOT EVENT_DYNAMIC_LIB AND NOT EVENT_STATIC_LIB)
    message(FATAL_ERROR "EVENT_DYNAMIC_LIB and EVENT_STATIC_LIB cannot be both OFF")
endif()

# default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo") # Release MinSizeRel RelWithDebInfo Debug
endif()

if(ENABLE_DEBUG)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(INSTALL_INC_DIR "${CMAKE_INSTALL_PREFIX}/include" CACHE PATH "Installation directory for headers")
set(INSTALL_LIB_DIR "${CMAKE_INSTALL_PREFIX}/lib" CACHE PATH "Installation directory for libraries")

set(SOURCES
    src/dns_resolve.cpp
    src/event_async.cpp
    src/event_base.cpp
    src/event_loop.cpp
    src/event_poll.cpp
    src/event_timer.cpp)

set(HEADERS
    include/event/dns_resolve.h
    include/event/async.h
    include/event/base.h
    include/event/loop.h
    include/event/poll.h
    include/event/timer.h)

# libevent
add_subdirectory(libevent_V2_1_10)
# 异步DNS解析库
if(EVENT_USE_CARES)
    # Build c-ares as embedded object library and do not install c-ares artifacts.
    set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
    set(CARES_STATIC OFF CACHE BOOL "" FORCE)
    set(CARES_SHARED OFF CACHE BOOL "" FORCE)
    set(CARES_OBJECT ON CACHE BOOL "" FORCE)
    set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    add_subdirectory(libares_V1_34_1)
    if(EVENT_DYNAMIC_LIB)
        set_target_properties(c-ares_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()
# nghttp2
# add_subdirectory(nghttp2_V1_61_0)

# build the static library
if(EVENT_STATIC_LIB)
    add_library(event_wrapper_static STATIC ${SOURCES} ${HEADERS})
    add_library(eular::event_wrapper_static ALIAS event_wrapper_static)

    target_include_directories(event_wrapper_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(event_wrapper_static
        PUBLIC
            event_static
    )
    if(EVENT_USE_CARES)
        target_sources(event_wrapper_static PRIVATE $<TARGET_OBJECTS:c-ares_objects>)
        target_include_directories(event_wrapper_static PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/libares_V1_34_1/include
            ${CMAKE_CURRENT_BINARY_DIR}/libares_V1_34_1
        )
        if(DEFINED CARES_DEPENDENT_LIBS)
            target_link_libraries(event_wrapper_static PRIVATE ${CARES_DEPENDENT_LIBS})
        endif()
    endif()

    if(EVENT_USE_CARES)
        target_compile_definitions(event_wrapper_static PRIVATE EVENT_WRAPPER_USE_CARES=1)
    else()
        target_compile_definitions(event_wrapper_static PRIVATE EVENT_WRAPPER_USE_CARES=0)
    endif()
    target_compile_definitions(event_wrapper_static PRIVATE EVENT_WRAPPER_STATIC)

    if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(event_wrapper_static PUBLIC pthread)
    endif()

    target_compile_options(
        event_wrapper_static PRIVATE
        ${CXX_COMPILER_FLAGS}
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4251 /wd4819>
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fvisibility=hidden -fvisibility-inlines-hidden>
    )

    set_property(TARGET event_wrapper_static PROPERTY ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    if(EVENT_SUBMODULE_ENABLE_INSTALL)
        install(TARGETS event_wrapper_static
            EXPORT event_wrapperTargets
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        )
    endif()
endif()

#build dynamic library
if(EVENT_DYNAMIC_LIB)
    add_library(event_wrapper SHARED ${SOURCES} ${HEADERS})
    add_library(eular::event_wrapper ALIAS event_wrapper)

    target_include_directories(event_wrapper
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(event_wrapper
        PUBLIC
            event_static
    )
    if(EVENT_USE_CARES)
        target_sources(event_wrapper PRIVATE $<TARGET_OBJECTS:c-ares_objects>)
        target_include_directories(event_wrapper PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/libares_V1_34_1/include
            ${CMAKE_CURRENT_BINARY_DIR}/libares_V1_34_1
        )
        if(DEFINED CARES_DEPENDENT_LIBS)
            target_link_libraries(event_wrapper PRIVATE ${CARES_DEPENDENT_LIBS})
        endif()
    endif()

    if(EVENT_USE_CARES)
        target_compile_definitions(event_wrapper PRIVATE EVENT_WRAPPER_USE_CARES=1)
    else()
        target_compile_definitions(event_wrapper PRIVATE EVENT_WRAPPER_USE_CARES=0)
    endif()

    if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(event_wrapper PUBLIC pthread)
    endif()

    target_compile_definitions(event_wrapper PRIVATE EVENT_WRAPPER_SHARED)
    target_compile_options(
        event_wrapper PRIVATE
        ${CXX_COMPILER_FLAGS}
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4251 /wd4819>
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fvisibility=hidden -fvisibility-inlines-hidden>
    )

    target_link_options(
        event_wrapper PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/IGNORE:4098>
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wl,--exclude-libs,ALL>
    )

    if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set_target_properties (
            event_wrapper
            PROPERTIES
            LINK_FLAGS "-Wl,-z,now,-z,relro"
        )
    endif ()

    set_property(TARGET event_wrapper PROPERTY LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    if(EVENT_SUBMODULE_ENABLE_INSTALL)
        install(TARGETS event_wrapper
            EXPORT event_wrapperTargets
            ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
            LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        )
    endif()
endif()

include(CMakePackageConfigHelpers)

if(EVENT_USE_CARES)
    set(EVENT_WRAPPER_NEED_CARES_FIND_PACKAGE "find_package(c-ares REQUIRED)")
else()
    set(EVENT_WRAPPER_NEED_CARES_FIND_PACKAGE "")
endif()

# 生成 xxxConfigVersion.cmake文件
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

# 用于生成 xxxConfig.cmake文件
configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION lib/cmake/${PROJECT_NAME}
)

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TEST)
    add_subdirectory(test)
endif()

if(EVENT_SUBMODULE_ENABLE_INSTALL)
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
        DESTINATION lib/cmake/${PROJECT_NAME}
    )

    export(
        EXPORT event_wrapperTargets
        NAMESPACE eular::
        FILE "cmake/${PROJECT_NAME}Targets.cmake")

    install(
        EXPORT event_wrapperTargets
        NAMESPACE eular::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
        FILE "${PROJECT_NAME}Targets.cmake")


    install(FILES ${HEADERS} DESTINATION "${INSTALL_INC_DIR}/event")

    # uninstall target
    if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake)
    endif()
endif()
