cmake_minimum_required(VERSION 3.16)

project(fcontext VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
option(FCONTEXT_BUILD_TESTS "Build fcontext unit tests" ${BUILD_TESTING})
option(FCONTEXT_BUILD_EXAMPLES "Build fcontext examples" ON)

set(FCONTEXT_ARCHITECTURE "" CACHE STRING
    "Target architecture (arm, arm64, i386, x86_64, ...)")
set(FCONTEXT_ABI "" CACHE STRING
    "Target ABI (aapcs, ms, o32, n64, sysv)")
set(FCONTEXT_BINARY_FORMAT "" CACHE STRING
    "Target binary format (elf, macho, pe, xcoff)")
set(FCONTEXT_ASSEMBLER "" CACHE STRING
    "Assembler implementation (gas, masm, armasm, armclang)")

if(NOT FCONTEXT_BINARY_FORMAT)
    if(APPLE)
        set(FCONTEXT_BINARY_FORMAT macho)
    elseif(WIN32 OR CYGWIN)
        set(FCONTEXT_BINARY_FORMAT pe)
    else()
        set(FCONTEXT_BINARY_FORMAT elf)
    endif()
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _processor)

if(NOT FCONTEXT_ARCHITECTURE)
    if(_processor MATCHES "^(aarch64|arm64)$")
        set(FCONTEXT_ARCHITECTURE arm64)
    elseif(_processor MATCHES "^(arm|armv[5-8].*)$")
        set(FCONTEXT_ARCHITECTURE arm)
    elseif(_processor MATCHES "^(x86_64|amd64)$")
        set(FCONTEXT_ARCHITECTURE x86_64)
    elseif(_processor MATCHES "^(i[3-6]86|x86)$")
        set(FCONTEXT_ARCHITECTURE i386)
    elseif(_processor MATCHES "^mips64")
        set(FCONTEXT_ARCHITECTURE mips64)
    elseif(_processor MATCHES "^mips")
        set(FCONTEXT_ARCHITECTURE mips32)
    elseif(_processor MATCHES "^(powerpc64|ppc64.*)$")
        set(FCONTEXT_ARCHITECTURE ppc64)
    elseif(_processor MATCHES "^(powerpc|ppc.*)$")
        set(FCONTEXT_ARCHITECTURE ppc32)
    elseif(_processor MATCHES "^riscv64")
        set(FCONTEXT_ARCHITECTURE riscv64)
    elseif(_processor MATCHES "^s390x")
        set(FCONTEXT_ARCHITECTURE s390x)
    elseif(_processor MATCHES "^sparc64")
        set(FCONTEXT_ARCHITECTURE sparc64)
    elseif(_processor MATCHES "^loongarch64")
        set(FCONTEXT_ARCHITECTURE loongarch64)
    else()
        message(FATAL_ERROR "Cannot determine target architecture from '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
endif()

if(NOT FCONTEXT_ABI)
    if(FCONTEXT_ARCHITECTURE MATCHES "^(arm|arm64)$")
        set(FCONTEXT_ABI aapcs)
    elseif(FCONTEXT_BINARY_FORMAT STREQUAL "pe")
        set(FCONTEXT_ABI ms)
    elseif(FCONTEXT_ARCHITECTURE STREQUAL "mips32")
        set(FCONTEXT_ABI o32)
    elseif(FCONTEXT_ARCHITECTURE STREQUAL "mips64")
        set(FCONTEXT_ABI n64)
    else()
        set(FCONTEXT_ABI sysv)
    endif()
endif()

if(NOT FCONTEXT_ASSEMBLER)
    if(MSVC)
        if(FCONTEXT_ARCHITECTURE MATCHES "^(arm|arm64)$")
            set(FCONTEXT_ASSEMBLER armasm)
        else()
            set(FCONTEXT_ASSEMBLER masm)
        endif()
    else()
        set(FCONTEXT_ASSEMBLER gas)
    endif()
endif()

if(NOT FCONTEXT_ASM_SUFFIX)
    if(FCONTEXT_ASSEMBLER STREQUAL "armclang")
        set(FCONTEXT_ASM_SUFFIX .S)
    elseif(FCONTEXT_BINARY_FORMAT STREQUAL "pe")
        set(FCONTEXT_ASM_SUFFIX .asm)
    else()
        set(FCONTEXT_ASM_SUFFIX .S)
    endif()
endif()

if(FCONTEXT_BINARY_FORMAT STREQUAL "mach-o")
    set(FCONTEXT_BINARY_FORMAT macho)
endif()

set(_fcontext_suffix
    "${FCONTEXT_ARCHITECTURE}_${FCONTEXT_ABI}_${FCONTEXT_BINARY_FORMAT}_${FCONTEXT_ASSEMBLER}${FCONTEXT_ASM_SUFFIX}")
set(_fcontext_asm_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/asm")
set(_fcontext_asm_sources
    "${_fcontext_asm_dir}/make_${_fcontext_suffix}"
    "${_fcontext_asm_dir}/jump_${_fcontext_suffix}"
    "${_fcontext_asm_dir}/ontop_${_fcontext_suffix}")

foreach(_source IN LISTS _fcontext_asm_sources)
    if(NOT EXISTS "${_source}")
        message(FATAL_ERROR
            "Missing assembler backend '${_source}'. "
            "Set FCONTEXT_ARCHITECTURE, FCONTEXT_ABI, "
            "FCONTEXT_BINARY_FORMAT and FCONTEXT_ASSEMBLER explicitly.")
    endif()
endforeach()

if(FCONTEXT_ASSEMBLER STREQUAL "masm")
    enable_language(ASM_MASM)
    set_source_files_properties(${_fcontext_asm_sources}
        PROPERTIES LANGUAGE ASM_MASM)
elseif(FCONTEXT_ASSEMBLER STREQUAL "armasm")
    enable_language(ASM_MARMASM)
    set_source_files_properties(${_fcontext_asm_sources}
        PROPERTIES LANGUAGE ASM_MARMASM)
else()
    enable_language(ASM)
    set_source_files_properties(${_fcontext_asm_sources}
        PROPERTIES LANGUAGE ASM)
endif()

add_library(fcontext STATIC
    ${_fcontext_asm_sources})
add_library(fcontext::fcontext ALIAS fcontext)

target_include_directories(fcontext
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fcontext PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS fcontext
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES include/fcontext/fcontext.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fcontext")

if(BUILD_TESTING AND FCONTEXT_BUILD_TESTS)
    add_subdirectory(test)
endif()

if(FCONTEXT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

message(STATUS "fcontext: architecture ${FCONTEXT_ARCHITECTURE}, ABI ${FCONTEXT_ABI}, "
    "format ${FCONTEXT_BINARY_FORMAT}, assembler ${FCONTEXT_ASSEMBLER}")
