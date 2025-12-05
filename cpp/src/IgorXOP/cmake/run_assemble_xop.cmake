# cmake/run_assemble_xop.cmake.in
# ------------------------------------------------------------------------------
# This is a "bootstrap" script executed by a POST_BUILD command.
#
# Its purpose is to take build-time variables (passed via -D on the command line)
# and use them to configure the *actual* assembly script. This two-step process
# is necessary to correctly resolve CMake generator expressions like
# $<TARGET_FILE:...> into concrete paths.
# ------------------------------------------------------------------------------

message(STATUS "run_assemble_xop.cmake: Bootstrapping assembly process...")

# 1. Validate that the required build-time path was passed in.
if(NOT DEFINED MACHO_BINARY_PATH_ARG)
  message(FATAL_ERROR "MACHO_BINARY_PATH_ARG was not provided to the bootstrap script.")
endif()

# 2. Set the variable that the final assembly script template expects.
set(MACHO_BINARY_PATH "${MACHO_BINARY_PATH_ARG}")

# 3. Use configure_file to create the final, fully-resolved assembly script.
#    All other variables (@BUNDLE_DIR@, @XOP_BUNDLE_NAME@, etc.) were already
#    set at the initial CMake configure time.
configure_file("@ASSEMBLE_SCRIPT_TEMPLATE@" "@CONFIGURED_ASSEMBLE_SCRIPT@" @ONLY)

# 4. Execute the final assembly script.
message(STATUS "run_assemble_xop.cmake: Executing final assembly script: @CONFIGURED_ASSEMBLE_SCRIPT@")
include("@CONFIGURED_ASSEMBLE_SCRIPT@")

