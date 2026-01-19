# Refactoring the `JsonConfig` Transactional API

## 1. Motivation

The original design of the `JsonConfig` transactional API, while functional, was unnecessarily complex. It relied on a combination of a nested `Transaction` class acting as a simple token, and two free functions (`with_json_read` and `with_json_write`) that were declared as `friend`s of both `JsonConfig` and the nested `Transaction` class.

This approach had several drawbacks:
- **Poor Encapsulation**: The core transaction logic was implemented in global free functions, loosely coupled to the classes they operated on.
- **Complex Friendships**: The need to declare the free functions as friends in two different classes created a confusing and brittle relationship.
- **Reduced Discoverability**: The API was not intuitive. A developer had to know that the correct pattern was to create a transaction token and then pass it to a separate global function.

The goal of this refactoring is to introduce a cleaner, more object-oriented design that is more intuitive, maintainable, and better encapsulated.

## 2. The New Design: `JsonConfigTransaction`

The new design replaces the free functions with methods on a new, dedicated transaction object.

### 2.1. The `JsonConfigTransaction` Class

A new class, `pylabhub::utils::JsonConfigTransaction`, is introduced. It is no longer nested within `JsonConfig`.

- **Role**: This class is a "smart" transaction object, not just a passive token. It holds a pointer to the parent `JsonConfig` instance and the user-provided `AccessFlags`.
- **Lifecycle**: It is a temporary, move-only object. It is created by calling `JsonConfig::transaction()` and is intended to be used immediately in a "fluent" API chain. The move-only semantics preserve the single-use nature of a transaction.

The class definition is as follows:

```cpp
class PYLABHUB_UTILS_EXPORT JsonConfigTransaction
{
  public:
    // Move-only, non-copyable
    JsonConfigTransaction(JsonConfigTransaction &&) noexcept = default;
    JsonConfigTransaction &operator=(JsonConfigTransaction &&) noexcept = default;
    JsonConfigTransaction(const JsonConfigTransaction &) = delete;
    JsonConfigTransaction &operator=(const JsonConfigTransaction &) = delete;
    ~JsonConfigTransaction();

    // The new transactional methods
    template <typename F>
    void read(F &&fn, std::error_code *ec = nullptr) &&;

    template <typename F>
    void write(F &&fn, std::error_code *ec = nullptr) &&;

  private:
    friend class JsonConfig;
    explicit JsonConfigTransaction(JsonConfig *owner,
                                   typename JsonConfig::AccessFlags flags) noexcept;

    JsonConfig *d_owner;
    typename JsonConfig::AccessFlags d_flags;
};
```

### 2.2. The Fluent API

The primary change from a user's perspective is the move to a fluent API. The `read()` and `write()` methods are called directly on the object returned by `transaction()`.

- `config.transaction().read(...)`
- `config.transaction(...).write(...)`

These methods are move-qualified (`&&`), which enforces that they can only be called on a temporary rvalue object, preventing misuse like storing the transaction object for later.

## 3. Usage Comparison

The new API is functionally identical but more readable.

#### **Before (Old Design)**

```cpp
#include "utils/JsonConfig.hpp"

void old_example(pylabhub::utils::JsonConfig& config)
{
    std::error_code ec;
    using Flags = pylabhub::utils::JsonConfig::AccessFlags;
    
    // Reading required passing a transaction token to a free function.
    with_json_read(config.transaction(), [&](const auto& j) {
        // ...
    }, &ec);

    // Writing followed the same pattern.
    with_json_write(config.transaction(Flags::CommitAfter), [&](auto& j) {
        j["new_setting"] = true;
    }, &ec);
}
```

#### **After (New Design)**

```cpp
#include "utils/JsonConfig.hpp"

void new_example(pylabhub::utils::JsonConfig& config)
{
    std::error_code ec;
    using Flags = pylabhub::utils::JsonConfig::AccessFlags;

    // Reading is a clear, chained method call.
    config.transaction().read([&](const auto& j) {
        // ...
    }, &ec);

    // Writing follows the same intuitive, fluent pattern.
    config.transaction(Flags::CommitAfter).write([&](auto& j) {
        j["new_setting"] = true;
    }, &ec);
}
```

## 4. Design & Implementation Benefits

1.  **Improved Encapsulation**: All transaction logic (reloading, locking, committing) is now implemented inside the methods of `JsonConfigTransaction`, rather than in global space.
2.  **Simplified Friendships**: The messy double-friendship with free functions is gone. We now have a clean, reciprocal friendship between `JsonConfig` and `JsonConfigTransaction`—a standard pattern for tightly coupled helper classes. `JsonConfig` is a friend to access the private constructor, and `JsonConfigTransaction` is a friend to access private locking/IO helpers.
3.  **Enhanced Readability & Discoverability**: The fluent API `config.transaction().read(...)` is more self-documenting. Developers can use IDE auto-completion to easily discover the `read` and `write` methods, which is not possible with the free-function-based approach.
4.  **Better Maintainability**: With the logic properly encapsulated, the transactional feature is easier to debug, modify, and extend without affecting unrelated code.

## 5. File Structure Changes

- **`src/include/utils/JsonConfig.hpp`**: Modified to remove the nested `Transaction` class and free functions, and to declare `JsonConfigTransaction` and the new `transaction()` method signature.
- **`src/include/utils/JsonConfig.inl`**: This file is now removed.
- **`src/include/utils/JsonConfigTransaction.inl`**: This new file is created to contain the template implementations for `JsonConfigTransaction::read` and `JsonConfigTransaction::write`.
- **`src/utils/JsonConfig.cpp`**: Updated to implement `JsonConfig::transaction()` and the `JsonConfigTransaction` destructor.
