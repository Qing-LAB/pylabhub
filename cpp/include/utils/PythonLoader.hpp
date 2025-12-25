// PythonLoader.h
#pragma once
#include <string>

namespace pylabhub::utils
{
#pragma pack(2)

// Runtime parameter structures for function-style interface
typedef struct
{
    char path[MAX_PATH];
    double result;
} PySetPythonParams;

typedef struct
{
    char callable[MAX_OBJ_NAME + 1];
    double result;
} PySetCleanupParams;

typedef struct
{
    char path[MAX_OBJ_NAME + 1];
    double result;
} PyReInitParams;

typedef struct
{
    double result;
} PyStatusParams;

typedef struct
{
    char varName[MAX_OBJ_NAME + 1];
    double result;
} PyExecParams;

typedef struct
{
    double result;
} PyCleanupParams;

#pragma pack()

void PostHistory(const std::string &s);

#ifdef __cplusplus
extern "C"
{
#endif

    // Initialize loader (called from XOPMain). Non-fatal if no python configured yet.
    HOST_IMPORT int PyLoader_init(void);

    // Cleanup / finalize python (call on CLEANUP / XOP unload)
    HOST_IMPORT int PyLoader_cleanup(void);

    // Set python/venv path and persist to JSON (path is UTF-8 string).
    // This only stores path; call PyReInit() to actually initialize.
    // Returns 0 on success, WMparamErr (or other Igor error) on failure.
    HOST_IMPORT int PySetPython(const char *pathUtf8);

    // Set a cleanup callable to run before finalization. Format: "module:function" or
    // "module.sub:func". Persisted to JSON. Returns 0 on success.
    HOST_IMPORT int PySetCleanupCallable(const char *callableUtf8);

    // Initialize Python in-process (dynamic loading + env/bootstrap emulation of PyConfig).
    // Refuses to initialize if interpreter already initialized (safety).
    // If pathUtf8 is NULL, will attempt to use path from persisted JSON.
    // Returns 0 on success, non-zero Igor error code on failure.
    HOST_IMPORT int PyReInit(const char *pathUtf8);

    // Returns 1 if python interpreter is initialized, 0 otherwise.
    HOST_IMPORT int PyIsInitialized(void);

    // Execute code stored in an Igor string variable named varName (C string).
    // Runs the Python code in-process. Returns 0 on success.
    // Any textual output is posted to Igor History. For larger capture you can extend this.
    HOST_IMPORT int PyExec(const char *igorStringVarName);

    // Explicitly run cleanup callable (if set) and finalize/unload interpreter.
    // Use this to unload Python before calling PyReInit again or on XOP unload.
    HOST_IMPORT int PyCleanup(void);

#ifdef __cplusplus
}
#endif

} // namespace pylabhub::utils