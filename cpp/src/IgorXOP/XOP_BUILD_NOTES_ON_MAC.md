# XOP macOS Bundle Notes
*(pylabhub / IgorXOP build reference)*

This document summarizes the **essential requirements** for correctly building
a loadable `.xop` bundle for Igor Pro on macOS using our CMake build system.

It collects verified knowledge from:
- WaveMetrics XOP Toolkit documentation
- Behavior observed with `pylabhubxop64` bundle builds
- Empirical debugging of the CMake build process

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
If `XOPMain` is missing, Igor cannot call into the bundle.

---

## 6. Compiling and Handling Resources

- **Rez File (`.r` -> `.rsrc`):** The `WaveAccess.r` file is compiled by `Rez` into `pylabhubxop64.rsrc`.
- **Localized Strings:** The static file `InfoPlist.strings` is copied into the bundle.

Both of these steps are handled automatically by the `assemble_xop.cmake` script, which is invoked as part of the main target's `POST_BUILD` command sequence. There is no separate `configure_file` step for `InfoPlist.strings`.

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
| `.xop` loads but `XOPMain` never called | Executable not exported or Info.plist executable name mismatch | check `Exports.exp` and `CFBundleExecutable` |
| Igor fails silently | missing or malformed `.rsrc` or Info.plist | ensure proper bundle layout |
| Linker warning: missing line-end in Exports.exp | no final newline | add trailing `\n` |
| `.r` compilation fails | `rez` not found or include paths missing | use `xcrun rez` and add `-I` paths |
| Finder shows generic folder icon | `CFBundlePackageType` not `IXOP` or `CFBundleSignature` missing | fix Info.plist keys |
| Build fails with compiler errors | `CC`/`CXX` environment variables are interfering | Use `CC= CXX= xcodebuild ...` |

---

## 10. Quick Verification Checklist

After `CC= CXX= xcodebuild ...`:

```bash
# Define the path to your staged bundle
BUNDLE="build_xcode/stage/IgorXOP/pylabhubxop64.xop"

# Confirm exported symbol
nm -gU "${BUNDLE}/Contents/MacOS/pylabhubxop64" | grep XOPMain

# Confirm bundle layout
tree "${BUNDLE}"

# Confirm Info.plist and localized strings
plutil -p "${BUNDLE}/Contents/Info.plist" | grep CFBundleExecutable
cat "${BUNDLE}/Contents/Resources/en.lproj/InfoPlist.strings"
```

All checks should pass before testing inside Igor.

---

## 11. Build Enhancements

- **Code Signing:** Ad-hoc signing is now implemented automatically as a post-build step to allow local execution. For distribution on newer macOS, this will need to be extended with a proper Developer ID and notarization.
- **`xcrun rez` Path:** The build relies on `Rez` being in the PATH. A future improvement could be to automatically detect it from `xcrun`.
- **Localization:** `InfoPlist.strings` could be extended to support multiple languages.

---

**Revision:** 2025-12-05
**Maintainers:** pylabhub XOP build team
**Purpose:** To document the modern CMake-based build process for the macOS XOP.
