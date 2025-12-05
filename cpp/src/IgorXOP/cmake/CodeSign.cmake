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
#   - MAIN_EXECUTABLE_NAME: The name of the executable file inside the bundle's Contents/MacOS directory.
# ------------------------------------------------------------------------------

if(NOT DEFINED BUNDLE_PATH OR NOT EXISTS "${BUNDLE_PATH}")
  message(FATAL_ERROR "CodeSign.cmake: BUNDLE_PATH='${BUNDLE_PATH}' is not defined or does not exist.")
endif()

if(NOT DEFINED MAIN_EXECUTABLE_NAME OR "${MAIN_EXECUTABLE_NAME}" STREQUAL "")
  message(FATAL_ERROR "CodeSign.cmake: MAIN_EXECUTABLE_NAME is not defined.")
endif()

if(NOT DEFINED SIGNING_IDENTITY OR "${SIGNING_IDENTITY}" STREQUAL "")
  message(STATUS "Code signing skipped: MACOSX_CODESIGN_IDENTITY is not set.")
  return()
endif()

find_program(CODESIGN_EXECUTABLE codesign)
if(NOT CODESIGN_EXECUTABLE)
  message(FATAL_ERROR "Code signing failed: 'codesign' executable not found in PATH.")
endif()

message(STATUS "Signing bundle: ${BUNDLE_PATH} with identity: ${SIGNING_IDENTITY}")

set(MAIN_EXECUTABLE_PATH "${BUNDLE_PATH}/Contents/MacOS/${MAIN_EXECUTABLE_NAME}")

execute_process(
  COMMAND ${CODESIGN_EXECUTABLE}
          --force
          --options runtime
          --main-executable "${MAIN_EXECUTABLE_PATH}"
          --sign
          "${SIGNING_IDENTITY}"
          "${BUNDLE_PATH}"
  RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "Code signing failed with exit code ${result}.")
endif()