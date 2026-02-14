# HEP-CORE-0005: Script Interface Abstraction Framework

| Property       | Value                                      |
| -------------- | ------------------------------------------ |
| **HEP**        | `HEP-CORE-0005`                            |
| **Title**      | Script Interface Abstraction Framework     |
| **Author**     | Gemini CLI Agent                           |
| **Status**     | Draft                                      |
| **Created**    | 2026-02-01                                 |

## Abstract

This Hub Enhancement Proposal (HEP) outlines the design for a **Script Interface Abstraction Framework**. This framework aims to provide a flexible and extensible mechanism for integrating user-defined real-time processing logic directly into core hub objects, such as the Message Hub and DataBlock Hub. By abstracting the scripting engine, the system can support multiple languages (starting with LuaJIT, with future potential for Python) while maintaining a consistent C++ API for script interaction, execution, and data exchange. This design emphasizes modularity, performance, and clear boundaries between C++ and script environments, addressing critical concerns like lifecycle management, access control, and concurrency.

## Motivation

As the pyLabHub system evolves, there is a growing need for dynamic, user-configurable processing logic that can react to incoming messages or data in real-time without requiring recompilation of the core C++ application. This is particularly crucial for:

1.  **User-Defined Logic**: Empowering users to define custom filtering, transformation, or routing rules for data directly within the hub.
2.  **Real-time Reaction**: Enabling low-latency responses to events within the Message Hub or new data in a DataBlock.
3.  **Flexibility & Experimentation**: Allowing rapid prototyping and iteration of processing logic without disrupting the stable C++ codebase.
4.  **Extensibility**: Providing a clear path to integrate various scripting languages (e.g., Lua for performance, Python for rich libraries) based on specific latency or functional requirements.
5.  **Simplified Control**: Offering a high-level control surface for complex C++ internals, reducing the need for extensive C++ development for certain tasks.

The Script Interface Abstraction Framework addresses these needs by establishing a robust, engine-agnostic API for embedding and interacting with scripting environments.

## Goals

*   **Engine Agnosticism**: Provide a generic C++ interface (`IScriptEngine`) that decouples client code from specific scripting language implementations.
*   **Extensibility**: Ensure that new scripting languages can be integrated with minimal impact on existing code.
*   **Performance**: Allow for efficient execution of scripts, especially with LuaJIT for latency-critical tasks.
*   **Safety & Isolation**: Define clear boundaries for script execution, minimizing potential for C++ application crashes due to script errors. Implement robust access control.
*   **Bidirectional Communication**: Enable both C++ to script function calls and script-initiated calls back into exposed C++ functionality (e.g., to send messages, read/write DataBlocks).
*   **Resource Management**: Facilitate proper lifecycle management of script engine instances and associated resources.
*   **Error Handling**: Provide mechanisms to catch and report script-level errors gracefully to the C++ application.

## Design Principles

1.  **Interface Segregation**: A lean `IScriptEngine` interface defines only essential script interaction, allowing specific engine implementations to expose additional, language-specific features if necessary (e.g., LuaJIT FFI).
2.  **Explicit Context**: Script execution will occur within an explicit context. Hub objects will pass necessary C++ "handles" or interfaces to the script, rather than exposing global C++ state.
3.  **Data-Oriented Exchange**: Prioritize efficient data transfer. For simple values, direct C++ <-> script type conversions. For complex data structures (especially DataBlocks), consider shared memory access, MessagePack serialization, or light-weight proxy objects.
4.  **Error Propagation**: Script errors should be caught within the C++ layer and transformed into C++ exceptions or status codes, preventing application termination.
5.  **Lifecycle Management & Ownership**: Each hub object (or component requiring scripting) will own its dedicated `IScriptEngine` instance via `std::unique_ptr`. This ensures the script engine's lifetime is strictly tied to its owner. The `IScriptEngine` will hold a `std::shared_ptr<IScriptContext>` to its owner, preventing premature destruction of the context while scripts are active.
6.  **Concurrency Model**: This framework unifies scripting systems under a common `IScriptEngine` interface. However, the underlying concurrency characteristics differ significantly and must be understood by the user.
    *   **Per-instance Thread Safety**: Each `IScriptEngine` instance is designed to be accessed by at most one C++ thread at a time. If a C++ component needs to perform parallel processing, it should instantiate and manage its own independent `IScriptEngine` instance for each parallel execution unit.
    *   **Shared Resources**: All C++ shared resources accessed via `IScriptContext` (e.g., global loggers, MessageHub instances) *must* be internally thread-safe within the C++ host (e.g., using mutexes or atomic operations).
    *   **Language-Specific Concurrency Implications**:
        *   **LuaJIT**: Each `lua_State*` (corresponding to an `IScriptEngine` instance) is independent and has no Global Interpreter Lock (GIL). Therefore, multiple `LuaJITScriptEngine` instances can run concurrently in different C++ threads without inherent serialization by the language runtime itself.
        *   **Python (CPython)**: The Global Interpreter Lock (GIL) is a fundamental constraint. A C++ process embedding CPython can, in practice, only have one primary Python interpreter. All Python code executed within this interpreter, regardless of which `PythonScriptEngine` object initiates it, will be subject to the GIL, effectively serializing Python execution. Even if multiple `PythonScriptEngine` instances exist in separate C++ threads, their Python bytecode execution will be bottlenecked by the single GIL. True parallelism for pure Python code requires multi-process approaches.
7.  **Access Control Layers**: A multi-layered approach to security and authorization will be employed:
    *   **Engine-Level Sandboxing**: Disable dangerous script functions (e.g., file I/O, process execution) by default within the script engine initialization.
    *   **Configuration-Based Permissions**: Utilize a centralized, privileged-controlled configuration (e.g., `JsonConfig`) to specify allowed script locations or specific C++ functions that scripts are authorized to call.
    *   **Contextual Authorization**: The `IScriptContext` implementation can apply fine-grained checks on operations requested by the script (e.g., a script might be allowed to log but not send a message).
8.  **Consistent Inter-Process Communication Protocol**: For all scenarios requiring communication between C++ components and separate scripting processes (e.g., external Python environments, or internal Python sub-processes if used for true parallelism), **ZeroMQ** will be the underlying messaging library and **MessagePack** will be the data serialization protocol. This ensures high-performance, flexible, and language-agnostic data exchange.

## Specification

### 1. `IScriptEngine` Interface

The core C++ interface for any scripting engine.

```cpp
// src/include/plh_scripting.hpp
#pragma once

#include <string>
#include <memory>
#include <vector>
#include <variant> // For flexible argument/return types
#include <functional> // For std::function
#include <map>

namespace pylabhub {
namespace scripting {

// Forward declaration for script context (e.g., a handle to the calling hub)
class IScriptContext;

// Recursive definitions for structured data (forward declarations)
struct ScriptList;
struct ScriptMap;

// Generic type for script function arguments and return values
using ScriptValue = std::variant<
    std::monostate,
    bool,
    int64_t,
    double,
    std::string,
    std::vector<char>, // For raw bytes, e.g., MessagePack
    void*, // For passing opaque C++ pointers/handles to script and back (with care)
    ScriptList,
    ScriptMap
>;

struct ScriptList : std::vector<ScriptValue> {};
struct ScriptMap : std::map<std::string, ScriptValue> {};


class IScriptEngine {
public:
    virtual ~IScriptEngine() = default;

    // Initializes the script engine with a context.
    // This must be called before any scripts are loaded or functions are called.
    virtual bool initialize(std::shared_ptr<IScriptContext> context) = 0;

    // Loads an initialization script (e.g., for warm-up, global definitions).
    // This script is executed immediately upon loading.
    virtual bool load_initialization_script(const std::string& script_content, const std::string& script_name = "init_script") = 0;
    virtual bool load_initialization_script_from_file(const std::string& file_path) = 0;

    // Loads a main processing script from a string. Returns true on success, false on error.
    // This script typically defines functions that will be called later by C++.
    virtual bool load_script_from_string(const std::string& script_content, const std::string& script_name = "anon_script") = 0;

    // Loads a main processing script from a file path. Returns true on success, false on error.
    virtual bool load_script_from_file(const std::string& file_path) = 0;

    // Calls a function within the loaded script.
    // Returns the result of the script function, or an error variant.
    virtual ScriptValue call_function(const std::string& function_name, const std::vector<ScriptValue>& args) = 0;

    // Registers a C++ function to be callable from the script environment.
    // The C++ function must match a specific signature (std::function<ScriptValue(const std::vector<ScriptValue>&)>).
    // This allows scripts to call back into C++ code, enabling bidirectional communication.
    virtual bool register_cpp_function(const std::string& script_func_name, std::function<ScriptValue(const std::vector<ScriptValue>&)> cpp_func) = 0;

    // Retrieves any pending errors from the script engine.
    virtual std::string get_last_error() const = 0;

    // Attempts to reload a previously loaded script.
    // Behavior is engine-specific. For some, it might destroy and re-create the engine.
    // For others, it might re-evaluate the script within the existing context.
    // The C++ host is responsible for managing when and how this is invoked.
    virtual bool reload_script(const std::string& script_name_or_path) = 0;

    // Configures the script environment with C++ provided data.
    // This method is called by the C++ host to provide initial configuration data
    // to the script environment. The script is expected to implement a corresponding
    // function (e.g., `configure(config_data)`) to process this data.
    virtual bool configure_script_environment(const ScriptValue& config_data) = 0;

    // Calls a designated "warm-up" function in the script to pre-JIT or pre-compile.
    // The script should provide an entry point (e.g., `warmup_function(dummy_data)`)
    // that the C++ host calls with representative dummy data.
    virtual bool warmup_script_function(const std::string& function_name, const std::vector<ScriptValue>& args) = 0;

    // Calls a designated "cleanup" function in the script to release resources.
    // The script should provide an entry point (e.g., `cleanup()`) that the C++
    // host calls before the engine is destroyed.
    virtual bool cleanup_script_function(const std::string& function_name) = 0;
};

// Interface for objects that provide context to scripts (implemented by the C++ host)
class IScriptContext {
public:
    virtual ~IScriptContext() = default;
    // Example: A method for the script to log messages back to the C++ application
    virtual void log_message(const std::string& message) = 0;
    // Example: A method to allow the script to send a message via the MessageHub
    virtual bool send_hub_message(const std::string& type, const std::vector<char>& payload) = 0;
    // Example: A method to read data from a DataBlock (needs careful authorization)
    virtual ScriptValue read_datablock_data(const std::string& datablock_name, size_t offset, size_t size) = 0;
    // ... potentially other methods to interact with the C++ host, always considering access control
};

// Factory function for creating script engines based on a type string.
std::unique_ptr<IScriptEngine> create_script_engine(const std::string& engine_type);

} // namespace scripting
} // namespace pylabhub
```

### 2. `LuaJITScriptEngine` Implementation (Conceptual)

This will be the first concrete implementation of `IScriptEngine`.

```cpp
// src/utils/luajit_script_engine.hpp
#pragma once

#include "plh_scripting.hpp"
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <map>

namespace pylabhub {
namespace scripting {

class LuaJITScriptEngine : public IScriptEngine {
public:
    LuaJITScriptEngine();
    ~LuaJITScriptEngine() override;

    bool initialize(std::shared_ptr<IScriptContext> context) override;
    bool load_initialization_script(const std::string& script_content, const std::string& script_name = "init_script") override;
    bool load_initialization_script_from_file(const std::string& file_path) override;
    bool load_script_from_string(const std::string& script_content, const std::string& script_name = "anon_script") override;
    bool load_script_from_file(const std::string& file_path) override;
    ScriptValue call_function(const std::string& function_name, const std::vector<ScriptValue>& args) override;
    bool register_cpp_function(const std::string& script_func_name, std::function<ScriptValue(const std::vector<ScriptValue>&)> cpp_func) override;
    std::string get_last_error() const override;
    bool reload_script(const std::string& script_name_or_path) override;
    bool configure_script_environment(const ScriptValue& config_data) override;
    bool warmup_script_function(const std::string& function_name, const std::vector<ScriptValue>& args) override;
    bool cleanup_script_function(const std::string& function_name) override;


private:
    lua_State* L;
    std::string last_error_message;
    std::shared_ptr<IScriptContext> script_context;
    std::map<std::string, std::function<ScriptValue(const std::vector<ScriptValue>&)>> cpp_callbacks;

    // Helper functions for Lua stack manipulation and type conversion
    void push_script_value(lua_State* L, const ScriptValue& val);
    ScriptValue pop_script_value(lua_State* L);
    
    // Static C function for Lua to call C++ callbacks
    static int lua_cpp_callback_dispatcher(lua_State* L);
    
    // Store a pointer to 'this' in Lua registry for callbacks
    void store_this_pointer_in_registry();
    LuaJITScriptEngine* get_this_pointer_from_registry();

    // Internal helper to load and run a script from content
    bool load_and_run_script_content(const std::string& script_content, const std::string& script_name);
};

} // namespace scripting
} // namespace pylabhub
```

**Key LuaJIT Specifics**:
*   Uses `lua_State* L` for the Lua virtual machine.
*   `push_script_value` and `pop_script_value` will handle conversion between `ScriptValue` and Lua types (numbers, strings, booleans, tables, userdata for `void*`).
*   C++ functions will be exposed via `lua_cpp_callback_dispatcher` which will retrieve the `LuaJITScriptEngine*` instance from the Lua registry, find the registered C++ callback, and invoke it.
*   Error handling will involve checking Lua stack for errors after `lua_pcall` and storing the message.
*   **Sandboxing**: During `initialize()`, disable problematic Lua functions (`io.*`, `os.*`, `dofile`, `loadfile`, `module`, etc.) using `lua_pushnil`, `lua_setglobal`, or by replacing their metatables.

### 3. Integration Points

The `MessageHub` and `DataBlock` objects will need to be able to host and interact with a script engine.

*   **Hosting**: A `std::unique_ptr<IScriptEngine>` member can be added to relevant hub classes.
*   **Initialization**: When a hub object is created, it can create and initialize its `IScriptEngine` instance, passing itself (or a facade/adapter) as the `IScriptContext`.
*   **Script Loading**: Methods like `load_processing_script(filepath)` can be added to hub objects. Initial (warm-up) scripts will be loaded first.
*   **Triggering**: Hub events (e.g., new message received, DataBlock update) will trigger calls to `IScriptEngine::call_function` for a predefined script function (e.g., `on_message_received(message_data)`).
*   **Context for Script**: The `IScriptContext` interface will be implemented by the hub object (or a dedicated proxy) to allow scripts to interact with the hub's functionalities (logging, sending messages, accessing data), always respecting the defined access control.

```cpp
// Conceptual integration in a Hub object
class MyMessageHub : public pylabhub::scripting::IScriptContext,
                     public std::enable_shared_from_this<MyMessageHub> // For shared_ptr to context
{
public:
    MyMessageHub() : script_engine(pylabhub::scripting::create_script_engine("LuaJIT")) {
        if (script_engine) {
            // Important: Use shared_from_this() only after the object is fully constructed and managed by a shared_ptr.
            // For constructor, may need to defer `initialize` or pass 'this' as raw ptr,
            // then upgrade to shared_ptr later or use a weak_ptr in IScriptEngine.
            // A common pattern is to have a factory function that creates the hub and initializes the script engine.
            std::shared_ptr<MyMessageHub> self_ptr = shared_from_this(); // Get a shared_ptr to myself
            script_engine->initialize(std::static_pointer_cast<pylabhub::scripting::IScriptContext>(self_ptr));

            // Register C++ functions that scripts can call
            script_engine->register_cpp_function("send_log", [this](const std::vector<ScriptValue>& args){
                if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
                    log_message(std::get<std::string>(args[0]));
                }
                return ScriptValue(); // Return monostate for void functions
            });
            // ... other registrations
            
            // Load initialization script (e.g., from a config)
            script_engine->load_initialization_script_from_file("config/hub_init.lua");

            // Configure the script environment
            ScriptMap config;
            config["allowed_modules"] = ScriptList{"numpy", "zmq"};
            config["log_level"] = "INFO";
            script_engine->configure_script_environment(config);

            // Warm-up a specific function
            script_engine->warmup_script_function("on_message", {ScriptValue(std::vector<char>{1,2,3})});
        }
    }

    void on_message_received(const Message& msg) {
        if (script_engine) {
            std::vector<ScriptValue> script_args;
            // Convert Message to ScriptValue representation (e.g., MessagePack bytes or Lua table)
            // script_args.push_back(message_to_script_value(msg));
            ScriptValue result = script_engine->call_function("on_message", script_args);
            // Process script result if needed
            // Example: if (std::holds_alternative<bool>(result) && std::get<bool>(result)) { /* script handled it */ }
        }
    }

    // IScriptContext implementation
    void log_message(const std::string& message) override {
        // Log the message using the C++ logger
    }
    bool send_hub_message(const std::string& type, const std::vector<char>& payload) override {
        // Implement sending a message via the MessageHub, checking for script's authorization
        return true;
    }
    ScriptValue read_datablock_data(const std::string& datablock_name, size_t offset, size_t size) override {
        // Implement reading from a DataBlock, with authorization checks
        return ScriptValue();
    }
private:
    std::unique_ptr<pylabhub::scripting::IScriptEngine> script_engine;
    // ... other hub members
};
```

### 4. CMake Support

*   **`src/CMakeLists.txt`**: Needs to include `plh_scripting.hpp` and define a library for `pylabhub::scripting`. This library will be an `INTERFACE` library if it only contains headers, or a static/shared library if it has shared C++ code (e.g., the `create_script_engine` factory).
*   **`src/utils/CMakeLists.txt`**: Will build `luajit_script_engine.cpp` and create a `pylabhub::scripting::luajit` target.
*   **Linkage**:
    *   `pylabhub::scripting` library (or `pylabhub::utils` if the implementation resides there) will link against `pylabhub::third_party::luajit`.
    *   Any hub object consuming `IScriptEngine` will link against `pylabhub::scripting`.

### 5. Scripting Environment Management Protocols

To ensure robust and predictable behavior across different scripting engines, the following protocols are defined for the C++ host to interact with embedded scripts:

*   **Configuration (`configure_script_environment`)**:
    *   **C++ Host**: The host calls `IScriptEngine::configure_script_environment(config_data)` with a `ScriptValue` (typically a `ScriptMap` or serialized data) containing initial configuration.
    *   **Script Interface**: Scripts are expected to define a function (e.g., `configure(config_table)` in Lua, `configure(config_dict)` in Python) which the C++ host will invoke through `configure_script_environment`. This function allows the script to read and apply language-specific settings.
*   **Initialization (`load_initialization_script*`, `init` function)**:
    *   **C++ Host**: The host loads an initialization script via `IScriptEngine::load_initialization_script*`. After loading, it automatically calls a predefined script function (e.g., `init()` in Lua/Python) via `IScriptEngine::call_function`.
    *   **Script Interface**: Scripts must provide an `init()` function that performs any necessary language-specific setup, global variable definitions, or module imports.
*   **Warm-up/JIT (`warmup_script_function`)**:
    *   **C++ Host**: The host calls `IScriptEngine::warmup_script_function(function_name, dummy_args)` before critical data processing begins.
    *   **Script Interface**: Scripts must provide a function (e.g., `warmup_process(dummy_data)`) which the C++ host will call with representative dummy data. This allows JIT compilers to optimize code paths and ensures resources are pre-allocated.
*   **Cleanup (`cleanup_script_function`)**:
    *   **C++ Host**: The host calls `IScriptEngine::cleanup_script_function(function_name)` before destroying the `IScriptEngine` instance.
    *   **Script Interface**: Scripts must provide a function (e.g., `cleanup()` in Lua/Python) that releases any script-managed resources (e.g., closes files, flushes buffers) to ensure a graceful shutdown.

### 6. Future Work

*   **Python Integration**: Implement `PythonScriptEngine` using `pybind11` or similar, adhering to the `IScriptEngine` interface.
    *   **Python in two folds**:
        *   **Internal Scripting (Strictly Confined for Well-Defined Tasks)**:
            *   **Purpose**: This environment is designed for strict, confined, and high-performance data processing and logic handling tasks. It operates within a tightly controlled environment to minimize overhead and maximize responsiveness for critical operations.
            *   **Python Instances**: For internal Python scripting within a single C++ process, the most practical approach is to manage a **single embedded Python interpreter instance**. While CPython supports sub-interpreters, their limitations (object sharing, C-extension compatibility) make them less suitable for general-purpose, robust parallelism in this context.
            *   **Multi-threading and the GIL (Best Practices)**: When multiple C++ threads need to run Python code (e.g., via different `PythonScriptEngine` instances), they will all contend for the single GIL. To mitigate this and maximize concurrency, script writers are responsible for cooperative coding:
                1.  **GIL Management in C++**: The `PythonScriptEngine` implementation *must* correctly acquire and release the GIL (`py::gil_scoped_acquire`, `py::gil_scoped_release` from `pybind11`). The GIL should be held only when actively executing Python code. C++ functions called from Python that perform long-running operations should release the GIL before starting their work and reacquire it before returning to Python.
                2.  **Cooperative Python Code**: Users writing internal Python scripts are responsible for writing GIL-cooperative code. This includes:
                    *   Relying on C-extension libraries (like NumPy, SciPy) that release the GIL during their computationally intensive operations.
                    *   Avoiding long-running CPU-bound loops in pure Python without explicitly yielding control.
                    *   Structuring logic to hand off control to GIL-releasing C++ functions when possible.
                3.  **True Parallelism**: For pure Python code that requires true, CPU-bound parallelism, the recommended approach is to leverage multi-process Python features (e.g., `multiprocessing` module) within the embedded interpreter, or to utilize the "External Python Processing" model described below.
            *   **Environment**: Uses an embedded Python interpreter configured with a specific, limited set of trusted packages (e.g., NumPy, ZeroMQ, MessagePack). Any deviation from this controlled set would shift it towards the external model.
        *   **External Python Processing (Flexible & Expandable)**:
            *   **Purpose**: This mode provides maximum flexibility and expandability, allowing users to leverage their full Python environment (with arbitrary packages). It's suitable for complex data science workflows, machine learning models, or integration with diverse third-party libraries, where higher latency can be tolerated for greater functionality.
            *   **Architecture**: The `PythonScriptEngine` for this mode would not embed a Python interpreter directly. Instead, it would act as an IPC client (e.g., using ZeroMQ sockets, HTTP, or a custom RPC protocol) communicating with a separate, externally managed Python process. This external process could be running in a user-defined virtual environment or Docker container. **ZeroMQ and MessagePack will be the consistent underlying protocols for this inter-process communication**, ensuring high-performance, flexible, and language-agnostic data exchange.
            *   **Script Interface**: The external Python process would implement a well-defined API (matching our `IScriptEngine` expectations for `call_function`, `register_cpp_function`, etc.) that the C++ `PythonScriptEngine` adapter translates into IPC messages.
            *   **Environment**: The user provides and manages their own Python environment and package installations.
    *   **Key Differences & Adaptations for Python (General)**:
        *   **Complex `ScriptValue` Types**: Python heavily relies on lists, dictionaries, and custom objects. The `ScriptValue` (`std::variant`) has been extended with `ScriptList` and `ScriptMap` to better accommodate these, but efficient conversion to/from native Python types will be crucial for `pybind11` implementations.
        *   **Sandboxing**: Python's rich standard library offers extensive access to the filesystem, network, and OS. Disabling these for sandboxing is more complex than in Lua and would require custom import hooks, `os` module replacement, or dedicated restricted execution environments.
        *   **Error Handling**: Python's rich exception objects would need to be carefully caught, logged, and possibly mapped to C++ exceptions or standardized error strings via `get_last_error()`.
*   **Dynamic Loading of Native Modules**: Allow the script interface to load pre-approved C++ shared libraries (DLLs/SOs) with strictly defined C interfaces for high-performance operations. This introduces a "plugin" mechanism, distinct from general scripting. The C++ host would manage `dlopen`/`LoadLibrary` calls and expose the loaded functionality to the script engine. This mechanism requires *extremely strict* access control and verification of module integrity (e.g., digital signatures, trusted paths).
*   **Hot Reloading (Advanced)**: Implement more sophisticated `reload_script` methods that attempt to preserve script state during re-evaluation, rather than full engine recreation. This is highly engine-specific.
*   **Script Debugging**: Consider integration with external script debuggers.
*   **DataBlock Direct Access**: For extremely high-performance scenarios, explore exposing raw `DataBlock` memory pointers to trusted scripts (e.g., LuaJIT FFI with C structures), guarded by stringent access control.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.