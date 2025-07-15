list(APPEND EXAMPLES
    aes_example
)

add_executable(aes_example aes_example.cc)
target_include_directories(aes_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
target_include_directories(aes_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

if(CRYPTOPP_STATIS_LIB MATCHES "ON")
target_link_libraries(aes_example cryptopp_static)
elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(aes_example cryptopp)
endif()
