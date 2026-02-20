#!/usr/bin/env bash
# ==============================================================================
# Standalone XOP Bundle Assembly Script for macOS
#
# This script manually assembles, processes, signs, and verifies an Igor Pro
# XOP bundle. It is designed to be run from the command line and replicates the
# logic from the project's automated CMake post-build process.
# ==============================================================================
set -euo pipefail

# --- Usage ---
if [ "$#" -lt 4 ]; then
  echo "Usage: $0 <cpp_root> <build_root> <xop_name> <config> [<signing_identity>]"
  echo ""
  echo "Arguments:"
  echo "  cpp_root:          Path to the 'cpp' directory of the source tree."
  echo "  build_root:        Path to the CMake build directory (e.g., 'cpp/build')."
  echo "  xop_name:          The base name of the XOP target (e.g., 'pylabhubxop')."
  echo "  config:            The build configuration (e.g., 'Release', 'Debug')."
  echo "  signing_identity:  (Optional) The identity for code signing. Defaults to '-' for ad-hoc signing."
  echo ""
  echo "Example:"
  echo "  $0 cpp build pylabhubxop Release"
  exit 1
fi

# --- 1. Argument and Path Setup ---
CPP_ROOT="$1"
BUILD_ROOT="$2"
XOP_NAME="$3"
CONFIG="$4"
SIGNING_IDENTITY="${5:--}" # Default to ad-hoc signing

# Derived Names
XOP_BASE_NAME="${XOP_NAME}64" # e.g., pylabhubxop64

# Source Paths
XOP_SRC_DIR="${CPP_ROOT}/src/IgorXOP"
BINARY_PATH="${BUILD_ROOT}/src/IgorXOP/${XOP_BASE_NAME}"
INFO_PLIST_TEMPLATE="${XOP_SRC_DIR}/Info.plist.in"
INFO_PLIST_STRINGS="${XOP_SRC_DIR}/InfoPlist.strings"
REZ_SOURCE="${XOP_SRC_DIR}/WaveAccess.r"
XOPSUPPORT_INCLUDE_PATH="${CPP_ROOT}/third_party/XOPToolkit/XOPSupport/Includes"

# Staging Paths
STAGE_DIR="${BUILD_ROOT}/stage_manual"
BUNDLE_DIR="${STAGE_DIR}/IgorXOP/${XOP_BASE_NAME}.xop"

echo "--- XOP Assembly Configuration ---"
echo "CPP Root:            ${CPP_ROOT}"
echo "Build Root:          ${BUILD_ROOT}"
echo "XOP Name:            ${XOP_NAME}"
echo "Config:              ${CONFIG}"
echo "Signing Identity:    ${SIGNING_IDENTITY}"
echo "Source Binary:       ${BINARY_PATH}"
echo "Destination Bundle:  ${BUNDLE_DIR}"
echo "----------------------------------"

# --- 2. Tool Discovery ---
echo "INFO: Locating required macOS developer tools..."
PLUTIL=$(xcrun -f plutil)
REZ=$(xcrun -f Rez)
CODESIGN=$(xcrun -f codesign)
XATTR=$(xcrun -f xattr)
NM=$(xcrun -f nm)
GREP=$(xcrun -f grep)

for tool in PLUTIL REZ CODESIGN XATTR NM GREP; do
  if [ -z "${!tool}" ]; then
    echo "ERROR: Required tool '${tool_name}' not found. Please ensure Xcode Command Line Tools are installed." >&2
    exit 1
  fi
done
echo "INFO: All tools found."

# --- 3. Bundle Assembly ---
echo ""
echo "--- Step 3: Assembling Bundle ---"

# Clean and create directory structure
echo "INFO: Creating bundle structure at ${BUNDLE_DIR}"
rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_DIR}/Contents/MacOS"
mkdir -p "${BUNDLE_DIR}/Contents/Resources/en.lproj"

# Copy and rename binary
echo "INFO: Copying binary to ${BUNDLE_DIR}/Contents/MacOS/${XOP_BASE_NAME}"
cp "${BINARY_PATH}" "${BUNDLE_DIR}/Contents/MacOS/${XOP_BASE_NAME}"
chmod +x "${BUNDLE_DIR}/Contents/MacOS/${XOP_BASE_NAME}"

# Configure and copy Info.plist
echo "INFO: Configuring and copying Info.plist"
CONFIGURED_INFO_PLIST="${BUNDLE_DIR}/Contents/Info.plist"
sed "s/@XOP_BUNDLE_NAME@/${XOP_BASE_NAME}/g" < "${INFO_PLIST_TEMPLATE}" > "${CONFIGURED_INFO_PLIST}"

# Copy InfoPlist.strings
echo "INFO: Copying InfoPlist.strings"
cp "${INFO_PLIST_STRINGS}" "${BUNDLE_DIR}/Contents/Resources/en.lproj/"

# Compile resources with Rez
echo "INFO: Compiling resources with Rez..."
RSRC_OUTPUT="${BUNDLE_DIR}/Contents/Resources/${XOP_BASE_NAME}.rsrc"
if [ -f "${REZ_SOURCE}" ]; then
    echo "INFO: Rez include path: ${XOPSUPPORT_INCLUDE_PATH}"
    "${REZ}" -I "${XOPSUPPORT_INCLUDE_PATH}" -useDF -o "${RSRC_OUTPUT}" "${REZ_SOURCE}"
    echo "INFO: Created resource file at ${RSRC_OUTPUT}"
else
    echo "WARN: Rez source file not found at ${REZ_SOURCE}. Skipping."
fi
echo "--- Assembly Complete ---"


# --- 4. Bundle Processing ---
echo ""
echo "--- Step 4: Processing Bundle ---"
# Clean extended attributes
echo "INFO: Cleaning extended attributes..."
"${XATTR}" -cr "${BUNDLE_DIR}"

# Sign the bundle
echo "INFO: Signing bundle with identity: '${SIGNING_IDENTITY}'"
"${CODESIGN}" -f -s "${SIGNING_IDENTITY}" "${BUNDLE_DIR}"
echo "--- Processing Complete ---"


# --- 5. Bundle Verification ---
echo ""
echo "--- Step 5: Verifying Bundle ---"
# Check structure
[ -d "${BUNDLE_DIR}/Contents/MacOS" ] && echo "  - 'Contents/MacOS' directory... OK" || { echo "FAIL: 'Contents/MacOS' missing." >&2; exit 1; }
[ -d "${BUNDLE_DIR}/Contents/Resources" ] && echo "  - 'Contents/Resources' directory... OK" || { echo "FAIL: 'Contents/Resources' missing." >&2; exit 1; }
[ -f "${BUNDLE_DIR}/Contents/Info.plist" ] && echo "  - 'Info.plist' file... OK" || { echo "FAIL: 'Info.plist' missing." >&2; exit 1; }
[ -f "${BUNDLE_DIR}/Contents/MacOS/${XOP_BASE_NAME}" ] && echo "  - Executable file... OK" || { echo "FAIL: Executable missing." >&2; exit 1; }
[ -s "${RSRC_OUTPUT}" ] && echo "  - Resource file (.rsrc)... OK" || { echo "WARN: Resource file is missing or empty." >&2; }


# Check exported symbol
echo "INFO: Checking for exported _XOPMain symbol..."
if "${NM}" -gU "${BUNDLE_DIR}/Contents/MacOS/${XOP_BASE_NAME}" | "${GREP}" -q "_XOPMain"; then
  echo "  - Exported '_XOPMain' symbol... OK"
else
  echo "FAIL: Required symbol '_XOPMain' not found in executable." >&2
  exit 1
fi

# Check Info.plist executable name
echo "INFO: Checking CFBundleExecutable in Info.plist..."
PLUTIL_OUTPUT=$("${PLUTIL}" -p "${BUNDLE_DIR}/Contents/Info.plist" | "${GREP}" "CFBundleExecutable")
if [[ "${PLUTIL_OUTPUT}" == *"${XOP_BASE_NAME}"* ]]; then
  echo "  - Info.plist CFBundleExecutable... OK"
else
  echo "FAIL: CFBundleExecutable in Info.plist does not match '${XOP_BASE_NAME}'. Found: ${PLUTIL_OUTPUT}" >&2
  exit 1
fi
echo "--- Verification Complete ---"

echo ""
echo "SUCCESS: XOP bundle created and verified at:"
echo "${BUNDLE_DIR}"
exit 0