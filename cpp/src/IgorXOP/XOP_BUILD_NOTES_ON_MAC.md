# XOP macOS Bundle Notes
*(pylabhub / IgorXOP build reference)*

This document summarizes the **essential requirements** for correctly building
a loadable `.xop` bundle for Igor Pro on macOS using CMake/Xcode.

It collects verified knowledge from:
- WaveMetrics XOP Toolkit documentation  
- Behavior observed with `pylabhub-xop64` bundle builds  
- Empirical debugging and resource layout validation  

---

## 1. Bundle Structure

A correct `.xop` is a **macOS bundle directory** with the following minimal layout:

```
pylabhub-xop64.xop/
└── Contents/
    ├── Info.plist
    ├── MacOS/
    │   └── pylabhub-xop64              # Mach-O executable (exporting XOPMain)
    └── Resources/
        ├── pylabhub-xop64.rare         # compiled resource archive (.r → .rare)
        └── en.lproj/
            └── InfoPlist.strings       # localized strings for Info.plist
```

All paths and names are **case-sensitive** and must match those
expected by Igor’s XOP loader.

---

## 2. Critical Naming Consistency

| Component | Required Value | Notes |
|------------|----------------|-------|
| **Bundle folder** | `<XOP_NAME>64.xop` | `.xop` suffix required |
| **Executable inside `Contents/MacOS`** | `<XOP_NAME>64` | must match `CFBundleExecutable` |
| **Info.plist → `CFBundleExecutable`** | same as executable | Igor uses this key to locate entry binary |
| **CFBundlePackageType** | `IXOP` | identifies the bundle as an Igor XOP |
| **CFBundleSignature** | `IGR0` | creator code recognized by Igor |
| **CFBundleIdentifier** | reverse-domain (e.g. `com.pylabhub.xop.pylabhub-xop64`) | optional but recommended |

Mismatched names between `Info.plist` and the actual binary prevent Igor
from finding or launching the bundle’s executable.

---

## 3. Info.plist Template (CMake)

Minimal working template:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>
  <string>@EXECUTABLE_NAME@</string>
  <key>CFBundleExecutable</key>
  <string>@EXECUTABLE_NAME@</string>
  <key>CFBundleIdentifier</key>
  <string>com.pylabhub.xop.@EXECUTABLE_NAME@</string>
  <key>CFBundlePackageType</key>
  <string>IXOP</string>
  <key>CFBundleSignature</key>
  <string>IGR0</string>
  <key>CFBundleVersion</key>
  <string>1.0</string>
</dict>
</plist>
```

CMake generates this with:
```cmake
configure_file(Info64.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(${XOP_TARGET} PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
)
```

---

## 4. Exported Symbol Requirements

Igor loads the Mach-O and looks for the `XOPMain` entry point.

- **Exported symbol name:** `_XOPMain` (on macOS, symbols are prefixed with `_`)
- **Required export file:** `Exports.exp`  
  ```text
  _XOPMain
  ```
- **CMake linkage flag:**
  ```cmake
  target_link_options(${XOP_TARGET} PRIVATE
    -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp)
  ```

Validate with:
```bash
nm -gU Contents/MacOS/pylabhub-xop64 | grep XOPMain
```
If `XOPMain` is missing, Igor cannot call into the bundle.

---

## 5. Compiling the Resource File (.r → .rare)

The `.r` resource (Rez source) must be compiled into a `.rare`
and placed in `Contents/Resources/`.

Typical command:
```bash
xcrun rez WaveAccess.r -o pylabhub-xop64.rare
```

CMake automation example:

```cmake
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${XOP_NAME}64.rare
  COMMAND xcrun rez ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r
          -o ${CMAKE_CURRENT_BINARY_DIR}/${XOP_NAME}64.rare
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r
  COMMENT "Building .rare resource file"
)
```

---

## 6. Localized Strings (`en.lproj/InfoPlist.strings`)

Each bundle should include a localized copy of `InfoPlist.strings`
so Finder and Igor show the correct name.

Minimal template:

```text
CFBundleName = "@EXECUTABLE_NAME@";
```

CMake can generate this using:

```cmake
configure_file(InfoPlist.strings.in ${CMAKE_CURRENT_BINARY_DIR}/en.lproj/InfoPlist.strings @ONLY)
```

---

## 7. Framework and Linker Settings

These frameworks are required by `libXOPSupport64.a`:

```cmake
target_link_libraries(${XOP_TARGET} PRIVATE
  "-framework Cocoa"
  "-framework CoreFoundation"
  "-framework CoreServices"
  "-framework Carbon"
  "-framework AudioToolbox"
)
```

Recommended linker flags:
```cmake
target_link_options(${XOP_TARGET} PRIVATE -Wl,-dead_strip)
```

---

## 8. Compiler Defines

Per `XOPStandardHeaders.h`, the following must be defined:

```cmake
target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
```

---

## 9. Common Pitfalls (and Solutions)

| Symptom | Likely Cause | Fix |
|----------|---------------|-----|
| `.xop` loads but `XOPMain` never called | Executable not exported or Info.plist executable name mismatch | check `Exports.exp` and `CFBundleExecutable` |
| Igor fails silently | missing or malformed `.rare` or Info.plist | ensure proper bundle layout |
| linker warning: missing line-end in Exports.exp | no final newline | add trailing `\\n` |
| `.r` compilation fails | `rez` not found or include paths missing | use `xcrun rez` and add `-I` paths |
| Finder shows generic folder icon | `CFBundlePackageType` not `IXOP` or `CFBundleSignature` missing | fix Info.plist keys |

---

## 10. Quick Verification Checklist

After `cmake --build ... --target pylabhubxop`:

```bash
# Confirm exported symbol
nm -gU Contents/MacOS/pylabhub-xop64 | grep XOPMain

# Confirm bundle layout
tree pylabhub-xop64.xop

# Confirm Info.plist and localized strings
plutil -p Contents/Info.plist | grep CFBundleExecutable
cat Contents/Resources/en.lproj/InfoPlist.strings
```

All checks should pass before testing inside Igor.

---

## 11. Future Enhancements

- Add code-signing / notarization steps for distribution on newer macOS versions.  
- Automatically detect and use `xcrun rez` path from `CMAKE_OSX_SYSROOT`.  
- Extend `InfoPlist.strings` localization to multiple languages.

---

**Revision:** 2025-11-15  
**Maintainers:** pylabhub XOP build team  
**Purpo