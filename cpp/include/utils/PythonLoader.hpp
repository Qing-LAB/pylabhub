/*******************************************************************************
 * @file PythonLoader.hpp
 * @brief C-style API for managing an embedded Python interpreter.
 *
 * **Design Philosophy**
 *
 * This header provides a C-style, ABI-stable interface for loading, initializing,
 * and interacting with an embedded Python interpreter from a native application,
 * specifically an Igor Pro XOP (External Operation). The C-style `extern "C"`
 * linkage is essential for creating a stable interface that can be reliably
 * called from the XOP framework and other C-based environments.
 *
 * 1.  **Dynamic Loading**: The loader does not link against a specific Python
 *     library at compile time. Instead, it dynamically loads the Python shared
 *     library (`python3.x.dll` or `libpython3.x.so`) at runtime from a path
 *     provided by the user. This allows the XOP to work with various user-installed
 *     Python versions without needing to be recompiled.
 *
 * 2.  **Configuration Persistence**: The path to the Python installation and other
 *     settings (like a cleanup callable) are persisted to a JSON configuration
 *     file. This allows the settings to be remembered across XOP loads and Igor
 *     Pro sessions.
 *
 * 3.  **State Management**: The API provides explicit functions for managing the
 *     interpreter's state:
 *     - `PyLoader_init()`: Initializes the loader's internal state.
 *     - `PySetPython()`: Configures the path to the Python interpreter.
 *     - `PyReInit()`: Loads and initializes the Python interpreter in-process.
 *     - `PyExec()`: Executes Python code.
 *     - `PyCleanup()`: Shuts down the interpreter.
 *
 * 4.  **Igor Pro Integration**:
 *     - The parameter structures (`Py...Params`) are designed to be compatible
 *       with Igor Pro's function-style XOP interface, where parameters are passed
 *       in a packed struct. The `#pragma pack(2)` directive ensures compatibility
 *       with Igor's memory alignment.
 *     - `PostHistory()` is a utility function for sending output back to the
 *       Igor Pro history log.
 *
 * **Usage (within an Igor Pro XOP)**
 *
 * An XOP would typically call these functions in response to user commands or
 * during its own lifecycle events (`XOPMain` with `INIT` or `CLEANUP` messages).
 *
 * ```c
 * // Example of handling an XOP function call to execute Python code.
 *
 * // Parameter structure for our XOP function.
 * typedef struct MyPyExecParams {
 *      char anIgorStringVar[MAX_VAR_NAME_LEN + 1];
 *      // ... other params ...
 *      double result;
 * } MyPyExecParams;
 *
 * // The function called by Igor.
 * extern "C" int MyPyExec(MyPyExecParams* p) {
 *     // Check if the interpreter is ready.
 *     if (!pylabhub::utils::PyIsInitialized()) {
 *         pylabhub::utils::PostHistory("Error: Python is not initialized.\r");
 *         return -1; // Return an Igor error code.
 *     }
 *
 *     // Execute the code contained in the Igor Pro string variable.
 *     return pylabhub::utils::PyExec(p->anIgorStringVar);
 * }
 * ```
 ******************************************************************************/
#pragma once

#include <string>

// These constants are likely defined in an Igor Pro-specific header.
// Forward-declare them here for context if that header is not included.
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef MAX_OBJ_NAME
#define MAX_OBJ_NAME 31
#endif

// This attribute is likely used to mark functions for export from the XOP.
#ifndef HOST_IMPORT
#define HOST_IMPORT
#endif

namespace pylabhub::utils
{

// The pack(2) pragma is critical for ensuring that the layout of these structs
// matches the memory alignment expected by Igor Pro when calling XOP functions.
#pragma pack(2)

/** @brief Parameter block for an XOP function to set the Python path. */
typedef struct
{
    char path[MAX_PATH]; ///< [in] A C-string containing the UTF-8 path to the Python installation
                         ///< or virtual environment.
    double result; ///< [out] Igor Pro requires a 'result' member for the function return value.
} PySetPythonParams;

/** @brief Parameter block for an XOP function to set the Python cleanup script. */
typedef struct
{
    char callable[MAX_OBJ_NAME + 1]; ///< [in] A C-string in "module:function" format.
    double result;                   ///< [out] The function return value.
} PySetCleanupParams;

/** @brief Parameter block for an XOP function to re-initialize the interpreter. */
typedef struct
{
    char path[MAX_OBJ_NAME + 1]; ///< [in] An optional path, or empty to use the saved path.
    double result;               ///< [out] The function return value.
} PyReInitParams;

/** @brief Parameter block for an XOP function to check the interpreter status. */
typedef struct
{
    double result; ///< [out] The function return value (1 if initialized, 0 otherwise).
} PyStatusParams;

/** @brief Parameter block for an XOP function to execute a Python script. */
typedef struct
{
    char varName[MAX_OBJ_NAME +
                 1]; ///< [in] The name of an Igor Pro string variable containing the script to run.
    double result;   ///< [out] The function return value.
} PyExecParams;

/** @brief Parameter block for an XOP function to clean up the interpreter. */
typedef struct
{
    double result; ///< [out] The function return value.
} PyCleanupParams;

#pragma pack()

/**
 * @brief Posts a message string to the Igor Pro history log.
 * @param s The message to post.
 */
void PostHistory(const std::string &s);

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initializes the Python loader's internal state.
     * This should be called once when the XOP is loaded (e.g., in `XOPMain`).
     * It is safe to call even if Python is not yet configured.
     * @return 0 on success.
     */
    HOST_IMPORT int PyLoader_init(void);

    /**
     * @brief Cleans up the Python loader's resources.
     * If the interpreter is running, it will be shut down. Should be called
     * when the XOP is unloaded.
     * @return 0 on success.
     */
    HOST_IMPORT int PyLoader_cleanup(void);

    /**
     * @brief Sets the path to the Python installation or virtual environment and
     * saves it to the configuration file.
     * @note This only stores the path; `PyReInit` must be called to load and
     *       initialize the interpreter from this path.
     * @param pathUtf8 A UTF-8 encoded string containing the path.
     * @return 0 on success, or an Igor Pro error code on failure.
     */
    HOST_IMPORT int PySetPython(const char *pathUtf8);

    /**
     * @brief Sets a Python function to be called automatically before the
     * interpreter is finalized.
     * @param callableUtf8 A string specifying the function in "module:function"
     *                     or "module.submodule:function" format.
     * @return 0 on success.
     */
    HOST_IMPORT int PySetCleanupCallable(const char *callableUtf8);

    /**
     * @brief Loads and initializes the Python interpreter in-process.
     * It will refuse to run if the interpreter is already initialized.
     * @param pathUtf8 The path to the Python installation. If NULL or empty,
     *                 the loader will use the path previously saved in the
     *                 configuration file.
     * @return 0 on success, or a non-zero Igor Pro error code on failure.
     */
    HOST_IMPORT int PyReInit(const char *pathUtf8);

    /**
     * @brief Checks if the embedded Python interpreter is currently initialized.
     * @return 1 if initialized, 0 otherwise.
     */
    HOST_IMPORT int PyIsInitialized(void);

    /**
     * @brief Executes a block of Python code.
     * @param igorStringVarName The name of an Igor Pro global string variable
     *                          that contains the Python code to be executed.
     * @return 0 on success. Any Python exceptions will be caught and reported
     *         to the Igor Pro history log.
     */
    HOST_IMPORT int PyExec(const char *igorStringVarName);

    /**
     * @brief Explicitly finalizes the Python interpreter.
     * If a cleanup callable was set, it is run first. This unloads the Python
     * shared library, allowing for a different version to be loaded via `PyReInit`.
     * @return 0 on success.
     */
    HOST_IMPORT int PyCleanup(void);

#ifdef __cplusplus
}
#endif

} // namespace pylabhub::utils