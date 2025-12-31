add_executable(aes_example aes_example.cc)
target_include_directories(aes_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
target_include_directories(aes_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

if(CRYPTOPP_STATIS_LIB MATCHES "ON")
target_link_libraries(aes_example cryptopp_static)
elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(aes_example cryptopp)
endif()

add_executable(crc32_example crc32_example.cc)
target_include_directories(crc32_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
target_include_directories(crc32_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

if(CRYPTOPP_STATIS_LIB MATCHES "ON")
target_link_libraries(crc32_example cryptopp_static)
elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(crc32_example cryptopp)
endif()

add_executable(base64_example base64_example.cc)
target_include_directories(base64_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
target_include_directories(base64_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

if(CRYPTOPP_STATIS_LIB MATCHES "ON")
target_link_libraries(base64_example cryptopp_static)
elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(base64_example cryptopp)
endif()

add_executable(md5_example md5_example.cc)
target_include_directories(md5_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
target_include_directories(md5_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

if(CRYPTOPP_STATIS_LIB MATCHES "ON")
target_link_libraries(md5_example cryptopp_static)
elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(md5_example cryptopp)
endif()

add_executable(rsa_example rsa_example.cc)
target_include_directories(rsa_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
target_include_directories(rsa_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

if(CRYPTOPP_STATIS_LIB MATCHES "ON")
target_link_libraries(rsa_example cryptopp_static)
elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
target_link_libraries(rsa_example cryptopp)
endif()

# add_executable(sha_example sha_example.cc)
# target_include_directories(sha_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/crypto)
# target_include_directories(sha_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal)

# if(CRYPTOPP_STATIS_LIB MATCHES "ON")
# target_link_libraries(sha_example cryptopp_static)
# elseif(CRYPTOPP_DYNAMIC_LIB MATCHES "ON")
# target_link_libraries(sha_example cryptopp)
# endif()
