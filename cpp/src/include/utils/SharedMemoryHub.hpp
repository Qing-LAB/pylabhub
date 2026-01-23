/*******************************************************************************
 * @file SharedMemoryHub.hpp
 * @brief Data Exchange Hub for inter-process communication.
 *
 * This module implements the Data Exchange Hub framework as specified in
 * HEP core-0002, providing high-performance shared memory channels and
 * ZeroMQ-based messaging for inter-process communication.
 *
 * @see docs/hep/hep-core-0002-data-exchange-hub-framework.md
 ******************************************************************************/

#pragma once

namespace pylabhub::hub
{

// Forward declarations for pImpl
class HubImpl;
class SharedMemoryProducerImpl;
class SharedMemoryConsumerImpl;
class ZmqPublisherImpl;
class ZmqSubscriberImpl;
class ZmqRequestServerImpl;
class ZmqRequestClientImpl;

// Forward declarations for public classes
class SharedMemoryProducer;
class SharedMemoryConsumer;
class ZmqPublisher;
class ZmqSubscriber;
class ZmqRequestServer;
class ZmqRequestClient;

/**
 * @brief Broker configuration for connecting to the Service Broker.
 */
struct PYLABHUB_UTILS_EXPORT BrokerConfig
{
    std::string endpoint;                 // e.g., "tcp://localhost:5555"
    std::string broker_public_key;        // CurveZMQ public key
    uint32_t heartbeat_interval_ms{5000}; // Heartbeat interval in milliseconds
};

/**
 * @brief Shared memory header structure for high-performance data exchange.
 *
 * This header is placed at the beginning of each shared memory segment and
 * contains synchronization primitives and metadata as specified in HEP core-0002.
 */
struct PYLABHUB_UTILS_EXPORT SharedMemoryHeader
{
    // Control Block: Synchronization primitives (platform-specific, opaque)
    alignas(64) char mutex_storage[64];     // Process-shared mutex
    alignas(64) char condition_storage[64]; // Process-shared condition variable
    alignas(64) char semaphore_storage[64]; // Process-shared semaphore

    // Atomic Flags: Lock-free state signaling
    std::atomic<uint64_t> frame_id{0};
    std::atomic<uint32_t> is_writing{0};
    std::atomic<uint32_t> data_ready{0};

    // Static Metadata Block: Performance-critical information
    double timestamp{0.0};
    uint64_t data_size{0};
    uint32_t data_type_hash{0};
    uint64_t dimensions[4]{0, 0, 0, 0};

    // Dynamic Metadata Region: Non-performance-critical metadata (JSON)
    static constexpr size_t DYNAMIC_METADATA_SIZE = 2048;
    char dynamic_metadata[DYNAMIC_METADATA_SIZE]{0};
};

/**
 * @brief The primary entry point for the Data Exchange Hub.
 *
 * This class manages the connection to the Service Broker, handles authentication
 * and heartbeats, and acts as a factory for creating communication channels.
 *
 * As specified in HEP core-0002, the Hub completely abstracts away broker
 * interactions, key management, and synchronization primitives.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT Hub
{
  public:
    /**
     * @brief Connects to the Service Broker and creates a Hub instance.
     *
     * This method handles authentication using CurveZMQ and starts a background
     * thread for sending periodic heartbeat messages to the broker.
     *
     * @param config Broker configuration including endpoint and public key.
     * @return A unique_ptr to the Hub instance, or nullptr on failure.
     */
    static std::unique_ptr<Hub> connect(const BrokerConfig &config);

    /**
     * @brief Destructor - disconnects from broker and cleans up resources.
     */
    ~Hub();

    // Non-copyable, movable
    Hub(const Hub &) = delete;
    Hub &operator=(const Hub &) = delete;
    Hub(Hub &&other) noexcept;
    Hub &operator=(Hub &&other) noexcept;

    // ========================================================================
    // High-Performance Channel (Shared Memory)
    // ========================================================================

    /**
     * @brief Creates a shared memory producer channel.
     *
     * This method creates a new shared memory segment, registers it with the
     * Service Broker, and returns a producer object for publishing data.
     *
     * @param name Unique name for the channel.
     * @param size Total size of the shared memory segment (must be >= sizeof(SharedMemoryHeader)).
     * @return A unique_ptr to the producer, or nullptr on failure.
     */
    std::unique_ptr<SharedMemoryProducer> create_shm_producer(const std::string &name, size_t size);

    /**
     * @brief Discovers and connects to an existing shared memory consumer channel.
     *
     * This method queries the Service Broker to find the channel, then opens
     * the shared memory segment for reading.
     *
     * @param name Name of the channel to discover.
     * @return A unique_ptr to the consumer, or nullptr if not found.
     */
    std::unique_ptr<SharedMemoryConsumer> find_shm_consumer(const std::string &name);

    // ========================================================================
    // General-Purpose Channel (ZeroMQ Messaging)
    // ========================================================================

    /**
     * @brief Creates a ZeroMQ publisher for one-to-many message distribution.
     *
     * The publisher is automatically registered with the Service Broker.
     *
     * @param service_name Name of the service/channel.
     * @return A unique_ptr to the publisher, or nullptr on failure.
     */
    std::unique_ptr<ZmqPublisher> create_publisher(const std::string &service_name);

    /**
     * @brief Discovers and connects to a ZeroMQ subscriber.
     *
     * @param service_name Name of the service/channel to subscribe to.
     * @return A unique_ptr to the subscriber, or nullptr if not found.
     */
    std::unique_ptr<ZmqSubscriber> find_subscriber(const std::string &service_name);

    /**
     * @brief Creates a ZeroMQ request server for command and control.
     *
     * The server is automatically registered with the Service Broker.
     *
     * @param service_name Name of the service.
     * @return A unique_ptr to the request server, or nullptr on failure.
     */
    std::unique_ptr<ZmqRequestServer> create_req_server(const std::string &service_name);

    /**
     * @brief Discovers and connects to a ZeroMQ request client.
     *
     * @param service_name Name of the service to connect to.
     * @return A unique_ptr to the request client, or nullptr if not found.
     */
    std::unique_ptr<ZmqRequestClient> find_req_client(const std::string &service_name);

  private:
    Hub();
    std::unique_ptr<HubImpl> pImpl;

    // Friend declarations for channel classes to access context
    friend class SharedMemoryProducer;
    friend class SharedMemoryConsumer;
    friend class ZmqPublisher;
    friend class ZmqSubscriber;
    friend class ZmqRequestServer;
    friend class ZmqRequestClient;

    // Friend declarations for Impl classes (implementation details)
    friend class HubImpl;
    friend class SharedMemoryProducerImpl;
    friend class SharedMemoryConsumerImpl;
    friend class ZmqPublisherImpl;
    friend class ZmqSubscriberImpl;
    friend class ZmqRequestServerImpl;
    friend class ZmqRequestClientImpl;

    // Helper method for channel classes to access ZeroMQ context
    void *get_context() const;
};

/**
 * @brief Producer for high-performance shared memory channels.
 *
 * Provides RAII-style management of shared memory publishing with transparent
 * synchronization handling.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT SharedMemoryProducer
{
  public:
    /**
     * @brief Destructor - cleans up shared memory resources.
     */
    ~SharedMemoryProducer();

    // Non-copyable, movable
    SharedMemoryProducer(const SharedMemoryProducer &) = delete;
    SharedMemoryProducer &operator=(const SharedMemoryProducer &) = delete;
    SharedMemoryProducer(SharedMemoryProducer &&other) noexcept;
    SharedMemoryProducer &operator=(SharedMemoryProducer &&other) noexcept;

    /**
     * @brief Begins a publishing operation.
     *
     * Locks the shared memory mutex and marks the segment as being written.
     * Returns a pointer to the data buffer for writing.
     *
     * @return Pointer to the data buffer, or nullptr on failure.
     */
    void *begin_publish();

    /**
     * @brief Ends a publishing operation.
     *
     * Updates metadata, increments frame_id, and signals consumers that data is ready.
     *
     * @param data_size Size of valid data written.
     * @param timestamp High-resolution timestamp.
     * @param data_type_hash Hash identifying the data type.
     * @param dimensions Array of up to 4 dimensions.
     */
    void end_publish(uint64_t data_size, double timestamp, uint32_t data_type_hash,
                     const uint64_t dimensions[4]);

    /**
     * @brief Gets the total size of the data buffer.
     */
    size_t buffer_size() const noexcept;

  private:
    friend class Hub;
    SharedMemoryProducer();
    std::unique_ptr<SharedMemoryProducerImpl> pImpl;
};

/**
 * @brief Consumer for high-performance shared memory channels.
 *
 * Provides RAII-style management of shared memory consumption with transparent
 * synchronization handling.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT SharedMemoryConsumer
{
  public:
    /**
     * @brief Destructor - cleans up shared memory resources.
     */
    ~SharedMemoryConsumer();

    // Non-copyable, movable
    SharedMemoryConsumer(const SharedMemoryConsumer &) = delete;
    SharedMemoryConsumer &operator=(const SharedMemoryConsumer &) = delete;
    SharedMemoryConsumer(SharedMemoryConsumer &&other) noexcept;
    SharedMemoryConsumer &operator=(SharedMemoryConsumer &&other) noexcept;

    /**
     * @brief Consumes the latest data from the shared memory segment.
     *
     * Waits for data to be ready (if needed), then returns a pointer to the
     * data buffer. The caller should check the header for metadata.
     *
     * @param timeout_ms Timeout in milliseconds (0 = wait indefinitely).
     * @return Pointer to the data buffer, or nullptr on timeout/failure.
     */
    const void *consume(uint32_t timeout_ms = 0);

    /**
     * @brief Gets a pointer to the shared memory header.
     */
    const SharedMemoryHeader *header() const noexcept;

    /**
     * @brief Gets the total size of the data buffer.
     */
    size_t buffer_size() const noexcept;

  private:
    friend class Hub;
    SharedMemoryConsumer();
    std::unique_ptr<SharedMemoryConsumerImpl> pImpl;
};

/**
 * @brief ZeroMQ publisher for one-to-many message distribution.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT ZmqPublisher
{
  public:
    /**
     * @brief Destructor - cleans up ZeroMQ resources.
     */
    ~ZmqPublisher();

    // Non-copyable, movable
    ZmqPublisher(const ZmqPublisher &) = delete;
    ZmqPublisher &operator=(const ZmqPublisher &) = delete;
    ZmqPublisher(ZmqPublisher &&other) noexcept;
    ZmqPublisher &operator=(ZmqPublisher &&other) noexcept;

    /**
     * @brief Publishes a message.
     *
     * @param topic Optional topic string for filtering.
     * @param data Message data.
     * @param size Size of message data.
     * @return True on success, false on failure.
     */
    bool publish(const char *topic, const void *data, size_t size);

  private:
    friend class Hub;
    ZmqPublisher();
    std::unique_ptr<ZmqPublisherImpl> pImpl;
};

/**
 * @brief ZeroMQ subscriber for receiving published messages.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT ZmqSubscriber
{
  public:
    /**
     * @brief Destructor - cleans up ZeroMQ resources.
     */
    ~ZmqSubscriber();

    // Non-copyable, movable
    ZmqSubscriber(const ZmqSubscriber &) = delete;
    ZmqSubscriber &operator=(const ZmqSubscriber &) = delete;
    ZmqSubscriber(ZmqSubscriber &&other) noexcept;
    ZmqSubscriber &operator=(ZmqSubscriber &&other) noexcept;

    /**
     * @brief Subscribes to a topic filter.
     *
     * @param topic_filter Topic filter string (empty = all messages).
     */
    void subscribe(const char *topic_filter = "");

    /**
     * @brief Receives a message (blocking).
     *
     * @param topic Output parameter for the topic string.
     * @param data Output parameter for the message data.
     * @param timeout_ms Timeout in milliseconds (0 = wait indefinitely).
     * @return True if message received, false on timeout/failure.
     */
    bool receive(std::string &topic, std::vector<uint8_t> &data, uint32_t timeout_ms = 0);

  private:
    friend class Hub;
    ZmqSubscriber();
    std::unique_ptr<ZmqSubscriberImpl> pImpl;
};

/**
 * @brief ZeroMQ request server for handling command requests.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT ZmqRequestServer
{
  public:
    /**
     * @brief Destructor - cleans up ZeroMQ resources.
     */
    ~ZmqRequestServer();

    // Non-copyable, movable
    ZmqRequestServer(const ZmqRequestServer &) = delete;
    ZmqRequestServer &operator=(const ZmqRequestServer &) = delete;
    ZmqRequestServer(ZmqRequestServer &&other) noexcept;
    ZmqRequestServer &operator=(ZmqRequestServer &&other) noexcept;

    /**
     * @brief Receives a request and sends a reply.
     *
     * @param request_data Received request data.
     * @param reply_data Reply data to send.
     * @param reply_size Size of reply data.
     * @param timeout_ms Timeout in milliseconds (0 = wait indefinitely).
     * @return True if request received and reply sent, false on timeout/failure.
     */
    bool handle_request(std::vector<uint8_t> &request_data, const void *reply_data,
                        size_t reply_size, uint32_t timeout_ms = 0);

  private:
    friend class Hub;
    ZmqRequestServer();
    std::unique_ptr<ZmqRequestServerImpl> pImpl;
};

/**
 * @brief ZeroMQ request client for sending command requests.
 *
 * This class uses the pImpl pattern for ABI stability.
 */
class PYLABHUB_UTILS_EXPORT ZmqRequestClient
{
  public:
    /**
     * @brief Destructor - cleans up ZeroMQ resources.
     */
    ~ZmqRequestClient();

    // Non-copyable, movable
    ZmqRequestClient(const ZmqRequestClient &) = delete;
    ZmqRequestClient &operator=(const ZmqRequestClient &) = delete;
    ZmqRequestClient(ZmqRequestClient &&other) noexcept;
    ZmqRequestClient &operator=(ZmqRequestClient &&other) noexcept;

    /**
     * @brief Sends a request and receives a reply.
     *
     * @param request_data Request data to send.
     * @param request_size Size of request data.
     * @param reply_data Received reply data.
     * @param timeout_ms Timeout in milliseconds (0 = wait indefinitely).
     * @return True if request sent and reply received, false on timeout/failure.
     */
    bool send_request(const void *request_data, size_t request_size,
                      std::vector<uint8_t> &reply_data, uint32_t timeout_ms = 5000);

  private:
    friend class Hub;
    ZmqRequestClient();
    std::unique_ptr<ZmqRequestClientImpl> pImpl;
};

/**
 * @brief Lifecycle module definition for the Data Exchange Hub.
 *
 * This function returns a ModuleDef that can be registered with the lifecycle
 * system. The module should be registered as a dynamic module.
 *
 * @return ModuleDef for registration.
 */
PYLABHUB_UTILS_EXPORT pylabhub::utils::ModuleDef GetLifecycleModule();

/**
 * @brief Checks if the Data Exchange Hub module has been initialized.
 * @return True if initialized, false otherwise.
 */
PYLABHUB_UTILS_EXPORT bool lifecycle_initialized() noexcept;

} // namespace pylabhub::hub
