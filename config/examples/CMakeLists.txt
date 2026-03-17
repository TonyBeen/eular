set(CONFIG_EXAMPLE_SOURCES
    example_ini_complex.cpp
    example_xml_complex.cpp
    example_yaml_complex.cpp
    example_json_complex.cpp
)

foreach(example_source ${CONFIG_EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_source} NAME_WE)

    add_executable(${example_name} ${example_source})
    target_link_libraries(${example_name} PRIVATE config::config)
endforeach()
