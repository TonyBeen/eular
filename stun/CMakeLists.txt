cmake_minimum_required(VERSION 3.15)

project(stun
    VERSION 1.0.0
    DESCRIPTION "STUN protocol implementation"
    LANGUAGES C CXX
)

# Options
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build shared library" OFF)

# C++ standard
set(CMAKE_CXX_STANDARD 11)

find_package(Libevent REQUIRED)

set(STUN_LIBEVENT_LIBS ${LIBEVENT_LIBRARIES})
if(NOT STUN_LIBEVENT_LIBS)
    set(STUN_LIBEVENT_LIBS ${LIBEVENT_STATIC_LIBRARIES})
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        # $<$<CONFIG:RELEASE>:-O2 -flto>
    )
endif()

# Source files
set(SOURCES
    src/socket_address.cpp
    src/stun.cpp
    src/ntrs_codec.cpp
    src/mqtt_client.cpp
)

# Add library/executable
if(BUILD_SHARED_LIBS)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    target_link_libraries(${PROJECT_NAME} PRIVATE stunmsg utils)
else()
    add_library(${PROJECT_NAME} STATIC ${SOURCES})
    target_link_libraries(${PROJECT_NAME} PRIVATE stunmsg)
endif()

# Include directories
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# 3rd party
add_subdirectory(3rd_party)

# Build libmosquitto only (no broker/tools/tests) from vendored source.
set(WITH_BROKER OFF CACHE BOOL "" FORCE)
set(WITH_CLIENTS OFF CACHE BOOL "" FORCE)
set(WITH_APPS OFF CACHE BOOL "" FORCE)
set(WITH_PLUGINS OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_LIB_CPP OFF CACHE BOOL "" FORCE)
set(WITH_STATIC_LIBRARIES OFF CACHE BOOL "" FORCE)
set(WITH_CTRL_SHELL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rd_party/mosquitto EXCLUDE_FROM_ALL)

# Link against libmosquitto and OpenSSL
target_link_libraries(${PROJECT_NAME} PUBLIC
    libmosquitto
    ${STUN_LIBEVENT_LIBS}
    pthread
    crypto
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mosquitto/include
    ${LIBEVENT_INCLUDE_DIRS}
)

# Testing
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Install
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/ DESTINATION include)

# Export
# install(EXPORT ${PROJECT_NAME}Targets
#     FILE ${PROJECT_NAME}Targets.cmake
#     NAMESPACE ${PROJECT_NAME}::
#     DESTINATION lib/cmake/${PROJECT_NAME}
# )
