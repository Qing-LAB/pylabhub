# cmake/BulkStagePrerequisites.cmake
#
# This script performs the bulk staging of pre-built external project artifacts.
# It is designed to be executed via `cmake -P` and expects the following
# CMake variables to be defined:
# - PREREQ_DIR: The source directory containing the installed prerequisites.
# - STAGING_DIR: The destination unified staging directory.
#
# This script copies common directories (lib, include, share, package) from
# PREREQ_DIR to STAGING_DIR and handles platform-specific needs, such as
# moving DLLs on Windows.
#

if(NOT DEFINED PREREQ_DIR OR NOT DEFINED STAGING_DIR)
  message(FATAL_ERROR "BulkStagePrerequisites.cmake requires PREREQ_DIR and STAGING_DIR variables to be defined.")
endif()

message(STATUS "Bulk staging prerequisites from: ${PREREQ_DIR}")

set(subdirs_to_copy bin lib include share)

foreach(subdir ${subdirs_to_copy})
  set(source_path "${PREREQ_DIR}/${subdir}")
  set(dest_path "${STAGING_DIR}/${subdir}")
  if(EXISTS "${source_path}")
    message(STATUS "  - Staging directory: ${source_path} -> ${dest_path}")
    file(COPY_DIRECTORY "${source_path}/" DESTINATION "${dest_path}/")
  else()
    message(STATUS "  - Skipping non-existent prerequisite directory: ${source_path}")
  endif()
endforeach()

# On Windows, find DLLs in the staged lib directory and copy them to bin/ and tests/
if(WIN32) # Use WIN32 as a proxy for PYLABHUB_IS_WINDOWS here, as this script runs in isolation
  set(staged_lib_dir "${STAGING_DIR}/lib")
  if(EXISTS "${staged_lib_dir}")
      file(GLOB_RECURSE dlls_to_stage "${staged_lib_dir}/*.dll")

      if(dlls_to_stage)
          message(STATUS "  - Found DLLs to stage to runtime directories: ${dlls_to_stage}")
          set(runtime_dest_bin "${STAGING_DIR}/bin")
          set(runtime_dest_tests "${STAGING_DIR}/tests")

          file(COPY ${dlls_to_stage} DESTINATION "${runtime_dest_bin}")
          file(COPY ${dlls_to_stage} DESTINATION "${runtime_dest_tests}")
          message(STATUS "  - Copied DLLs to ${runtime_dest_bin} and ${runtime_dest_tests}")
      endif()
  endif()
endif()
