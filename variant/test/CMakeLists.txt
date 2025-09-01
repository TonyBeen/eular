file(GLOB TEST_SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/variant/*.cc)

set(TEST_SRC_LIST
    ${TEST_SRC_LIST}
    main.cc
)

add_executable(main ${TEST_SRC_LIST})
target_link_libraries(main variant)
target_include_directories(main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/catch-1.12.0)
