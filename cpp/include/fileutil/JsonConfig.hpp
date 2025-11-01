#pragma once
// JsonConfig.hpp - header declaration and template methods for JsonConfig
// Place at: include/fileutil/JsonConfig.hpp
//
// Design summary:
//  - Thread-safe in-memory JSON with optional atomic on-disk persistence.
//  - In-process exclusive writer guard using pylabhub::util::AtomicGuard<int> (single-owner).
//  - Cross-process locking for disk writes via FileLock (non-blocking by default).
//  - with_json_write() uses try_acquire_if_zero() to enforce there is at most one exclusive writer.
//  - save() will fail if an exclusive with_json_write is active (conservative fail-fast policy).
//
// Important notes for developers:
//  - Avoid calling save() from inside with_json_write callback — save() refuses during an active
//  exclusive guard.
//  - with_json_write callback should be small and not block for long durations.
//  - Templates (with_json_*, get/set) remain in header; heavier OS code is in JsonConfig.cpp.

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "fileutil/FileLock.hpp"
#include "fileutil/PathUtil.hpp"
#include "util/AtomicGuard.hpp"

namespace pylabhub::fileutil
{

using json = nlohmann::json;

// Logging macros can be overridden before include.
#ifndef JC_LOG_ERROR
#include <iostream>
#define JC_LOG_ERROR(x)                                                                            \
    do                                                                                             \
    {                                                                                              \
        std::cerr << "[JsonConfig ERROR] " << x << std::endl;                                      \
    } while (0)
#endif
#ifndef JC_LOG_WARN
#define JC_LOG_WARN(x)                                                                             \
    do                                                                                             \
    {                                                                                              \
        std::cerr << "[JsonConfig WARN ] " << x << std::endl;                                      \
    } while (0)
#endif
#ifndef JC_LOG_INFO
#define JC_LOG_INFO(x)                                                                             \
    do                                                                                             \
    { /* no-op by default */                                                                       \
    } while (0)
#endif

class JsonConfig
{
  public:
    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile);
    ~JsonConfig();

    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    // initialize config (set path). If createIfMissing==true tries to create the file.
    bool init(const std::filesystem::path &configFile, bool createIfMissing = false);

    // persist to disk (non-blocking). Return false if lock not acquired or IO error.
    bool save() noexcept;

    // reload from disk (non-blocking). Return false if lock not acquired or IO error.
    bool reload() noexcept;

    // Return a copy of the in-memory json (thread-safe).
    json as_json() const noexcept;

    /// Atomically replace both the in-memory JSON and the on-disk file. Non-blocking lock.
    bool replace(const json &newData) noexcept;

    // Template helpers (header-only)
    template <typename F> bool with_json_read(F &&cb) const noexcept;
    template <typename F> bool with_json_write(F &&cb);

    template <typename T> std::optional<T> get_optional(const std::string &path) const noexcept;
    template <typename T> T get_or(const std::string &path, const T &def) const noexcept;
    template <typename T> T get(const std::string &path) const;
    template <typename T> void set(const std::string &path, T &&value);

    bool remove(const std::string &path) noexcept;
    bool has(const std::string &path) const noexcept;

  private:
    struct Impl
    {
        std::filesystem::path configPath;
        json data;
        mutable std::shared_mutex rwMutex; // protects data

        // exclusive_guard: 0 == none, 1 == exclusive writer active
        std::atomic<int> exclusive_guard{0};

        // optional: mark dirty to indicate deferred write requested while exclusive guard active
        bool dirty{false};
    };

    mutable std::mutex _initMutex; // guards _impl initialization and lifetime
    std::unique_ptr<Impl> _impl;

    // atomic write helper implemented in JsonConfig.cpp (platform-specific)
    static void atomic_write_json(const std::filesystem::path &target, const json &j);
};

// ---------------- Implementation (templates + inline helpers) ----------------

inline JsonConfig::JsonConfig() noexcept = default;
inline JsonConfig::JsonConfig(const std::filesystem::path &configFile)
{
    init(configFile, false);
}
inline JsonConfig::~JsonConfig() = default;
inline JsonConfig::JsonConfig(JsonConfig &&) noexcept = default;
inline JsonConfig &JsonConfig::operator=(JsonConfig &&) noexcept = default;

inline bool JsonConfig::remove(const std::string &path) noexcept
{
    try
    {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            return false;
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);

        size_t pos = 0;
        json *cur = &_impl->data;
        std::vector<std::string> keys;
        while (pos < path.size())
        {
            auto dot = path.find('.', pos);
            std::string key =
                (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
            keys.push_back(key);
            if (dot == std::string::npos)
                break;
            pos = dot + 1;
            if (!cur->contains(key) || !(*cur)[key].is_object())
                return false;
            cur = &(*cur)[key];
        }
        cur = &_impl->data;
        for (size_t i = 0; i + 1 < keys.size(); ++i)
            cur = &(*cur)[keys[i]];
        return cur->erase(keys.back()) > 0;
    }
    catch (...)
    {
        return false;
    }
}

inline bool JsonConfig::has(const std::string &path) const noexcept
{
    try
    {
        json j = as_json();
        size_t pos = 0;
        const json *cur = &j;
        while (pos < path.size())
        {
            auto dot = path.find('.', pos);
            std::string key =
                (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
            if (!cur->contains(key))
                return false;
            cur = &(*cur)[key];
            if (dot == std::string::npos)
                break;
            pos = dot + 1;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// with_json_read: executes cb(const json&) under shared (read) lock.
template <typename F> inline bool JsonConfig::with_json_read(F &&cb) const noexcept
{
    try
    {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            return false;
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        const json &ref = _impl->data;
        std::forward<F>(cb)(ref);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// with_json_write: tries to acquire a single-owner exclusive guard (0->1).
// - If another exclusive writer exists, returns false immediately.
// - If successful, executes cb(json&) under memory write lock.
// - Do NOT call save() from inside cb() - save() refuses while exclusive guard active.
template <typename F> inline bool JsonConfig::with_json_write(F &&cb)
{
    try
    {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            _impl = std::make_unique<Impl>();

        // Attach guard without increment; attempt test-and-set
        pylabhub::util::AtomicGuard<int> guard(&_impl->exclusive_guard, /*do_increment=*/false);
        int prev = 0;
        if (!guard.try_acquire_if_zero(prev))
        {
            JC_LOG_WARN("JsonConfig::with_json_write: exclusive guard busy (prev=" << prev << ")");
            return false;
        }

        // We now hold exclusive ownership; execute callback under write lock.
        std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
        std::forward<F>(cb)(_impl->data);

        // Optionally mark dirty if caller wants implicit save policy.
        // _impl->dirty = true;

        // guard will release on destruction (end of scope)
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// small helpers (copy-based)
template <typename T>
inline std::optional<T> JsonConfig::get_optional(const std::string &path) const noexcept
{
    try
    {
        json j = as_json();
        size_t pos = 0;
        const json *cur = &j;
        while (pos < path.size())
        {
            auto dot = path.find('.', pos);
            std::string key =
                (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
            if (!cur->contains(key))
                return std::nullopt;
            cur = &(*cur)[key];
            if (dot == std::string::npos)
                break;
            pos = dot + 1;
        }
        return cur->get<T>();
    }
    catch (...)
    {
        return std::nullopt;
    }
}

template <typename T>
inline T JsonConfig::get_or(const std::string &path, const T &def) const noexcept
{
    auto v = get_optional<T>(path);
    if (v)
        return *v;
    return def;
}

template <typename T> inline T JsonConfig::get(const std::string &path) const
{
    auto v = get_optional<T>(path);
    if (!v)
        throw std::runtime_error("JsonConfig::get: key not found or wrong type: " + path);
    return *v;
}

template <typename T> inline void JsonConfig::set(const std::string &path, T &&value)
{
    std::lock_guard<std::mutex> g(_initMutex);
    if (!_impl)
        _impl = std::make_unique<Impl>();
    std::unique_lock<std::shared_mutex> w(_impl->rwMutex);

    size_t pos = 0;
    json *cur = &_impl->data;
    while (true)
    {
        auto dot = path.find('.', pos);
        std::string key =
            (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
        if (dot == std::string::npos)
        {
            (*cur)[key] = std::forward<T>(value);
            break;
        }
        else
        {
            if (!cur->contains(key) || !(*cur)[key].is_object())
                (*cur)[key] = json::object();
            cur = &(*cur)[key];
            pos = dot + 1;
        }
    }
}

} // namespace pylabhub::fileutil
