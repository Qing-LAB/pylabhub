#pragma once

/*******************************************************************************
 * @file RecursionGuard.hpp
 * @brief A thread-local, RAII-based guard to detect and prevent re-entrant calls.
 *
 * **Design and Purpose**
 * `RecursionGuard` is a utility designed to prevent deadlocks in classes that
 * use non-recursive mutexes for serialization. It works by tracking, on a
 * per-thread basis, which object instances are currently inside a guarded
 * function call.
 *
 * **How It Works**
 * 1.  **Thread-Local Stack**: The guard uses a `thread_local` vector of `void*`
 *     pointers. Each thread in the application gets its own independent instance
 *     of this vector, which acts as a "call stack" of object pointers.
 * 2.  **RAII Guard**: When a `RecursionGuard` object is created, it pushes the
 *     provided object's pointer (`key`) onto the current thread's stack. When
 *     the guard is destroyed (at the end of a scope), it pops the pointer off.
 * 3.  **Static Check**: The `is_recursing(key)` static method allows a function
 *     to check if its object's pointer is already on the current thread's stack
 *     before it proceeds. If it is, this indicates a nested (recursive) call,
 *     which can then be safely rejected.
 *
 * **Usage**
 * This pattern is used by `JsonConfig` to prevent deadlocks.
 *
 * ```cpp
 * void MyClass::some_method() {
 *     const void* key = static_cast<const void*>(this);
 *
 *     // Check for re-entrancy before locking the mutex.
 *     if (RecursionGuard::is_recursing(key)) {
 *         // Log a warning and refuse to enter.
 *         return;
 *     }
 *
 *     // Create the guard to mark entry into the protected section.
 *     RecursionGuard guard(key);
 *
 *     // Now it's safe to lock the non-recursive mutex.
 *     std::lock_guard<std::mutex> lock(my_mutex_);
 *     // ... do work ...
 * }
 * ```
 *
 * **Performance Impact**
 * The overhead is negligible. The check involves a fast search on a very small,
 * thread-local vector, and the RAII object performs a simple push/pop. This is
 * significantly cheaper than the mutex lock it is designed to protect.
 ******************************************************************************/

#include "platform.hpp" // For PYLABHUB_API
#include <vector>

namespace pylabhub::utils
{

class PYLABHUB_API RecursionGuard
{
  public:
    // RAII guard. Pushes key onto thread-local stack on construction, pops on destruction.
    explicit RecursionGuard(const void *key);
    ~RecursionGuard();

    RecursionGuard(const RecursionGuard &) = delete;
    RecursionGuard &operator=(const RecursionGuard &) = delete;

    // Checks if the given key is already present on the current thread's stack.
    static bool is_recursing(const void *key);

  private:
    const void *key_;
    // Declaration of the thread-local stack. Definition is in the .cpp file.
    // this has been REMOVED from the header because Windows does not allow
    // thread_local variables to be exposed in DLLs. Instead, we are defining it
    // only in the .cpp file as a static thread-local variable.
    // static thread_local std::vector<const void *> g_stack;
};

} // namespace pylabhub::utils