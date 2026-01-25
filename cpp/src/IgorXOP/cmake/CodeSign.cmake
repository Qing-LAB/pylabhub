# cmake/CodeSign.cmake
# ------------------------------------------------------------------------------
# Conditionally signs a macOS bundle.
#
# This script is executed by a POST_BUILD custom command. It finds the `codesign`
# tool and executes it only if a signing identity is provided.
#
# Expects the following variables to be passed via -D on the command line:
#   - BUNDLE_PATH: The full path to the .xop bundle to be signed.
#   - SIGNING_IDENTITY: The identity to use for signing (e.g., "Developer ID Application: Your Name").
# ------------------------------------------------------------------------------

if(NOT DEFINED BUNDLE_PATH OR NOT EXISTS "${BUNDLE_PATH}")
  message(FATAL_ERROR "CodeSign.cmake: BUNDLE_PATH='${BUNDLE_PATH}' is not defined or does not exist.")
endif()

if(NOT DEFINED SIGNING_IDENTITY OR "${SIGNING_IDENTITY}" STREQUAL "")
  message(STATUS "Code signing skipped: MACOSX_CODESIGN_IDENTITY is not set.")
  return()
endif()

# Find Apple's codesign executable.
# First, try xcrun to get the tool from the active Xcode toolchain.
execute_process(
  COMMAND xcrun --find codesign
  OUTPUT_VARIABLE CODESIGN_EXECUTABLE
  RESULT_VARIABLE _xcrun_result
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)

# If xcrun fails, fall back to the standard system location for command line tools.
if(NOT _xcrun_result EQUAL 0 OR NOT EXISTS "${CODESIGN_EXECUTABLE}")
  if(EXISTS "/usr/bin/codesign")
    set(CODESIGN_EXECUTABLE "/usr/bin/codesign")
  else()
    set(CODESIGN_EXECUTABLE) # Explicitly clear the variable
  endif()
endif()

# If neither method found the official tool, exit with an error.
if(NOT CODESIGN_EXECUTABLE)
  message(FATAL_ERROR "Code signing failed: Apple's 'codesign' executable not found via 'xcrun' or at '/usr/bin/codesign'. Please ensure the Xcode Command Line Tools are installed correctly.")
else()
  message(STATUS "Found Apple codesign executable at: ${CODESIGN_EXECUTABLE}")
endif()

message(STATUS "Signing bundle: ${BUNDLE_PATH} with identity: ${SIGNING_IDENTITY}")

# Use the simple, robust command confirmed to work with the official codesign tool.
# Separate arguments are used for robustness with execute_process.
execute_process(
  COMMAND ${CODESIGN_EXECUTABLE}
          -f
          -s
          "${SIGNING_IDENTITY}"
          "${BUNDLE_PATH}"
  RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "Code signing failed with exit code ${result}.")
endif()