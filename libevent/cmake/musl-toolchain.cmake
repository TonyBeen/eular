# musl-toolchain.cmake
# Prefer MUSL_PATH from the environment; fall back to the default install root.
if(DEFINED ENV{MUSL_PATH})
    set(MUSL_PATH "$ENV{MUSL_PATH}" CACHE PATH "musl toolchain root")
else()
    set(MUSL_PATH "/opt/musl-toolchain" CACHE PATH "musl toolchain root")
endif()

message(STATUS "Using musl toolchain at: ${MUSL_PATH}")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_MUSL_TRIPLE x86_64-linux-musl)
set(_MUSL_BIN "${MUSL_PATH}/bin")

set(CMAKE_C_COMPILER "${_MUSL_BIN}/${_MUSL_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${_MUSL_BIN}/${_MUSL_TRIPLE}-g++")
set(CMAKE_AR "${_MUSL_BIN}/${_MUSL_TRIPLE}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_MUSL_BIN}/${_MUSL_TRIPLE}-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${_MUSL_BIN}/${_MUSL_TRIPLE}-strip" CACHE FILEPATH "")

set(CMAKE_SYSROOT "${MUSL_PATH}/${_MUSL_TRIPLE}")

# Search headers and libraries in the musl sysroot instead of the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Standalone musl builds are typically consumed as static artifacts.
set(CMAKE_EXE_LINKER_FLAGS "-static" CACHE STRING "Linker flags for musl static linking")
