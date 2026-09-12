find_package(Threads REQUIRED)

set(UTP_C_EXAMPLES
    utp_c_echo_server
    utp_c_echo_client
    ntrsc
    nat_punch
)

add_executable(utp_c_echo_server echo_server.c)
add_executable(utp_c_echo_client echo_client.c)
add_executable(ntrsc ntrsc.cpp)
add_executable(nat_punch nat_punch.cpp)

foreach(example_target IN LISTS UTP_C_EXAMPLES)
    target_link_libraries(${example_target} PRIVATE utp_c event_core Threads::Threads)
    target_include_directories(${example_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../3rd")
    target_compile_features(${example_target} PRIVATE c_std_11)
    if(UTP_C_SANITIZER_FLAGS)
        target_compile_options(${example_target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
        target_link_options(${example_target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        if(example_target STREQUAL "ntrsc" OR example_target STREQUAL "nat_punch")
            target_compile_options(${example_target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
                -Wformat=2)
            if(example_target STREQUAL "ntrsc")
                # C 日志桥接到 C++ 时需要透传受控的可变格式串。
                set_source_files_properties(ntrsc.cpp PROPERTIES
                    COMPILE_OPTIONS "-Wno-format-nonliteral")
            endif()
        else()
            target_compile_options(${example_target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
                -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
        endif()
    elseif(MSVC)
        target_compile_options(${example_target} PRIVATE /W4 /WX)
    endif()
endforeach()

target_compile_features(nat_punch PRIVATE cxx_std_11)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nat_punch PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wformat=2)
endif()

target_include_directories(ntrsc SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/include")
target_include_directories(nat_punch SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/include")
target_compile_features(ntrsc PRIVATE cxx_std_11)
