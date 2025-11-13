# cmake/assemble_xop.cmake
#
# Usage:
#   cmake -P assemble_xop.cmake <built_target_path> <desired_bundle_dir> <bundle_filename>
#
# This script is intentionally conservative and portable:
#  - If built_target_path is already a bundle (folder with Contents/MacOS) it does minimal checks.
#  - If built_target_path is a single Mach-O, it creates the bundle folder structure,
#    copies the Mach-O into Contents/MacOS/, copies configured Info.plist if present,
#    copies any .r resources from the source dir, and sets exec permissions.
#
cmake_minimum_required(VERSION 3.0)

# Validate args
if(NOT ARGC GREATER 0)
  message(FATAL_ERROR "assemble_xop.cmake: missing arguments. Usage: cmake -P assemble_xop.cmake <built_target_path> <bundle_dir> <bundle_filename>")
endif()

set(built_target "${ARGV0}")
set(desired_bundle_dir "")
if(ARGC GREATER 1)
  set(desired_bundle_dir "${ARGV1}")
endif()
set(bundle_filename "")
if(ARGC GREATER 2)
  set(bundle_filename "${ARGV2}")
endif()

message(STATUS "assemble_xop.cmake: built_target='${built_target}'")
message(STATUS "assemble_xop.cmake: desired_bundle_dir='${desired_bundle_dir}'")
message(STATUS "assemble_xop.cmake: bundle_filename='${bundle_filename}'")

# Sanity: built target must exist
if(NOT EXISTS "${built_target}")
  message(FATAL_ERROR "assemble_xop.cmake: built target does not exist: ${built_target}")
endif()

# Normalize absolute paths (CMake generator-expr usually yields absolute)
get_filename_component(built_target_abs "${built_target}" ABSOLUTE)

# Derive a default desired_bundle_dir if not provided
if(desired_bundle_dir STREQUAL "")
  get_filename_component(basedir "${built_target_abs}" DIRECTORY)
  if(bundle_filename STREQUAL "")
    message(FATAL_ERROR "assemble_xop.cmake: bundle filename not provided and cannot be derived")
  endif()
  set(desired_bundle_dir "${basedir}/${bundle_filename}.bundle")
endif()

# Bundle canonical paths
set(bundle_contents "${desired_bundle_dir}/Contents")
set(bundle_macos_dir "${bundle_contents}/MacOS")
set(bundle_resources_dir "${bundle_contents}/Resources")

# If already a bundle, ensure Info.plist presence and return
if(IS_DIRECTORY "${built_target_abs}")
  if(EXISTS "${built_target_abs}/Contents/MacOS")
    message(STATUS "assemble_xop.cmake: detected built target is already a bundle. Ensuring Info.plist exists...")
    if(NOT EXISTS "${built_target_abs}/Contents/Info.plist")
      # Prefer configured Info.plist from the build tree (common place used by CMake configure_file).
      set(configured_plist "${CMAKE_BINARY_DIR}/src/IgorXOP/Info.plist")
      if(EXISTS "${configured_plist}")
        file(COPY "${configured_plist}" DESTINATION "${built_target_abs}/Contents")
        message(STATUS "assemble_xop.cmake: copied configured Info.plist into existing bundle")
      else()
        message(WARNING "assemble_xop.cmake: Info.plist missing in bundle and no configured Info.plist found at ${configured_plist}")
      endif()
    endif()
    message(STATUS "assemble_xop.cmake: bundle ready at ${built_target_abs}")
    return()
  endif()
endif()

# Create bundle structure
message(STATUS "assemble_xop.cmake: packaging single file into bundle: ${desired_bundle_dir}")
file(MAKE_DIRECTORY "${bundle_macos_dir}")
file(MAKE_DIRECTORY "${bundle_resources_dir}")

# Determine destination executable name.
# Prefer XOP_BUNDLE_EXECUTABLE if passed in environment; otherwise use filename of built target.
if(DEFINED ENV{XOP_BUNDLE_EXECUTABLE})
  set(dest_exec_name "$ENV{XOP_BUNDLE_EXECUTABLE}")
else()
  get_filename_component(dest_exec_name "${built_target_abs}" NAME)
endif()
set(dest_exec "${bundle_macos_dir}/${dest_exec_name}")

# Copy the built Mach-O into the bundle's MacOS directory
file(COPY "${built_target_abs}" DESTINATION "${bundle_macos_dir}")

# Ensure executable bit (best-effort)
execute_process(COMMAND /bin/chmod +x "${dest_exec}"
                RESULT_VARIABLE chmod_res
                OUTPUT_VARIABLE chmod_out
                ERROR_VARIABLE chmod_err
                )

if(NOT chmod_res EQUAL 0)
  message(WARNING "assemble_xop.cmake: could not set executable permissions on ${dest_exec}: ${chmod_err}")
endif()

# Copy Info.plist if configured during configure-time (common location)
set(configured_plist "${CMAKE_BINARY_DIR}/src/IgorXOP/Info.plist")
if(EXISTS "${configured_plist}")
  file(COPY "${configured_plist}" DESTINATION "${bundle_contents}")
  message(STATUS "assemble_xop.cmake: copied configured Info.plist into bundle")
else()
  # Minimal fallback Info.plist (keeps Igor happy)
  message(WARNING "assemble_xop.cmake: configured Info.plist not found at ${configured_plist}; writing minimal Info.plist")
  file(WRITE "${bundle_contents}/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict><key>CFBundlePackageType</key><string>IXOP</string><key>CFBundleName</key><string>${bundle_filename}</string><key>CFBundleExecutable</key><string>${dest_exec_name}</string></dict></plist>\n")
endif()

# Copy resource files (.r) adjacent to the source dir into Resources/ if present
set(src_resources_dir "${CMAKE_SOURCE_DIR}/src/IgorXOP")
file(GLOB r_files "${src_resources_dir}/*.r")
if(r_files)
  foreach(rfile IN LISTS r_files)
    file(COPY "${rfile}" DESTINATION "${bundle_resources_dir}")
  endforeach()
  message(STATUS "assemble_xop.cmake: copied resource files into bundle Resources/")
endif()

message(STATUS "assemble_xop.cmake: created bundle at ${desired_bundle_dir}")
