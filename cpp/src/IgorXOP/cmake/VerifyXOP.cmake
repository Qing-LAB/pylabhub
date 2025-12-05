# cpp/src/IgorXOP/cmake/VerifyXOP.cmake
#
# Verifies the structure and integrity of a newly assembled .xop bundle.
# This script is intended to be run by a POST_BUILD command.
#
# Expects:
#   - BUNDLE_TO_VERIFY: Path to the .xop bundle directory.
#   - EXECUTABLE_NAME: The expected name of the binary inside the bundle.

if(NOT DEFINED BUNDLE_TO_VERIFY OR NOT EXISTS "${BUNDLE_TO_VERIFY}")
    message(FATAL_ERROR "VerifyXOP.cmake: BUNDLE_TO_VERIFY='${BUNDLE_TO_VERIFY}' is not defined or does not exist.")
endif()
if(NOT DEFINED EXECUTABLE_NAME)
    message(FATAL_ERROR "VerifyXOP.cmake: EXECUTABLE_NAME was not provided.")
endif()

message(STATUS "Verifying XOP bundle: ${BUNDLE_TO_VERIFY}")

set(INFO_PLIST_PATH "${BUNDLE_TO_VERIFY}/Contents/Info.plist")
set(EXECUTABLE_PATH "${BUNDLE_TO_VERIFY}/Contents/MacOS/${EXECUTABLE_NAME}")

# 1. Check basic structure
if(NOT EXISTS "${BUNDLE_TO_VERIFY}/Contents/MacOS")
    message(FATAL_ERROR "Verification failed: Missing 'Contents/MacOS' directory in bundle.")
endif()
if(NOT EXISTS "${BUNDLE_TO_VERIFY}/Contents/Resources")
    message(FATAL_ERROR "Verification failed: Missing 'Contents/Resources' directory in bundle.")
endif()
if(NOT EXISTS "${INFO_PLIST_PATH}")
    message(FATAL_ERROR "Verification failed: Missing 'Contents/Info.plist' file in bundle.")
endif()
message(STATUS "  - Bundle structure................... OK")

# 2. Check for the executable file
if(NOT EXISTS "${EXECUTABLE_PATH}")
    message(FATAL_ERROR "Verification failed: Missing executable file at '${EXECUTABLE_PATH}'.")
endif()
message(STATUS "  - Executable file.................... OK")

# 3. Check for exported XOPMain symbol
find_program(NM_EXECUTABLE nm)
if(NOT NM_EXECUTABLE)
    message(WARNING "Verification warning: 'nm' tool not found. Skipping XOPMain symbol check.")
else()
    execute_process(
        COMMAND ${NM_EXECUTABLE} -gU "${EXECUTABLE_PATH}"
        COMMAND grep "_XOPMain"
        RESULT_VARIABLE grep_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT grep_result EQUAL 0)
        message(FATAL_ERROR "Verification failed: Required symbol '_XOPMain' not found or not exported in the executable.")
    endif()
    message(STATUS "  - Exported '_XOPMain' symbol....... OK")
endif()

# 4. Check Info.plist content (simple check for CFBundleExecutable)
find_program(PLUTIL_EXECUTABLE plutil)
if(NOT PLUTIL_EXECUTABLE)
    message(WARNING "Verification warning: 'plutil' tool not found. Skipping Info.plist content check.")
else()
    execute_process(
        COMMAND ${PLUTIL_EXECUTABLE} -p "${INFO_PLIST_PATH}"
        COMMAND grep "CFBundleExecutable"
        RESULT_VARIABLE grep_result
        OUTPUT_VARIABLE grep_output
        ERROR_QUIET
    )
    if(NOT grep_result EQUAL 0)
        message(FATAL_ERROR "Verification failed: Could not find 'CFBundleExecutable' key in Info.plist.")
    endif()
    if(NOT grep_output MATCHES "\"${EXECUTABLE_NAME}\"")
        message(FATAL_ERROR "Verification failed: 'CFBundleExecutable' key in Info.plist does not match expected executable name. Found: ${grep_output}")
    endif()
    message(STATUS "  - Info.plist CFBundleExecutable.... OK")
endif()

message(STATUS "XOP bundle verification successful.")
