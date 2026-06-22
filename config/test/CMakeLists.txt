set(CONFIG_TEST_SOURCES
    test_ini_config_catch.cc
    test_xml_config_catch.cc
    test_json_parser_node_catch.cc
    test_yaml_parser_node_catch.cc
    test_config_observer_catch.cc
    test_config_observer_perf.cc
)

if(TARGET config_static)
    set(CONFIG_TEST_LINK_LIBRARY config_static)
    set(CONFIG_TEST_SUFFIX static)
elseif(TARGET config_shared)
    set(CONFIG_TEST_LINK_LIBRARY config_shared)
    set(CONFIG_TEST_SUFFIX shared)
else()
    message(FATAL_ERROR "No config library target available for tests")
endif()

foreach(test_source ${CONFIG_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    set(test_target ${test_name}_${CONFIG_TEST_SUFFIX})
    add_executable(${test_target} ${test_source})
    target_include_directories(${test_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${test_target} PRIVATE CONFIG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(test_source STREQUAL "test_config_observer_perf.cc")
        target_compile_definitions(${test_target} PRIVATE CONFIG_TEST_PERF)
    endif()
    target_link_libraries(${test_target} PRIVATE ${CONFIG_TEST_LINK_LIBRARY} Threads::Threads)
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
