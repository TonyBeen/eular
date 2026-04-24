file(GLOB TEST_SRC_LIST CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
set(TEST_MAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cc)
list(REMOVE_ITEM TEST_SRC_LIST ${TEST_MAIN_FILE})

if(UTILS_STATIC_LIB)
    set(LINK_LIBS eular::utils_static)
    set(TEST_DEFINITIONS UTILS_STATIC)
elseif(UTILS_DYNAMIC_LIB)
    set(LINK_LIBS eular::utils)
    set(TEST_DEFINITIONS)
endif()

set(ALL_TEST_TARGETS)

add_library(utils_test_main_obj OBJECT ${TEST_MAIN_FILE})
target_compile_definitions(utils_test_main_obj
    PUBLIC
        ${TEST_DEFINITIONS}
        CATCH_CONFIG_ENABLE_BENCHMARKING
        CATCH_CONFIG_NO_POSIX_SIGNALS)
target_include_directories(utils_test_main_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(utils_test_main_obj PRIVATE /utf-8)
endif()

foreach(test_file ${TEST_SRC_LIST})
    get_filename_component(test ${test_file} NAME_WE)

    add_executable(${test} ${test_file} $<TARGET_OBJECTS:utils_test_main_obj>)
    target_compile_definitions(${test}
        PUBLIC
            ${TEST_DEFINITIONS}
            CATCH_CONFIG_ENABLE_BENCHMARKING
            CATCH_CONFIG_NO_POSIX_SIGNALS)

    target_link_libraries(${test} ${LINK_LIBS})
    target_include_directories(${test} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${test} PRIVATE /utf-8)
    endif()
    add_test(NAME ${test} COMMAND ${test})
    list(APPEND ALL_TEST_TARGETS ${test})
endforeach(test_file)

add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${ALL_TEST_TARGETS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Build and run all utils tests")
