#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe configuration holder with atomic on-disk writes and cross-process advisory locking.
 *
 * Design:
 *  - JsonConfig owns an in-memory `nlohmann::json` instance and manages its lifecycle.
 *  - All persistent I/O (reload/save/atomic writes) and FileLock usage are implemented
 *    in the dynamic library (.cpp) to preserve ABI stability.
 *  - Consumers manipulate the JSON inside short callbacks provided to `with_json_write` /
 *    `with_json_read`. These callbacks are executed under appropriate locks.
 *
 * Error handling:
 *  - Public API methods are noexcept-friendly and return bool. Optional diagnostics
 *    can be collected via an `std::error_code*` out-parameter (default nullptr).
 *
 * Threading & locking:
 *  - `initMutex` (std::mutex) serializes structural operations: init/reload/save and
 *    protects configPath and lifecycle.
 *  - `rwMutex` (std::shared_mutex) permits concurrent shared reads and exclusive writes
 *    to the in-memory JSON object.
 *
 * Usage example:
 *  JsonConfig cfg("/etc/myapp/config.json", true); // init and create if missing
 *  cfg.with_json_write([](nlohmann::json &j){
 *      j["port"] = 1234;
 *  });
 *
 *  cfg.with_json_read([](nlohmann::json const &j){
 *      auto host = j.value("host", std::string("localhost"));
 *  });
 */

#include <functional>
#include <memory>
#include <string>
#include <filesystem>
#include <system_error>

#include "nlohmann/json.hpp"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"
#include "recursion_guard.hpp"

namespace pylabhub::utils
{

/**
 * @class JsonConfig
 * @brief Process-safe, thread-safe manager for a JSON configuration file.
 *
 * The class provides:
 *  - Atomic on-disk writes (platform-aware, tmp-file + rename + fsync semantics).
 *  - Cross-process advisory locking via FileLock during I/O operations.
 *  - Scoped callback-based access to the in-memory JSON object.
 *
 * ABI-stability notes:
 *  - Implementation (Impl) is private and defined in the .cpp. The header does not expose implementation
 *    details, keeping the class size/layout stable across library versions.
 *
 * All public methods do not throw exceptions (errors reported using std::error_code optional out param).
 */
class PYLABHUB_UTILS_EXPORT JsonConfig
{
  public:
    /**
     * @brief Default construct an uninitialized JsonConfig instance.
     *
     * No file is opened; call init() to bind to a concrete config file.
     * This constructor is noexcept.
     */
    JsonConfig() noexcept;

    /**
     * @brief Construct and initialize with a config path.
     * @param configFile path to the config file
     * @param createIfMissing if true, create an empty JSON file if missing
     * @param ec optional pointer to std::error_code to receive diagnostic information (nullable)
     *
     * This constructor will attempt to initialize and, if requested, create the file.
     */
    explicit JsonConfig(const std::filesystem::path &configFile, bool createIfMissing = false,
                        std::error_code *ec = nullptr);

    /**
     * @brief Destructor (defined in .cpp where Impl is complete).
     *
     * Ensures any owned resources are cleaned up. Non-throwing.
     */
    ~JsonConfig();

    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    /**
     * @brief Initialize or reinitialize the config instance with a file path.
     * @param configFile path to the config file
     * @param createIfMissing if true, create an empty JSON file if missing
     * @param ec optional pointer to std::error_code to receive diagnostic information (nullable)
     * @return true on success, false on failure (see ec if provided)
     *
     * Threading: acquires init-lock to serialize structural operations.
     */
    bool init(const std::filesystem::path &configFile, bool createIfMissing = false,
              std::error_code *ec = nullptr);

    /**
     * @brief Force reload from disk into memory.
     * @param ec optional pointer to std::error_code for diagnostics
     * @return true on success, false on failure
     *
     * This acquires a FileLock (advisory) to ensure we read a consistent file.
     */
    bool reload(std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Force save current in-memory JSON to disk atomically.
     * @param ec optional pointer to std::error_code for diagnostics
     * @return true on success, false on failure
     *
     * The function snapshots the in-memory JSON under lock and performs an
     * atomic on-disk replace (tmp file + rename + fsync) while holding
     * the structural lock.
     */
    bool save(std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Provide exclusive access to the JSON (modify and save).
     * @param fn callback accepting nlohmann::json& to modify the config.
     * @param ec optional pointer to std::error_code for diagnostics
     * @return true on success (including save), false on failure
     *
     * This function:
     *  - prevents recursive reentry on the same instance/thread,
     *  - acquires the initMutex, then the exclusive json lock, calls the callback,
     *    marks the object dirty, and then persists to disk.
     *
     * This is implemented as a non-template (std::function) entry point to keep ABI stable.
     */
    bool with_json_write_impl(std::function<void(nlohmann::json &)> fn,
                              std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Provide shared read-only access to the JSON.
     * @param fn callback accepting nlohmann::json const&
     * @param ec optional pointer to std::error_code for diagnostics
     * @return true on success, false on failure
     */
    bool with_json_read_impl(std::function<void(nlohmann::json const &)> fn,
                             std::error_code *ec = nullptr) const noexcept;

    /**
     * @brief Header-only convenience: accept any callable convertible to std::function.
     * These inline templates are thin adapters and call the non-template impl methods above.
     */
    template <typename F>
    bool with_json_write(F &&fn, std::error_code *ec = nullptr) noexcept
    {
        try
        {
            std::function<void(nlohmann::json &)> cb = std::forward<F>(fn);
            return with_json_write_impl(std::move(cb), ec);
        }
        catch (...)
        {
            if (ec) *ec = std::make_error_code(std::errc::operation_canceled);
            return false;
        }
    }

    template <typename F>
    bool with_json_read(F &&fn, std::error_code *ec = nullptr) const noexcept
    {
        try
        {
            std::function<void(nlohmann::json const &)> cb = std::forward<F>(fn);
            return with_json_read_impl(std::move(cb), ec);
        }
        catch (...)
        {
            if (ec) *ec = std::make_error_code(std::errc::operation_canceled);
            return false;
        }
    }

    /**
     * @brief Check whether this instance has been initialized with a config path.
     * @return true if initialized
     */
    bool is_initialized() const noexcept;

    /**
     * @brief Return the configured path (may be empty).
     * @return copy of the path
     */
    std::filesystem::path config_path() const noexcept;

  private:
    // Implementation hidden (complete definition is in JsonConfig.cpp).
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // Platform-aware, atomic write helper used internally.
    // Declared here for clarity but defined in the .cpp; not part of public API.
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j,
                                  std::error_code *ec = nullptr);
};

} // namespace pylabhub::utils
