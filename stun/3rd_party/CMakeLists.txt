cmake_minimum_required(VERSION 2.6.2)

project(stun-msg)

# set(CMAKE_C_FLAGS ${CMAKE_CXX_FLAGS})
# set(CMAKE_C_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})
# set(CMAKE_C_FLAGS_RELEASE ${CMAKE_CXX_FLAGS_RELEASE})
# set(CMAKE_C_FLAGS_MINSIZEREL ${CMAKE_CXX_FLAGS_MINSIZEREL})
# set(CMAKE_C_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELWITHDEBINFO})

include_directories(include)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-D_GNU_SOURCE -D__USE_FILE_OFFSET64 -D_LARGEFILE64_SOURCE)
    add_compile_options(-W -Wall -Werror -Wextra -Wno-deprecated)
    # -Wno-unused-result -Wno-unused-function -Wno-unused-parameter
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /WX)
endif()

add_library(stunmsg STATIC
    src/crc32.c     include/crc32.h
    src/hmac_sha1.c include/hmac_sha1.h
    src/sha1.c      include/sha1.h
    src/md5.c       include/md5.h
    src/stun_msg.c  include/stun/msg.h
)

target_include_directories(stunmsg
PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

set_target_properties(stunmsg PROPERTIES
    VERSION 0.1.0
    SOVERSION 0
    OUTPUT_NAME stunmsg
)
