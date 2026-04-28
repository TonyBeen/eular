set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests" FORCE)

set(C4CORE_DEV OFF CACHE BOOL "Disable c4core development targets" FORCE)
set(C4CORE_BUILD_TESTS OFF CACHE BOOL "Disable c4core tests" FORCE)
set(C4CORE_BUILD_BENCHMARKS OFF CACHE BOOL "Disable c4core benchmarks" FORCE)
set(C4CORE_INSTALL ON CACHE BOOL "Disable c4core install target" FORCE)

add_subdirectory(c4core)
