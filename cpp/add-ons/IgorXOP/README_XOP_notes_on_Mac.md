# XOP macOS Bundle Notes
*(pylabhub / IgorXOP build reference)*

This document summarizes the **essential requirements** for correctly building
a loadable `.xop` bundle for Igor Pro on macOS using our CMake build system.

It collects verified knowledge from:
- WaveMetrics XOP Toolkit documentation
- Behavior observed with `pylabhubxop64` bundle builds
- Empirical debugging of the CMake build process
The build system has evolved to include automated assembly, verification, and code signing, encapsulating the lessons learned below.

---

## 1. Building with CMake (Xcode Generator)

The recommended way to build the XOP on macOS is to use CMake to generate an Xcode project.

**IMPORTANT**: This process is sensitive to your shell environment. If you have other compilers installed (e.g., via Homebrew), you must ensure they do not interfere with Xcode's own toolchain.

#### Step 1: Clean and Generate the Xcode Project

From the `cpp` directory, create a clean build directory and run CMake with the `Xcode` generator.

```bash
rm -rf build_xcode
mkdir build_xcode
cmake -S . -B build_xcode -G Xcode
```

Our build script is designed to automatically handle toolchain detection when using the Xcode generator.

#### Step 2: Build the Project

Build the project using `xcodebuild`. The `CC=` and `CXX=` prefixes are critical to prevent your shell environment from overriding the Xcode project's compiler settings.

```bash
CC= CXX= xcodebuild -project build_xcode/pylabhub-cpp.xcodeproj
```

#### Step 3: Find the Staged Artifacts

After a successful build, all artifacts, including the complete and signed `.xop` bundle, will be placed in the staging directory:

`build_xcode/stage/IgorXOP/`

---

## 2. Bundle Structure

A correct `.xop` is a **macOS bundle directory** with the following minimal layout. Our CMake script assembles this structure automatically.

```
pylabhubxop64.xop/
└── Contents/
    ├── _CodeSignature/
    │   └── CodeResources             # Created by codesign
    ├── Info.plist
    ├── MacOS/
    │   └── pylabhubxop64              # Mach-O executable (exporting XOPMain)
    └── Resources/
        ├── pylabhubxop64.rsrc         # compiled resource archive (.r -> .rsrc)
        └── en.lproj/
            └── InfoPlist.strings       # localized strings for Info.plist
```

All paths and names are **case-sensitive** and must match those expected by Igor’s XOP loader.

---

## 3. Critical Naming Consistency

| Component | Required Value | Notes |
|------------|----------------|-------|
| **Bundle folder** | `<XOP_NAME>64.xop` | `.xop` suffix required |
| **Executable inside `Contents/MacOS`** | `<XOP_NAME>64` | must match `CFBundleExecutable` |
| **Info.plist → `CFBundleExecutable`** | same as executable | Igor uses this key to locate entry binary |
| **CFBundlePackageType** | `IXOP` | identifies the bundle as an Igor XOP |
| **CFBundleSignature** | `IGR0` | creator code recognized by Igor |
| **CFBundleIdentifier** | reverse-domain (e.g. `com.pylabhub.xop.pylabhubxop64`) | optional but recommended |

Mismatched names between `Info.plist` and the actual binary prevent Igor
from finding or launching the bundle’s executable.

---

## 4. Info.plist Template (CMake)

The build uses `Info.plist.in` as a template. The `@XOP_BUNDLE_NAME@` variable is substituted by CMake.

Minimal working template (`Info.plist.in`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>
  <string>@XOP_BUNDLE_NAME@</string>
  <key>CFBundleExecutable</key>
  <string>@XOP_BUNDLE_NAME@</string>
  <key>CFBundleIdentifier</key>
  <string>com.pylabhub.xop.@XOP_BUNDLE_NAME@</string>
  <key>CFBundlePackageType</key>
  <string>IXOP</string>
  <key>CFBundleSignature</key>
  <string>IGR0</string>
  <key>CFBundleVersion</key>
  <string>1.0</string>
</dict>
</plist>
```

CMake generates the final `Info.plist` with this call in `IgorXOP/CMakeLists.txt`:
```cmake
set(INFO_PLIST_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
set(CONFIGURED_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
configure_file("${INFO_PLIST_TEMPLATE}" "${CONFIGURED_INFO_PLIST}" @ONLY)
```
The configured file is then copied into the bundle by a post-build script.

---

## 5. Exported Symbol Requirements

Igor loads the Mach-O and looks for the `XOPMain` entry point.

- **Exported symbol name:** `_XOPMain` (on macOS, symbols are prefixed with `_`)
- **Required export file:** `Exports.exp`
- **CMake linkage flag:**
  ```cmake
  target_link_options(${XOP_TARGET} PRIVATE
    -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp)
  ```

Validate with:
```bash
nm -gU build_xcode/stage/IgorXOP/pylabhubxop64.xop/Contents/MacOS/pylabhubxop64 | grep XOPMain
```
If `XOPMain` is missing, Igor cannot call into the bundle. Our build system now automates this check.

---

## 6. Compiling and Handling Resources

Resource compilation is a critical and historically fragile part of building XOPs. Our build system fully automates this.

- **Rez Compilation (`.r` -> `.rsrc`):**
  The `.r` file, which defines the XOP's interface with Igor, is compiled by Apple's `Rez` tool into a binary `pylabhubxop64.rsrc` file. This is handled by the `assemble_xop.cmake` script.

  **Key Implementation Detail:** The script invokes `Rez` with the `-useDF` flag. This is essential on modern macOS to ensure the compiled resources are written to the file's "data fork." Without it, `Rez` defaults to the legacy "resource fork," which is often stripped during file copy operations (e.g., by CMake or `git`), resulting in an empty `.rsrc` file and a "logical end-of-file" error in Igor.

- **Localized Strings:** The static `InfoPlist.strings` file is copied into `Contents/Resources/en.lproj/`. If the source file is missing, the script generates a minimal placeholder to ensure the bundle is valid.

Both steps are part of a `POST_BUILD` command sequence.

---

## 7. Framework and Linker Settings

These frameworks are required by `libXOPSupport64.a` and are linked automatically by the build script:

```cmake
target_link_libraries(${XOP_TARGET} PRIVATE
  "-framework Cocoa"
  "-framework CoreFoundation"
  "-framework CoreServices"
  "-framework Carbon"
  "-framework AudioToolbox"
)
```

---

## 8. Compiler Defines

Per `XOPStandardHeaders.h`, the following are defined automatically:

```cmake
target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
```

---

## 9. Common Pitfalls (and Solutions)

| Symptom | Likely Cause | Fix |
|----------|---------------|-----|
| Igor shows "logical end-of-file during load" error | The `.rsrc` file inside the bundle is empty or corrupted. This happens if `Rez` wrote to the legacy resource fork, which was then stripped during a file copy. | The build script must use `Rez` with the `-useDF` flag. Our CMake build does this automatically. If building manually, ensure you add this flag. |
| `.xop` loads but `XOPMain` never called | Executable not exported or Info.plist executable name mismatch | Check `Exports.exp` and `CFBundleExecutable`. This is now verified automatically by the build. |
| Igor fails silently | Missing or malformed `.rsrc` or `Info.plist` | Ensure proper bundle layout. This is now verified automatically by the build. |
| Linker warning: missing line-end in Exports.exp | No final newline in the `.exp` file. | Add a trailing newline (`\n`) to the file. |
| `.r` compilation fails (`rez` error) | `rez` not found or include paths missing | Ensure Xcode Command Line Tools are installed (`xcode-select --install`). The build script automatically finds `rez` and adds necessary `-I` include paths. |
| Finder shows generic folder icon | `CFBundlePackageType` not `IXOP` or `CFBundleSignature` missing | Fix `Info.plist` keys. |
| Build fails with compiler errors | `CC`/`CXX` environment variables are interfering | Use `CC= CXX= xcodebuild ...` |

---

## 10. Automated and Manual Verification

Our CMake build includes a robust, automated verification script (`VerifyXOP.cmake`) that runs as a `POST_BUILD` step. It checks for the most common structural and configuration errors, causing the build to fail if any are found.

**Automated Checks:**
- Correct bundle directory structure (`Contents/MacOS`, `Contents/Resources`).
- Presence of the `Info.plist` file.
- Presence of the main executable.
- The `_XOPMain` symbol is correctly exported in the executable (via `nm`).
- `CFBundleExecutable` in `Info.plist` matches the executable's filename (via `plutil`).

**Manual Verification Checklist:**

If you need to debug a bundle or verify one from an external source, the following manual checks (which mirror the automated process) are recommended.

```bash
# Define the path to your staged bundle
BUNDLE="build_xcode/stage/IgorXOP/pylabhubxop64.xop"

# 1. Confirm bundle layout
tree "${BUNDLE}"

# 2. Confirm exported symbol
nm -gU "${BUNDLE}/Contents/MacOS/pylabhubxop64" | grep XOPMain

# 3. Confirm Info.plist executable name
plutil -p "${BUNDLE}/Contents/Info.plist" | grep CFBundleExecutable

# 4. Check for a non-empty resource file
ls -l "${BUNDLE}/Contents/Resources/pylabhubxop64.rsrc"
```

A successful build from our CMake system implies all these checks have passed.

---

## 11. Build Process Details and Enhancements

- **Consolidated Assembly Script:** The XOP bundle assembly is now handled by a single, consolidated CMake script (`consolidated_xop_assembly.cmake.in`). This script is executed as a `POST_BUILD` command on the XOP target and performs all necessary steps: creating the bundle structure, compiling resources, copying dependencies, cleaning attributes, code signing, and verification. This simplifies the build logic and removes the need for multiple intermediary scripts.

- **Dependency Bundling**: On macOS, the dynamic library `pylabhub-utils.dylib` is now automatically copied into the XOP bundle's `Contents/MacOS` directory during assembly. This crucial step ensures that the XOP can find its necessary runtime dependencies, resolving "module not found" errors when loading the XOP in Igor Pro.

- **Code Signing:** The build automatically performs ad-hoc signing via the `CodeSign.cmake` post-build script. This is required for local execution on Apple Silicon Macs. Signing is triggered if the CMake variable `MACOSX_CODESIGN_IDENTITY` is set. For ad-hoc signing, set it to `-`. For distribution, this would be set to a proper Developer ID. The script prioritizes the system's `codesign` tool at `/usr/bin`.

- **Localization:** `InfoPlist.strings` is currently hardcoded for English (`en.lproj`). The build could be extended to support multiple localizations by adding other `.lproj` directories.

---

**Revision:** 2025-12-05
**Maintainers:** pylabhub XOP build team
**Purpose:** To document the modern CMake-based build process for the macOS XOP.
