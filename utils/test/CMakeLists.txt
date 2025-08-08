file(GLOB TEST_SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

if(UTILS_STATIC_LIB)
    set(LINK_LIBS eular::utils_static)
    set(CXX_FLAGS -DUTILS_STATIC)
elseif(UTILS_DYNAMIC_LIB)
    set(LINK_LIBS eular::utils)
    set(CXX_FLAGS)
endif()

foreach(test_file ${TEST_SRC_LIST})
    get_filename_component(test ${test_file} NAME_WE)
    
    add_executable(${test} ${test}.cc)
    target_link_libraries(${test} ${LINK_LIBS})
    target_include_directories(${test} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${test} PUBLIC ${CXX_FLAGS})
    target_compile_definitions(${test} PUBLIC "-DCATCH_CONFIG_MAIN -DCATCH_CONFIG_ENABLE_BENCHMARKING")
endforeach(test_file)
