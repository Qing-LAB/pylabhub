# cmake/StageIfMissing.cmake
#
# Copies SRC → DST only when DST does not already exist.
# Used to deploy default configuration files without overwriting user edits.
#
# Usage (via add_custom_target / add_custom_command):
#   cmake -DSRC=<source_file> -DDST=<destination_file> -P StageIfMissing.cmake

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "StageIfMissing.cmake: SRC and DST must be defined.")
endif()

if(NOT EXISTS "${DST}")
    get_filename_component(_dst_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Staged default config: ${DST}")
else()
    message(STATUS "Preserved existing config: ${DST}")
endif()
