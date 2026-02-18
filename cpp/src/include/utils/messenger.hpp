#pragma once
/**
 * @file messenger.hpp
 * @brief ZeroMQ-based async messaging with the central broker.
 *
 * Messenger is a singleton that manages communication with the broker via an
 * internal worker thread. Public methods are thread-safe.
 *
 * - register_producer / register_consumer: fire-and-forget (enqueue and return immediately).
 *   Errors are logged by the worker thread.
 * - discover_producer: enqueues a request and blocks until the broker responds.
 * - connect / disconnect: synchronous; guarded by internal mutex.
 */
#include "pylabhub_utils_export.h"

#include "utils/module_def.hpp"

#include <memory>
#include <optional>
#include <string>

namespace pylabhub::hub
{

class MessengerImpl;

struct ProducerInfo
{
    std::string shm_name;
    uint64_t producer_pid;
    std::string schema_hash;
    uint32_t schema_version;
};

struct ConsumerInfo
{
    std::string shm_name;
    std::string schema_hash;
    uint32_t schema_version;
};

/**
 * @class Messenger
 * @brief Manages communication with the central broker.
 *
 * All public methods are thread-safe. ZMQ socket access is single-threaded
 * (internal worker thread only). Async queue decouples callers from socket I/O.
 */
class PYLABHUB_UTILS_EXPORT Messenger
{
  public:
    Messenger();
    ~Messenger();

    Messenger(const Messenger &) = delete;
    Messenger &operator=(const Messenger &) = delete;
    Messenger(Messenger &&) noexcept;
    Messenger &operator=(Messenger &&) noexcept;

    /**
     * @brief Connects to the broker, starts the worker thread.
     * @return true if the connection was established successfully.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- endpoint and server_key are distinct
    [[nodiscard]] bool connect(const std::string &endpoint, const std::string &server_key);

    /**
     * @brief Closes the broker connection. Worker thread remains running.
     */
    void disconnect();

    /**
     * @brief Registers a producer (fire-and-forget). Errors logged by worker.
     */
    void register_producer(const std::string &channel, const ProducerInfo &info);

    /**
     * @brief Registers a consumer (stub; not yet implemented).
     */
    void register_consumer(const std::string &channel, const ConsumerInfo &info);

    /**
     * @brief Discovers a producer via the broker (synchronous: blocks on response).
     * @param timeout_ms Maximum time to wait for broker response (ms).
     * @return ConsumerInfo on success, or std::nullopt on timeout/error/not-connected.
     */
    [[nodiscard]] std::optional<ConsumerInfo> discover_producer(const std::string &channel,
                                                                 int timeout_ms = 5000);

    /**
     * @brief Returns the lifecycle-managed singleton Messenger instance.
     * @pre GetLifecycleModule() (DataExchangeHub) must be initialized via LifecycleGuard.
     */
    static Messenger &get_instance();

  private:
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<MessengerImpl> pImpl;
};

/**
 * @brief Returns true if the Data Exchange Hub lifecycle module is initialized.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool lifecycle_initialized() noexcept;

/**
 * @brief Factory function for the Data Exchange Hub lifecycle module.
 */
PYLABHUB_UTILS_EXPORT pylabhub::utils::ModuleDef GetLifecycleModule();

} // namespace pylabhub::hub
