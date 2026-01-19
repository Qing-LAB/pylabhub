

---

### `SharedMemoryHub` (C++ Namespace: `pylabhub::hub`)

The Data Exchange Hub provides a high-performance inter-process communication (IPC) framework, as specified in the `HEP-core-0002` design document. It uses a central broker for service discovery and supports multiple communication patterns. All hub components are managed as dynamic lifecycle modules.

*   **Key Concepts**:
    *   **Hub**: The primary entry point, created via `Hub::connect()`. It manages the connection to the broker and acts as a factory for communication channels.
    *   **Channels**: The hub provides two main types of channels:
        1.  **High-Performance (Shared Memory)**: For zero-copy data transfer between processes on the same machine. Managed by `SharedMemoryProducer` and `SharedMemoryConsumer`.
        2.  **General-Purpose (ZeroMQ)**: For flexible messaging patterns, including Publish-Subscribe (`ZmqPublisher`/`ZmqSubscriber`) and Request-Reply (`ZmqRequestServer`/`ZmqRequestClient`).

*   **Lifecycle Management**: The hub framework is designed to be loaded dynamically. Before using any hub components, you must first load its lifecycle module:
    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/SharedMemoryHub.hpp"
    #include "utils/Logger.hpp"

    // In main(), after static initialization:
    pylabhub::utils::RegisterDynamicModule(pylabhub::hub::GetLifecycleModule());
    pylabhub::utils::LoadModule("pylabhub::hub::DataExchangeHub");
    ```

*   **Basic Usage**:
    ```cpp
    #include "utils/SharedMemoryHub.hpp"
    #include <string>
    #include <vector>

    void run_hub_example() {
        // 1. Configure and connect to the broker
        pylabhub::hub::BrokerConfig config;
        config.endpoint = "tcp://localhost:5555";
        config.broker_public_key = "YOUR_BROKER_PUBLIC_KEY"; // Replace with actual key
        
        auto hub = pylabhub::hub::Hub::connect(config);
        if (!hub) {
            // Handle connection failure
            return;
        }

        // 2. Create a high-performance producer
        auto producer = hub->create_shm_producer("my_data_channel", 1024 * 1024);
        if (producer) {
            // Write data to the shared memory buffer
            void* buffer = producer->begin_publish();
            // ... write to buffer ...
            uint64_t dims[] = {0,0,0,0};
            producer->end_publish(/*data_size*/ 512, /*timestamp*/ 0.0, /*type_hash*/ 0, dims);
        }

        // 3. Create a general-purpose publisher
        auto publisher = hub->create_publisher("my_topic");
        if (publisher) {
            std::string msg = "hello world";
            publisher->publish("event.update", msg.data(), msg.size());
        }
    }
    ```

### `PythonLoader` (C++ Namespace: `pylabhub::utils`)

The `PythonLoader` provides a C-style, ABI-stable interface for managing an embedded Python interpreter. This utility is designed specifically for integration with C-based frameworks like the Igor Pro XOP (External Operation) toolkit.

*   **Key Features**:
    *   **Dynamic Loading**: It does not link against a specific Python library at compile time. Instead, it dynamically loads the Python shared library (`python3.x.dll` or `libpython3.x.so`) at runtime from a path specified by the user.
    *   **State Management**: It provides explicit `extern "C"` functions to initialize (`PyLoader_init`), configure (`PySetPython`), execute code in (`PyExec`), and shut down (`PyCleanup`) the interpreter.
    *   **Igor Pro Integration**: The function signatures and data structures are designed to be directly compatible with Igor Pro's XOP API.

*   **Usage Context**:
    This is a specialized utility and is typically not used in standard C++ applications. Its primary use case is within the `src/IgorXOP` project to enable Python scripting inside Igor Pro.
