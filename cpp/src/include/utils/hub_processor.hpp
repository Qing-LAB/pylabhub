#pragma once
/**
 * @file hub_processor.hpp
 * @brief hub::Processor — demand-driven data transform pipeline.
 *
 * The Processor reads from an input Queue, calls a user-provided handler to
 * transform the data, and writes the result to an output Queue.  It runs a
 * single process_thread_ that blocks on in_queue->read_acquire() (demand-driven,
 * not fixed-rate) and calls the handler synchronously.
 *
 * @par Typical usage
 * @code
 * auto in_q  = ShmQueue::from_consumer(std::move(dbc), sizeof(SensorData));
 * auto out_q = ShmQueue::from_producer(std::move(dbp), sizeof(ProcessedData));
 *
 * auto proc = Processor::create(*in_q, *out_q, {.overflow_policy = OverflowPolicy::Drop});
 * proc->set_process_handler<void, SensorData, void, ProcessedData>(
 *     [](auto& ctx) {
 *         ctx.output().value = ctx.input().value * 2.0;
 *         return true; // commit
 *     });
 * proc->start();
 * // ...
 * proc->stop();
 * @endcode
 *
 * @par Templates
 * Templates appear only in ProcessorContext<> and set_process_handler<>().
 * Queue construction and all Processor methods are non-template.
 *
 * See docs/HEP/HEP-CORE-0015-Processor-Binary.md for design rationale.
 */
#include "pylabhub_utils_export.h"
#include "utils/hub_queue.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace pylabhub::hub
{

// ============================================================================
// ProcessorContext — typed accessors over raw void* pointers
// ============================================================================

/**
 * @brief Typed view of one processing context passed to the handler.
 *
 * InFlexZoneT / OutFlexZoneT may be void to indicate "no flexzone".
 * The flexzone accessor methods are compile-time disabled (SFINAE) when
 * the corresponding template parameter is void.
 *
 * @tparam InFlexZoneT   Flexzone type for the input queue (void if unused).
 * @tparam InDataBlockT  Slot data type for the input queue.
 * @tparam OutFlexZoneT  Flexzone type for the output queue (void if unused).
 * @tparam OutDataBlockT Slot data type for the output queue.
 */
template <typename InFlexZoneT, typename InDataBlockT,
          typename OutFlexZoneT, typename OutDataBlockT>
struct ProcessorContext
{
    const void* _in_data;   ///< from in_queue->read_acquire()
    void*       _out_data;  ///< from out_queue->write_acquire()
    const void* _in_fz;     ///< from in_queue->read_flexzone()  (may be nullptr)
    void*       _out_fz;    ///< from out_queue->write_flexzone() (may be nullptr)

    // ── Data slot accessors ───────────────────────────────────────────────────

    [[nodiscard]] const InDataBlockT& input() const
    {
        return *static_cast<const InDataBlockT*>(_in_data);
    }

    [[nodiscard]] OutDataBlockT& output()
    {
        return *static_cast<OutDataBlockT*>(_out_data);
    }

    // ── Flexzone accessors (compile-time guarded) ─────────────────────────────

    template <typename F = InFlexZoneT>
    [[nodiscard]] auto in_flexzone() const noexcept
        -> std::enable_if_t<!std::is_void_v<F>, const F&>
    {
        return *static_cast<const InFlexZoneT*>(_in_fz);
    }

    template <typename F = OutFlexZoneT>
    [[nodiscard]] auto out_flexzone() noexcept
        -> std::enable_if_t<!std::is_void_v<F>, F&>
    {
        return *static_cast<OutFlexZoneT*>(_out_fz);
    }

    // ── Runtime flexzone availability checks ──────────────────────────────────

    /** Returns true when the input queue has a configured flexzone. */
    [[nodiscard]] bool has_in_flexzone()  const noexcept { return _in_fz  != nullptr; }
    /** Returns true when the output queue has a configured flexzone. */
    [[nodiscard]] bool has_out_flexzone() const noexcept { return _out_fz != nullptr; }
};

// ============================================================================
// Internal type-erased handler function type
// ============================================================================

/**
 * @brief Type-erased processing handler: in_data × in_fz → out_data × out_fz → bool.
 *
 * Returns true to commit the output slot; false to discard it.
 * This type lives in the pImpl layer; user code interacts via set_process_handler<>().
 */
using ProcessorHandlerFn =
    std::function<bool(const void* in_data, const void* in_fz,
                       void*       out_data, void*       out_fz)>;

/**
 * @brief Type-erased timeout handler: called when read_acquire() times out.
 *
 * Receives output slot data pointer and output flexzone pointer.
 * out_data may be nullptr when Drop policy is active and the output queue is full.
 * Return true to commit the output slot; false to discard it.
 */
using ProcessorTimeoutFn = std::function<bool(void* out_data, void* out_fz)>;

/**
 * @brief Pre-handler hook: called before every handler or timeout invocation.
 *
 * Useful for per-iteration setup such as GIL acquire, logging, or metrics.
 * Called on the process_thread_, after output slot acquisition.
 */
using ProcessorPreHookFn = std::function<void()>;

// ============================================================================
// ProcessorOptions
// ============================================================================

/** Configuration options for hub::Processor. */
struct ProcessorOptions
{
    /** Output overflow policy (Block = backpressure; Drop = skip when full). */
    OverflowPolicy            overflow_policy{OverflowPolicy::Block};

    /**
     * @brief Maximum time to wait for input slot in each loop iteration.
     *
     * After this timeout, the loop retries (checking stop_ flag).
     * Larger values mean slower reaction to stop().  Default: 5 seconds.
     */
    std::chrono::milliseconds input_timeout{std::chrono::milliseconds{5000}};

    /**
     * @brief Zero-fill output slot before calling the handler.
     *
     * When true, the output slot is memset to 0 after write_acquire() and
     * before the handler is invoked.  Ensures no stale data leaks through
     * if the handler only partially fills the output.  Default: false.
     */
    bool zero_fill_output{false};
};

// ============================================================================
// Processor
// ============================================================================

struct ProcessorImpl;

/**
 * @class Processor
 * @brief Demand-driven data transform pipeline: in_queue → handler → out_queue.
 *
 * Internally runs a single process_thread_ that:
 *   1. Blocks on in_queue->read_acquire(input_timeout).
 *   2. Acquires an output slot (Block or Drop policy).
 *   3. Calls the installed handler (typed via set_process_handler<>).
 *   4. Commits or discards the output slot based on the handler's return value.
 *   5. Releases the input slot.
 *
 * @par Ownership
 * The Processor holds non-owning references to the input and output queues.
 * The queues must outlive the Processor.
 */
class PYLABHUB_UTILS_EXPORT Processor
{
public:
    /**
     * @brief Create a Processor from an input and output Queue.
     *
     * @param in_queue   Queue to read from (must remain valid until stop()).
     * @param out_queue  Queue to write to (must remain valid until stop()).
     * @param opts       Configuration options.
     * @return           Processor on success; std::nullopt on invalid queues.
     */
    [[nodiscard]] static std::optional<Processor>
    create(QueueReader& in_queue, QueueWriter& out_queue, ProcessorOptions opts = {});

    ~Processor();
    Processor(Processor&&) noexcept;
    Processor& operator=(Processor&&) noexcept;
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    // ── Handler installation ──────────────────────────────────────────────────

    /**
     * @brief Install a typed processing handler.
     *
     * The handler receives a ProcessorContext<InF,InD,OutF,OutD> and returns
     * true to commit the output slot, or false to discard it.
     *
     * Passing nullptr (default-constructed std::function) removes the handler.
     * The process_thread_ idles (10ms sleep loop) when no handler is installed.
     *
     * Thread-safe: the handler is stored atomically and may be replaced while
     * the Processor is running.
     *
     * @tparam InF   Input flexzone type (void = no flexzone).
     * @tparam InD   Input slot data type.
     * @tparam OutF  Output flexzone type (void = no flexzone).
     * @tparam OutD  Output slot data type.
     * @param fn     Handler function; nullptr to remove.
     */
    template <typename InF, typename InD, typename OutF, typename OutD>
    void set_process_handler(
        std::function<bool(ProcessorContext<InF, InD, OutF, OutD>&)> fn)
    {
        if (!fn) { _store_handler(nullptr); return; }
        auto wrapped = std::make_shared<ProcessorHandlerFn>(
            [fn = std::move(fn)](const void* in_data, const void* in_fz,
                                 void*       out_data, void*       out_fz) -> bool {
                ProcessorContext<InF, InD, OutF, OutD> ctx{in_data, out_data, in_fz, out_fz};
                return fn(ctx);
            });
        _store_handler(std::move(wrapped));
    }

    /**
     * @brief Install a raw type-erased processing handler.
     *
     * For advanced use when the caller manages its own type casting.
     * Passing nullptr removes the handler.  Thread-safe.
     */
    void set_raw_handler(ProcessorHandlerFn fn);

    /** @brief Returns true if a processing handler is currently installed. */
    [[nodiscard]] bool has_process_handler() const noexcept;

    // ── Timeout handler ──────────────────────────────────────────────────────

    /**
     * @brief Install a handler called when read_acquire() times out.
     *
     * The timeout handler receives an output slot (which may be nullptr in
     * Drop mode when the output queue is full) and the output flexzone.
     * Return true to commit; false to discard.
     *
     * Passing a default-constructed (empty) function removes the handler.
     * Thread-safe: may be called while the Processor is running.
     */
    void set_timeout_handler(ProcessorTimeoutFn fn);

    // ── Pre-handler hook ─────────────────────────────────────────────────────

    /**
     * @brief Install a hook called before every handler/timeout invocation.
     *
     * Called on process_thread_ after output slot acquisition, before the
     * handler.  Useful for GIL acquire, per-iteration logging, or metrics.
     *
     * Passing a default-constructed (empty) function removes the hook.
     * Thread-safe: may be called while the Processor is running.
     */
    void set_pre_hook(ProcessorPreHookFn fn);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * @brief Start the Processor.
     *
     * Calls in_queue.start() + out_queue.start() (idempotent for ShmQueue),
     * then launches process_thread_.
     *
     * @return true on success; false if already running.
     */
    bool start();

    /**
     * @brief Stop the Processor.
     *
     * Sets the stop flag, waits for process_thread_ to exit, then calls
     * out_queue.stop() + in_queue.stop().  Safe to call multiple times.
     *
     * Maximum shutdown latency: opts.input_timeout (default 5 s).
     */
    void stop();

    [[nodiscard]] bool is_running()  const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── Counters ──────────────────────────────────────────────────────────────

    /** Total input slots received (read_acquire succeeded). */
    [[nodiscard]] uint64_t in_slots_received() const noexcept;
    /** Total output slots committed (handler returned true). */
    [[nodiscard]] uint64_t out_slots_written() const noexcept;
    /** Total slots dropped (no output slot available, or handler returned false). */
    [[nodiscard]] uint64_t out_drop_count()    const noexcept;

    /**
     * @brief Total loop iterations (incremented unconditionally each cycle).
     *
     * Advances on both normal (data received) and timeout iterations.
     * Useful for heartbeat liveness monitoring: if iteration_count stalls,
     * the loop is stuck.
     */
    [[nodiscard]] uint64_t iteration_count() const noexcept;

    // ── Critical error ───────────────────────────────────────────────────────

    /**
     * @brief Signal a critical error, causing the process loop to exit.
     *
     * May be called from the handler or timeout handler.  The reason string
     * is stored and the loop exits at the next iteration boundary.
     * Thread-safe: may be called from any thread.
     */
    void set_critical_error(std::string reason);

    /** @brief Returns true if a critical error has been signaled. */
    [[nodiscard]] bool has_critical_error() const noexcept;

    /** @brief Returns the critical error reason string (empty if none). */
    [[nodiscard]] std::string critical_error_reason() const;

    // ── Cleanup ───────────────────────────────────────────────────────────────

    /** Stop (if running) and release all resources. */
    void close();

private:
    explicit Processor(std::unique_ptr<ProcessorImpl> impl);
    std::unique_ptr<ProcessorImpl> pImpl;

    /** Internal: store handler into pImpl atomically. */
    void _store_handler(std::shared_ptr<ProcessorHandlerFn> h) noexcept;
};

} // namespace pylabhub::hub
