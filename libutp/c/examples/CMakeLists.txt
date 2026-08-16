find_package(Threads REQUIRED)

set(UTP_C_ECHO_EXAMPLES
    utp_c_echo_server
    utp_c_echo_client
)

add_executable(utp_c_echo_server echo_server.c)
add_executable(utp_c_echo_client echo_client.c)

foreach(example_target IN LISTS UTP_C_ECHO_EXAMPLES)
    target_link_libraries(${example_target} PRIVATE utp_c event_core Threads::Threads)
    target_include_directories(${example_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../3rd")
    target_compile_features(${example_target} PRIVATE c_std_11)
    if(UTP_C_SANITIZER_FLAGS)
        target_compile_options(${example_target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
        target_link_options(${example_target} PRIVATE ${UTP_C_SANITIZER_FLAGS})
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${example_target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
    elseif(MSVC)
        target_compile_options(${example_target} PRIVATE /W4 /WX)
    endif()
endforeach()
