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

# Find the codesign executable.
# On macOS, the system `codesign` is located at `/usr/bin/codesign`.
# We use this absolute path to avoid issues with environments like Conda that
# can prepend their own, potentially incompatible, tools to the PATH.
if(EXISTS "/usr/bin/codesign")
  set(CODESIGN_EXECUTABLE "/usr/bin/codesign")
  message(STATUS "Found codesign executable at /usr/bin/codesign")
else()
  message(FATAL_ERROR "Could not find codesign at the expected system path: /usr/bin/codesign. Please ensure the Xcode Command Line Tools are installed correctly.")
endif()

if(NOT CODESIGN_EXECUTABLE)
  message(FATAL_ERROR "Code signing failed: 'codesign' executable not found in PATH.")
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