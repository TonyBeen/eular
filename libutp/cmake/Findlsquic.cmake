find_path(LSQUIC_INCLUDE_DIR
    NAMES lsquic/lsquic.h
    PATHS /usr/local/include /usr/include
)

find_library(LSQUIC_LIBRARY
    NAMES lsquic
    PATHS /usr/local/lib /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(lsquic DEFAULT_MSG LSQUIC_INCLUDE_DIR LSQUIC_LIBRARY)

if(lsquic_FOUND AND NOT TARGET lsquic::lsquic)
    add_library(lsquic::lsquic UNKNOWN IMPORTED)
    set_target_properties(lsquic::lsquic PROPERTIES
        IMPORTED_LOCATION "${LSQUIC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LSQUIC_INCLUDE_DIR}"
    )
endif()
