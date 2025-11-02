// JsonConfig.hpp
#pragma once

/*
  JsonConfig.hpp

  - Template-based helpers (set/get/get_or/has/erase/update) are implemented in this header.
  - Non-template implementation (init/save/reload/replace/atomic_write_json/save_locked, etc.)
    remain in JsonConfig.cpp and are not changed here.
  - Locking policy matches existing code in JsonConfig.cpp:
    * _initMutex guards lifetime/structure and is taken by public entry points.
    * Impl::rwMutex (std::shared_mutex) protects concurrent read/write access to _impl->data.
*/

#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <stdexcept>
#include <type_traits>

#include "fileutil/FileLock.hpp"
#include "nlohmann/json.hpp"

namespace pylabhub::fileutil {

using json = nlohmann::json;

class JsonConfig {
public:
    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile);
    ~JsonConfig();

    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    // Non-template operations implemented in JsonConfig.cpp
    bool init(const std::filesystem::path &configFile, bool createIfMissing);
    bool save() noexcept;
    bool reload() noexcept;
    bool replace(const json &newData) noexcept;
    json as_json() const noexcept;

    // with_json_write template is in-header and uses save_locked (CPP implementation).
    template <typename F>
    bool with_json_write(F &&fn);

    // Template accessors (in-header)
    template <typename T>
    bool set(const std::string &key, T const &value) noexcept;

    template <typename T>
    T get(const std::string &key) const;

    template <typename T>
    T get_or(const std::string &key, T const &default_value) const noexcept;

    bool has(const std::string &key) const noexcept;
    bool erase(const std::string &key) noexcept;

    template <typename Func>
    bool update(const std::string &key, Func &&updater) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    mutable std::mutex _initMutex; // protects _impl and structural state

    // This function is implemented in the .cpp file. Caller must follow the contract:
    // - public save() locks _initMutex then calls save_locked(...)
    // - with_json_write may call save_locked(...) while still holding _initMutex
    bool save_locked(std::error_code &ec);

    // thread-local stack for with_json_write reentrancy detection
    static thread_local std::vector<const void *> g_with_json_write_stack;

    struct WithJsonWriteReentrancyGuard {
        explicit WithJsonWriteReentrancyGuard(const void *k) : key(k) { g_with_json_write_stack.push_back(k); }
        ~WithJsonWriteReentrancyGuard() {
            if (!g_with_json_write_stack.empty() && g_with_json_write_stack.back() == key) {
                g_with_json_write_stack.pop_back();
            } else {
                auto it = std::find(g_with_json_write_stack.begin(), g_with_json_write_stack.end(), key);
                if (it != g_with_json_write_stack.end()) g_with_json_write_stack.erase(it);
            }
        }
        WithJsonWriteReentrancyGuard(const WithJsonWriteReentrancyGuard &) = delete;
        WithJsonWriteReentrancyGuard &operator=(const WithJsonWriteReentrancyGuard &) = delete;
        const void *key;
    };
};

// define thread_local symbol (header so templates/linkage see it)
thread_local std::vector<const void *> JsonConfig::g_with_json_write_stack;

// ---------------- with_json_write template (keeps previous behavior) ----------------
template <typename F>
bool JsonConfig::with_json_write(F &&fn)
{
    const void *key = static_cast<const void *>(this);
    if (std::find(g_with_json_write_stack.begin(), g_with_json_write_stack.end(), key) !=
        g_with_json_write_stack.end())
    {
        JC_LOG_WARN("JsonConfig::with_json_write - nested call detected on same instance; refusing to re-enter.");
        return false;
    }

    WithJsonWriteReentrancyGuard reentrancy_guard(key);

    std::lock_guard<std::mutex> g(_initMutex);

    bool callback_ok = false;
    try {
        callback_ok = fn();
    } catch (...) {
        throw;
    }

    std::error_code ec;
    bool saved = save_locked(ec);
    if (!saved) {
        JC_LOG_ERROR("JsonConfig::with_json_write: save_locked failed: " << ec.message());
    }

    return callback_ok && saved;
}

// ---------------- Template accessor implementations ----------------

template <typename T>
bool JsonConfig::set(const std::string &key, T const &value) noexcept
{
    try {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl) _impl = std::make_unique<Impl>();
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        _impl->data[key] = value;
        return true;
    } catch (...) {
        return false;
    }
}

template <typename T>
T JsonConfig::get(const std::string &key) const
{
    std::lock_guard<std::mutex> g(_initMutex);
    if (!_impl) throw std::runtime_error("JsonConfig::get: not initialized");

    std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
    auto it = _impl->data.find(key);
    if (it == _impl->data.end()) {
        throw std::runtime_error("JsonConfig::get: key not found: " + key);
    }
    try {
        return it->get<T>();
    } catch (const std::exception &ex) {
        throw std::runtime_error(std::string("JsonConfig::get: conversion failed for key ") + key + ": " + ex.what());
    }
}

template <typename T>
T JsonConfig::get_or(const std::string &key, T const &default_value) const noexcept
{
    try {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl) return default_value;
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        auto it = _impl->data.find(key);
        if (it == _impl->data.end()) return default_value;
        try {
            return it->get<T>();
        } catch (...) {
            return default_value;
        }
    } catch (...) {
        return default_value;
    }
}

inline bool JsonConfig::has(const std::string &key) const noexcept
{
    try {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl) return false;
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        return _impl->data.find(key) != _impl->data.end();
    } catch (...) {
        return false;
    }
}

inline bool JsonConfig::erase(const std::string &key) noexcept
{
    try {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl) return false;
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        auto it = _impl->data.find(key);
        if (it == _impl->data.end()) return false;
        _impl->data.erase(it);
        return true;
    } catch (...) {
        return false;
    }
}

template <typename Func>
bool JsonConfig::update(const std::string &key, Func &&updater) noexcept
{
    static_assert(std::is_invocable_v<Func, json &>, "update(Func) must be invocable as f(json&)");
    try {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl) _impl = std::make_unique<Impl>();
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        json &target = _impl->data[key]; // create if missing
        updater(target);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace pylabhub::fileutil
