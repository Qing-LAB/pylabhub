# Lifecycle Dynamic Module Extensions — User Data Support

**Status**: Implementing (2026-03-31)
**Scope**: Add userdata to lifecycle callbacks for safe pointer passing

---

## 1. Problem

Lifecycle callbacks are `void(*)(const char *arg)`. To access the owning object
(e.g., a ScriptEngine), code needs static globals. This is fragile and unsafe
for multi-instance scenarios.

## 2. Solution

### 2.1 Callback signature change

```cpp
// Before:
using LifecycleCallback = void (*)(const char *arg);

// After:
using LifecycleCallback = void (*)(const char *arg, void *userdata);
```

Existing callbacks add `void * /*userdata*/` and ignore it. Lifecycle passes
nullptr when no userdata is configured (default).

### 2.2 ModuleDef constructor overload

```cpp
// Existing (unchanged):
explicit ModuleDef(std::string_view name);

// New overload — userdata set once at construction, immutable:
using UserDataValidateFn = bool (*)(void *userdata, uint64_t key);
ModuleDef(std::string_view name, void *userdata, UserDataValidateFn validate);

// Getter (read key before moving ModuleDef into lifecycle):
uint64_t userdata_key() const noexcept;
```

### 2.3 Key generation

`LifecycleManager::next_unique_key()` — static method on the singleton.
Returns from `m_next_userdata_key.fetch_add(1)` atomic counter on
LifecycleManagerImpl. Called by the ModuleDef constructor.

### 2.4 Data stored on module node

```
InternalModuleDef / InternalGraphNode:
    void *userdata{nullptr};
    uint64_t userdata_key{0};
    UserDataValidateFn userdata_validate{nullptr};
```

### 2.5 Lambda wrapping

Userdata captured at `set_startup`/`set_shutdown` time (userdata already
set at construction):

```cpp
void ModuleDef::set_startup(LifecycleCallback cb) {
    void *ud = pImpl->def.userdata;
    pImpl->def.startup = [cb, ud]() { cb(nullptr, ud); };
}
```

### 2.6 Validation before callbacks

```
loadModuleInternal / processOneUnloadInThread:
    if (userdata_key != 0 && userdata_validate):
        if (!userdata_validate(userdata, userdata_key)):
            → skip callback, mark FAILED / contaminated
    // proceed with callback (userdata baked into lambda)
```

### 2.7 Validation function (user provides)

```cpp
bool validate(void *ptr, uint64_t key) {
    auto *obj = static_cast<MyClass*>(ptr);
    return obj->magic_ == kExpectedMagic && obj->lifecycle_key_ == key;
}
```

Magic number catches recycled memory. Generation key catches stale objects.

## 3. Implementation Status

### Done:
- LifecycleCallback signature: `void(*)(const char*, void*)` ✓
- ModuleDef constructor overload with userdata ✓
- UserDataValidateFn type ✓
- userdata_key() getter ✓
- next_unique_key() on LifecycleManager ✓
- InternalModuleDef + InternalGraphNode new fields ✓
- Lambda wrapping captures userdata ✓
- Field propagation in registerDynamicModule ✓
- Validation in loadModuleInternal ✓
- Validation in processOneUnloadInThread ✓
- All 22 existing callbacks updated across 13 files ✓

### Pending:
- Clean build verification
- Test run
- Commit

## 4. Files Modified

| File | Change |
|------|--------|
| `module_def.hpp` | Callback signature, constructor, ValidateFn, userdata_key() |
| `lifecycle_impl.hpp` | 3 fields on structs, atomic counter, nextUserdataKey() |
| `lifecycle.hpp` | next_unique_key() static method |
| `lifecycle.cpp` | Constructor, lambda wrapping, field propagation, next_unique_key() |
| `lifecycle_dynamic.cpp` | Validation in startup + shutdown dispatch |
| 13 callback files | Add `void * /*userdata*/` to 22 functions |
