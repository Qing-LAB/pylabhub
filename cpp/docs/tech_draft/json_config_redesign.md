# JsonConfig Redesign: Transactional and Atomic Operations

This document outlines the redesigned approach for `JsonConfig` to ensure atomic, process-safe read and write operations via a simple and safe transactional API.

## Core Design Principles

The new design is centered around a temporary, move-only "Transaction" token. This object acts as a capability, granting access to perform a single, scoped operation on the `JsonConfig` instance. This new API co-exists with the original manual-locking API (`lock_for_read`, `lock_for_write`), providing a simpler and safer alternative for most use cases.

1.  **Simplified, Safe API**: The primary goal is to provide a simple, lambda-based API that prevents common mistakes. The transactional functions (`with_json_read`, `with_json_write`) manage all locking and resource management, reducing boilerplate and increasing safety.
2.  **Pimpl Idiom Compliance**: The implementation details remain hidden. The transaction token provides authorized access to the private `pImpl` of `JsonConfig`, avoiding the need to expose internal state in the public header.
3.  **Clear, Single-Use Intent**: The API is designed to accept the transaction token by rvalue reference (`&&`). This enforces a clean, single-use semantic where a temporary token is created and immediately consumed by the operation (e.g., `with_json_write(config.transaction(), ...)`).
4.  **No Resource Leaks**: The transaction token is a lightweight object created on the stack, used for a single call, and immediately destroyed. Its lifetime is strictly controlled.
5.  **Fluent and Expressive API**: Behavioral flags (e.g., whether to reload from disk or commit after writing) are encapsulated within the transaction token itself, keeping the main function signatures clean and readable.
6.  **Preservation of Manual Control**: The original `lock_for_read` and `lock_for_write` primitives are preserved for advanced use cases that require fine-grained, manual control over lock lifetimes.

## Implementation Plan

1.  **Introduce `AccessFlags` Enum**: Define an `enum class AccessFlags` within `JsonConfig.hpp` to specify the behavior of a transaction.
    ```cpp
    enum class AccessFlags
    {
        Default = 0,
        UnSynced = Default, // In-memory only
        ReloadFirst = 1 << 0,
        CommitAfter = 1 << 1,
        FullSync = ReloadFirst | CommitAfter, // Reload, then commit
    };
    ```
2.  **Define `JsonConfig::Transaction` Token**:
    *   Create a public, nested, move-only class `JsonConfig::Transaction` inside the `JsonConfig` class definition.
    *   It contains a pointer to its parent `JsonConfig` instance and the `AccessFlags` for the transaction.
3.  **Implement `JsonConfig::transaction()` Method**:
    *   Add a public method: `[[nodiscard]] Transaction transaction(AccessFlags flags = AccessFlags::Default) noexcept;`
    *   This factory method constructs and returns a configured `Transaction` token.
4.  **Create Free Functions and `.inl` File**:
    *   Declare templated, free `friend` functions `with_json_read` and `with_json_write` in `JsonConfig.hpp`.
    *   The function signatures are clean and enforce the single-use token pattern:
        ```cpp
        template <typename F>
        void with_json_write(JsonConfig::Transaction&& tx, F&& fn, std::error_code *ec = nullptr);
        ```
    *   Create a new file `src/include/utils/JsonConfig.inl` to hold the template implementations. This file is included at the end of `JsonConfig.hpp`.
5.  **Implement Transactional Logic**:
    *   Inside `JsonConfig.inl`, the `with_json_read` and `with_json_write` functions use the existing `lock_for_read` and `lock_for_write` primitives to ensure thread safety.
    *   They interpret the `AccessFlags` on the transaction token to conditionally call `reload()` or `commit()` on the lock guards, providing the desired atomicity and disk-sync behavior.
6.  **Update Tests**: Refactor tests in `test_jsonconfig.cpp` to use the new, fluent API, and add a new test case (`ManualLockingApi`) to ensure the manual-locking primitives remain functional.