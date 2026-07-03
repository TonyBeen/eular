# musl-toolchain.cmake
# 优先读取环境变量 MUSL_PATH，未设置时使用默认路径
if(DEFINED ENV{MUSL_PATH})
    set(MUSL_PATH "$ENV{MUSL_PATH}" CACHE PATH "musl toolchain root")
else()
    set(MUSL_PATH "/opt/musl-toolchain" CACHE PATH "musl toolchain root")
endif()

message(STATUS "Using musl toolchain at: ${MUSL_PATH}")

# 目标系统
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 工具链前缀
set(_MUSL_TRIPLE x86_64-linux-musl)
set(_MUSL_BIN    "${MUSL_PATH}/bin")

set(CMAKE_C_COMPILER   "${_MUSL_BIN}/${_MUSL_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${_MUSL_BIN}/${_MUSL_TRIPLE}-g++")
set(CMAKE_AR           "${_MUSL_BIN}/${_MUSL_TRIPLE}-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_MUSL_BIN}/${_MUSL_TRIPLE}-ranlib"  CACHE FILEPATH "")
set(CMAKE_STRIP        "${_MUSL_BIN}/${_MUSL_TRIPLE}-strip"   CACHE FILEPATH "")

# sysroot
set(CMAKE_SYSROOT "${MUSL_PATH}/${_MUSL_TRIPLE}")

# 只在 sysroot 内查找头文件和库，不使用宿主机路径
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 强制静态链接以提高可移植性
set(CMAKE_EXE_LINKER_FLAGS "-static" CACHE STRING "Linker flags for musl static linking")
# set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
