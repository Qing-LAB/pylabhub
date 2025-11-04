// JsonConfig.hpp
#pragma once
/*
JsonConfig.hpp

Overview & design goals
-----------------------
- Provide a thread-safe in-memory JSON configuration manager with atomic on-disk
  writes and cross-process coordination using FileLock.
- Offer two primary read patterns:
    * as_json() - copies the entire JSON (safe to hold/use outside locks).
    * with_json_read(cb) - invokes a callback with a const json& under a shared lock
      to avoid copying for short-lived reads.
- Provide a write pattern:
    * with_json_write(cb) - invokes a callback while holding the structural mutex
      (_initMutex) and then persists the result (save_locked) while still holding
      that mutex. Nested with_json_write on the same instance is explicitly refused.
- Template helpers: set/get/get_or/has/erase/update are provided in-header and
  operate on top-level keys. Mutators set a per-instance atomic dirty flag.
- save() is the public persistence API; save_locked(ec) performs the write and
  assumes the caller holds _initMutex (or that we are in the fast-path from
  inside with_json_write).

Design notes & critical considerations (summarized)
-------------------------------------------------
- In-process concurrency:
    - _initMutex: guards structural state and is held by with_json_write during user callbacks.
      We intentionally keep this non-recursive and *refuse* nested with_json_write to avoid
      hidden reentrancy complexity and deadlocks.
    - Impl::rwMutex: shared_mutex protecting the in-memory JSON for fine-grained readers/writers.
    - Per-thread, per-instance reentrancy detection: a thread-local stack of active with_json_write
      instances is used to detect and refuse nested with_json_write on the *same* object.
- Cross-process concurrency:
    - FileLock is used by save_locked to prevent simultaneous writers in different processes.
- Dirty flag:
    - Impl::dirty (std::atomic<bool>) marks when memory may be newer than disk. save_locked
      skips disk writes when dirty==false. Mutators set dirty=true; successful save clears it.
- Exception safety:
    - Public functions swallow exceptions and log errors where appropriate, returning bool for
failure.
    - Templates follow the existing code style: read helpers use noexcept and return
success/failure.

Notes for maintainers
---------------------
- If you need synchronous set-and-persist semantics, add a helper set_and_save() that calls set()
then save().
- The current design optimizes frequent reads (avoid copying) and avoids unnecessary disk writes.
- If you want reentrant with_json_write support, replace the non-recursive _initMutex with an
  explicit owner + recursion count and adjust with_json_write accordingly (more complex).
*/

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "fileutil/FileLock.hpp"
#include "nlohmann/json.hpp"
#include "util/Logger.hpp"

namespace pylabhub::fileutil
{

using json = nlohmann::json;

class JsonConfig
{
  public:
    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile);
    ~JsonConfig();

    // disallow copy/move at class level (mutex prevents defaulted move)
    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;

    // disallow move as default — implement later if needed
    JsonConfig(JsonConfig &&) noexcept = delete;
    JsonConfig &operator=(JsonConfig &&) noexcept = delete;

    // Non-template operations (implemented in JsonConfig.cpp)
    bool init(const std::filesystem::path &configFile, bool createIfMissing);
    bool save() noexcept;
    bool reload() noexcept;
    bool replace(const json &newData) noexcept;
    json as_json() const noexcept;

    // with_json_write: exclusive write callback. It holds _initMutex during the callback,
    // then calls save_locked() while still holding the mutex to persist changes.
    // Returns true if callback ran and save succeeded.
    template <typename F> bool with_json_write(F &&fn);

    // with_json_read: read-only callback that receives const json& under a shared lock.
    // Returns true on success; false if the instance is not initialized or if the callback throws.
    // noexcept and const so callers can safely call it without exceptions escaping.
    template <typename F> bool with_json_read(F &&cb) const noexcept;

    // ---------------- Template accessors ----------------
    // Top-level key helpers. Implemented in-header (templates).
    template <typename T> bool set(const std::string &key, T const &value) noexcept;

    template <typename T> T get(const std::string &key) const;

    template <typename T> T get_or(const std::string &key, T const &default_value) const noexcept;

    bool has(const std::string &key) const noexcept;
    bool erase(const std::string &key) noexcept;

    template <typename Func> bool update(const std::string &key, Func &&updater) noexcept;

  private:
    // private impl holds the JSON + locks + dirty flag
    // complete type here so header-only/template functions can safely access _impl members
    // -----------------------------------------------------------------------------
    struct Impl
    {
        std::filesystem::path configPath;
        nlohmann::json data;
        std::shared_mutex rwMutex;      // protects data for readers/writers
        std::atomic<bool> dirty{false}; // true if memory may be newer than disk

        Impl() = default;
        ~Impl() = default;

        // non-copyable/movable underlying members are fine because Impl is
        // only owned via unique_ptr in JsonConfig.
    };

    std::unique_ptr<Impl> _impl;

    // structural mutex (non-recursive). Protects lifecycle of _impl and serializes with_json_write.
    mutable std::mutex _initMutex;

    // save_locked: performs the actual atomic on-disk write. Caller must hold _initMutex.
    bool save_locked(std::error_code &ec);

    // Reentrancy detection: a thread-local stack of active with_json_write instances.
    // Declared here; defined (thread_local storage) in JsonConfig.cpp so the template code can
    // reference it while the storage lives in exactly one translation unit.
    // thread_local storage declaration (single definition remains in .cpp)
    static thread_local std::vector<const void *> g_with_json_write_stack;
    struct WithJsonWriteReentrancyGuard
    {
        explicit WithJsonWriteReentrancyGuard(const void *k) : key(k)
        {
            g_with_json_write_stack.push_back(k);
        }
        ~WithJsonWriteReentrancyGuard()
        {
            if (!g_with_json_write_stack.empty() && g_with_json_write_stack.back() == key)
            {
                g_with_json_write_stack.pop_back();
            }
            else
            {
                // defensive removal in case stack got modified unexpectedly
                auto it =
                    std::find(g_with_json_write_stack.begin(), g_with_json_write_stack.end(), key);
                if (it != g_with_json_write_stack.end())
                    g_with_json_write_stack.erase(it);
            }
        }
        WithJsonWriteReentrancyGuard(const WithJsonWriteReentrancyGuard &) = delete;
        WithJsonWriteReentrancyGuard &operator=(const WithJsonWriteReentrancyGuard &) = delete;
        const void *key;
    };

  private:
    // atomic, cross-platform write helper (definition in JsonConfig.cpp)
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j);
};

// ---------------- with_json_write ----------------
template <typename F> bool JsonConfig::with_json_write(F &&fn)
{
    const void *key = static_cast<const void *>(this);

    // Detect nested with_json_write on the same instance for this thread and refuse.
    if (std::find(g_with_json_write_stack.begin(), g_with_json_write_stack.end(), key) !=
        g_with_json_write_stack.end())
    {
        LOGGER_WARN(
            "JsonConfig::with_json_write - nested call detected on same instance; refusing to "
            "re-enter.");
        return false;
    }
    WithJsonWriteReentrancyGuard guard(key);

    // Lock structural mutex to ensure _impl exists and to serialize with other writers.
    std::lock_guard<std::mutex> lg(_initMutex);

    bool callback_ok = false;
    try
    {
        callback_ok = fn();
    }
    catch (...)
    {
        // reentrancy guard will pop; propagate design choice: we allow exception to escape
        // so caller can handle it — but this matches previous behavior where callback exceptions
        // are allowed.
        throw;
    }

    std::error_code ec;
    bool saved = save_locked(ec);
    if (!saved)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: save_locked failed: {}", ec.message());
    }
    return callback_ok && saved;
}

// ---------------- with_json_read ----------------
template <typename F> bool JsonConfig::with_json_read(F &&cb) const noexcept
{
    try
    {
        // Ensure structural state is present before attempting to read
        std::lock_guard<std::mutex> lg(_initMutex);
        if (!_impl)
            return false;

        // Acquire shared lock for concurrent reads
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        const json &ref = _impl->data;

        // Invoke callback with a const reference to avoid copying
        std::forward<F>(cb)(ref);
        return true;
    }
    catch (...)
    {
        // Swallow exceptions as per library convention for read helpers
        return false;
    }
}

// ---------------- Template accessors ----------------

template <typename T> bool JsonConfig::set(const std::string &key, T const &value) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lg(_initMutex);
        if (!_impl)
            _impl = std::make_unique<Impl>();
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        _impl->data[key] = value;
        _impl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename T> T JsonConfig::get(const std::string &key) const
{
    std::lock_guard<std::mutex> lg(_initMutex);
    if (!_impl)
        throw std::runtime_error("JsonConfig::get: not initialized");

    std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
    auto it = _impl->data.find(key);
    if (it == _impl->data.end())
        throw std::runtime_error("JsonConfig::get: key not found: " + key);
    try
    {
        return it->template get<T>();
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error(std::string("JsonConfig::get: conversion failed for key ") + key +
                                 ": " + ex.what());
    }
}

template <typename T>
T JsonConfig::get_or(const std::string &key, T const &default_value) const noexcept
{
    try
    {
        std::lock_guard<std::mutex> lg(_initMutex);
        if (!_impl)
            return default_value;
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        auto it = _impl->data.find(key);
        if (it == _impl->data.end())
            return default_value;
        try
        {
            return it->template get<T>();
        }
        catch (...)
        {
            return default_value;
        }
    }
    catch (...)
    {
        return default_value;
    }
}

inline bool JsonConfig::has(const std::string &key) const noexcept
{
    try
    {
        std::lock_guard<std::mutex> lg(_initMutex);
        if (!_impl)
            return false;
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        return _impl->data.find(key) != _impl->data.end();
    }
    catch (...)
    {
        return false;
    }
}

inline bool JsonConfig::erase(const std::string &key) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lg(_initMutex);
        if (!_impl)
            return false;
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        auto it = _impl->data.find(key);
        if (it == _impl->data.end())
            return false;
        _impl->data.erase(it);
        _impl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename Func> bool JsonConfig::update(const std::string &key, Func &&updater) noexcept
{
    static_assert(std::is_invocable_v<Func, json &>, "update(Func) must be invocable as f(json&)");
    try
    {
        std::lock_guard<std::mutex> lg(_initMutex);
        if (!_impl)
            _impl = std::make_unique<Impl>();
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        json &target = _impl->data[key]; // create if missing
        updater(target);
        _impl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace pylabhub::fileutil

// end JsonConfig.hpp
