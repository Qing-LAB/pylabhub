# cpp/src/IgorXOP/cmake/process_bundle.cmake
# This script processes a fully assembled .xop bundle.
# It performs cleaning, signing, and verification.
#
# Expects the following variables to be passed via -D:
# - BUNDLE_PATH: Path to the .xop bundle to process.
# - EXECUTABLE_NAME: The name of the Mach-O executable inside the bundle.
# - SIGNING_IDENTITY: The identity to use for code signing.
# - CODESIGN_SCRIPT_PATH: Path to the CodeSign.cmake script.
# - VERIFY_SCRIPT_PATH: Path to the VerifyXOP.cmake script.

if(NOT DEFINED BUNDLE_PATH OR NOT EXISTS "${BUNDLE_PATH}")
  message(FATAL_ERROR "process_bundle.cmake: BUNDLE_PATH='${BUNDLE_PATH}' is not defined or does not exist.")
endif()

# --- Step 1: Clean extended attributes ---
find_program(XATTR_EXECUTABLE xattr HINTS /usr/bin)
if(XATTR_EXECUTABLE)
  message(STATUS "Processing bundle: Cleaning extended attributes with xattr...")
  execute_process(COMMAND ${XATTR_EXECUTABLE} -cr "${BUNDLE_PATH}" RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(WARNING "xattr command failed with exit code ${result} for bundle: ${BUNDLE_PATH}")
  endif()
else()
  message(WARNING "Processing bundle: 'xattr' tool not found. Skipping cleaning of extended attributes.")
endif()

# --- Step 2: Sign the bundle ---
if(DEFINED CODESIGN_SCRIPT_PATH AND EXISTS "${CODESIGN_SCRIPT_PATH}")
  message(STATUS "Processing bundle: Running code signing script...")
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      -D "BUNDLE_PATH=${BUNDLE_PATH}"
      -D "SIGNING_IDENTITY=${SIGNING_IDENTITY}"
      -P "${CODESIGN_SCRIPT_PATH}"
    RESULT_VARIABLE result
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Code signing script failed with exit code ${result}.")
  endif()
else()
    message(WARNING "Processing bundle: CODESIGN_SCRIPT_PATH not provided. Skipping signing.")
endif()

# --- Step 3: Verify the bundle ---
if(DEFINED VERIFY_SCRIPT_PATH AND EXISTS "${VERIFY_SCRIPT_PATH}")
  message(STATUS "Processing bundle: Running verification script...")
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      -D "BUNDLE_TO_VERIFY=${BUNDLE_PATH}"
      -D "EXECUTABLE_NAME=${EXECUTABLE_NAME}"
      -P "${VERIFY_SCRIPT_PATH}"
    RESULT_VARIABLE result
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Bundle verification script failed with exit code ${result}.")
  endif()
else()
    message(WARNING "Processing bundle: VERIFY_SCRIPT_PATH not provided. Skipping verification.")
endif()

message(STATUS "Bundle processing steps completed successfully.")
