# /cmake/PreInstallCheck.cmake

# This script is executed at the beginning of the installation process.
# It checks for the existence of a sentinel file to ensure that the 'stage_all'
# target has been successfully run before attempting to install.

# The top-level CMakeLists.txt sets PYLABHUB_STAGING_DIR. During the install
# script execution, this variable is passed down.
if(NOT EXISTS "${PYLABHUB_STAGING_DIR}/.stage_complete")
  message(FATAL_ERROR "Installation cannot proceed because the staging directory is incomplete. "
    "Please build the 'stage_all' target first.\n"
    "Run this command from your build directory:\n"
    "  cmake --build . --target stage_all\n"
  )
else()
  message(STATUS "[pylabhub-install] Pre-install check passed: Staging directory is complete.")
endif()

