// src/utils/hub/hub_processor.cpp
/**
 * @file hub_processor.cpp
 * @brief hub::Processor implementation.
 *
 * Single process_thread_ loop: read from in_queue → call handler → write to out_queue.
 * Demand-driven (no fixed rate); blocks on in_queue->read_acquire(input_timeout).
 *
 * The type-erased ProcessorHandlerFn is stored via PortableAtomicSharedPtr for
 * safe hot-swap from any thread while the Processor is running.
 */
#include "utils/hub_processor.hpp"
#include "utils/logger.hpp"
#include "portable_atomic_shared_ptr.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace pylabhub::hub
{

// ============================================================================
// ProcessorImpl — internal state
// ============================================================================

struct ProcessorImpl
{
    QueueReader*     in_queue{nullptr};
    QueueWriter*     out_queue{nullptr};
    ProcessorOptions opts;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread       process_thread_;

    // Hot-swappable handler; null = idle (no processing).
    pylabhub::utils::detail::PortableAtomicSharedPtr<ProcessorHandlerFn> handler_;

    // Timeout handler + pre-hook (opt-in).
    pylabhub::utils::detail::PortableAtomicSharedPtr<ProcessorTimeoutFn> timeout_handler_;
    pylabhub::utils::detail::PortableAtomicSharedPtr<ProcessorPreHookFn> pre_hook_;

    std::atomic<uint64_t> in_received_{0};
    std::atomic<uint64_t> out_written_{0};
    std::atomic<uint64_t> out_drops_{0};
    std::atomic<uint64_t> iteration_count_{0};

    // Critical error state.
    std::atomic<bool> critical_error_{false};
    std::string       critical_error_reason_;
    std::mutex        critical_error_mu_;

    // ── Main loop ─────────────────────────────────────────────────────────────
    void run_process_thread_();
};

// ============================================================================
// ProcessorImpl::run_process_thread_
// ============================================================================

void ProcessorImpl::run_process_thread_()
{
    const bool   drop_mode  = (opts.overflow_policy == OverflowPolicy::Drop);
    // Output timeout: Drop = non-blocking; Block = same as input_timeout so the
    // loop stays responsive to shutdown / control messages.
    const auto   out_timeout = drop_mode ? std::chrono::milliseconds{0}
                                         : opts.input_timeout;
    const size_t out_item_sz = out_queue->item_size();

    while (!stop_.load(std::memory_order_relaxed) &&
           !critical_error_.load(std::memory_order_relaxed))
    {
        ++iteration_count_;

        // ── Load handler (hot-swappable) ─────────────────────────────────
        auto h = handler_.load(std::memory_order_acquire);
        if (!h)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }

        // ── 1. Block until input arrives (or timeout) ────────────────────
        const void* in_data = in_queue->read_acquire(opts.input_timeout);

        if (!in_data)
        {
            // ── TIMEOUT PATH ─────────────────────────────────────────────
            auto th = timeout_handler_.load(std::memory_order_acquire);
            if (!th)
                continue; // no timeout handler — just retry

            void* out_data = out_queue->write_acquire(out_timeout);
            if (out_data && opts.zero_fill_output)
                std::memset(out_data, 0, out_item_sz);

            void* out_fz = out_queue->write_flexzone();

            auto ph = pre_hook_.load(std::memory_order_acquire);
            if (ph)
            {
                try { (*ph)(); }
                catch (...) {}
            }

            bool commit = false;
            try
            {
                commit = (*th)(out_data, out_fz);
            }
            catch (const std::exception& e)
            {
                LOGGER_ERROR("[hub::Processor] Timeout handler threw: {}", e.what());
                commit = false;
            }
            catch (...)
            {
                LOGGER_ERROR("[hub::Processor] Timeout handler threw unknown exception");
                commit = false;
            }

            if (out_data)
            {
                if (commit)
                {
                    out_queue->write_commit();
                    ++out_written_;
                }
                else
                {
                    out_queue->write_discard();
                    ++out_drops_;
                }
            }
            continue;
        }

        // ── NORMAL PATH ──────────────────────────────────────────────────
        ++in_received_;

        if (stop_.load(std::memory_order_relaxed) ||
            critical_error_.load(std::memory_order_relaxed))
        {
            in_queue->read_release();
            break;
        }

        // ── 2. Acquire output slot (Drop=non-blocking, Block=wait) ───────
        void* out_data = out_queue->write_acquire(out_timeout);
        if (!out_data)
        {
            ++out_drops_;
            in_queue->read_release();
            LOGGER_WARN("[hub::Processor] Output full — dropped input slot");
            continue;
        }

        if (opts.zero_fill_output)
            std::memset(out_data, 0, out_item_sz);

        // ── 3. Get flexzone pointers (nullptr if not configured) ─────────
        const void* in_fz  = in_queue->read_flexzone();
        void*       out_fz = out_queue->write_flexzone();

        // ── 4. Pre-hook ──────────────────────────────────────────────────
        auto ph = pre_hook_.load(std::memory_order_acquire);
        if (ph)
        {
            try { (*ph)(); }
            catch (...) {}
        }

        // ── 5. Call handler — pure C++, no GIL, no Python ────────────────
        bool commit = false;
        try
        {
            commit = (*h)(in_data, in_fz, out_data, out_fz);
        }
        catch (const std::exception& e)
        {
            LOGGER_ERROR("[hub::Processor] Handler threw: {}", e.what());
            commit = false;
        }
        catch (...)
        {
            LOGGER_ERROR("[hub::Processor] Handler threw unknown exception");
            commit = false;
        }

        // ── 6. Commit or discard output; release input ───────────────────
        if (commit)
        {
            out_queue->write_commit();
            ++out_written_;
        }
        else
        {
            out_queue->write_discard();
            ++out_drops_;
        }

        in_queue->read_release();
    }
}

// ============================================================================
// Processor::create
// ============================================================================

std::optional<Processor>
Processor::create(QueueReader& in_queue, QueueWriter& out_queue, ProcessorOptions opts)
{
    auto impl          = std::make_unique<ProcessorImpl>();
    impl->in_queue     = &in_queue;
    impl->out_queue    = &out_queue;
    impl->opts         = std::move(opts);
    return Processor(std::move(impl));
}

// ============================================================================
// Constructor / destructor / move
// ============================================================================

Processor::Processor(std::unique_ptr<ProcessorImpl> impl) : pImpl(std::move(impl)) {}

Processor::~Processor()
{
    close();
}

Processor::Processor(Processor&&) noexcept            = default;
Processor& Processor::operator=(Processor&&) noexcept = default;

// ============================================================================
// Handler management
// ============================================================================

void Processor::_store_handler(std::shared_ptr<ProcessorHandlerFn> h) noexcept
{
    if (pImpl)
        pImpl->handler_.store(std::move(h), std::memory_order_release);
}

void Processor::set_raw_handler(ProcessorHandlerFn fn)
{
    if (!fn)
    {
        _store_handler(nullptr);
        return;
    }
    _store_handler(std::make_shared<ProcessorHandlerFn>(std::move(fn)));
}

bool Processor::has_process_handler() const noexcept
{
    return pImpl && pImpl->handler_.load(std::memory_order_relaxed) != nullptr;
}

// ============================================================================
// Timeout handler + pre-hook
// ============================================================================

void Processor::set_timeout_handler(ProcessorTimeoutFn fn)
{
    if (!pImpl)
        return;
    if (!fn)
    {
        pImpl->timeout_handler_.store(nullptr, std::memory_order_release);
        return;
    }
    pImpl->timeout_handler_.store(
        std::make_shared<ProcessorTimeoutFn>(std::move(fn)),
        std::memory_order_release);
}

void Processor::set_pre_hook(ProcessorPreHookFn fn)
{
    if (!pImpl)
        return;
    if (!fn)
    {
        pImpl->pre_hook_.store(nullptr, std::memory_order_release);
        return;
    }
    pImpl->pre_hook_.store(
        std::make_shared<ProcessorPreHookFn>(std::move(fn)),
        std::memory_order_release);
}

// ============================================================================
// Critical error
// ============================================================================

void Processor::set_critical_error(std::string reason)
{
    if (!pImpl)
        return;
    {
        std::lock_guard<std::mutex> lk(pImpl->critical_error_mu_);
        pImpl->critical_error_reason_ = std::move(reason);
    }
    pImpl->critical_error_.store(true, std::memory_order_release);
}

bool Processor::has_critical_error() const noexcept
{
    return pImpl && pImpl->critical_error_.load(std::memory_order_acquire);
}

std::string Processor::critical_error_reason() const
{
    if (!pImpl)
        return {};
    std::lock_guard<std::mutex> lk(pImpl->critical_error_mu_);
    return pImpl->critical_error_reason_;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool Processor::start()
{
    if (!pImpl)
        return false;
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return false; // already running

    // Start queues (idempotent — no-op if already running).
    if (!pImpl->in_queue->start() || !pImpl->out_queue->start())
    {
        pImpl->running_.store(false, std::memory_order_release);
        return false;
    }
    pImpl->in_queue->reset_metrics();
    pImpl->out_queue->reset_metrics();

    pImpl->stop_.store(false, std::memory_order_release);
    pImpl->process_thread_ = std::thread([this] { pImpl->run_process_thread_(); });
    return true;
}

void Processor::stop()
{
    if (!pImpl)
        return;
    if (!pImpl->running_.load(std::memory_order_acquire))
        return;

    pImpl->stop_.store(true, std::memory_order_release);

    if (pImpl->process_thread_.joinable())
        pImpl->process_thread_.join();

    pImpl->out_queue->stop();
    pImpl->in_queue->stop();

    pImpl->running_.store(false, std::memory_order_release);
}

bool Processor::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

bool Processor::is_stopping() const noexcept
{
    return pImpl && pImpl->stop_.load(std::memory_order_relaxed);
}

// ============================================================================
// Counters
// ============================================================================

uint64_t Processor::in_slots_received() const noexcept
{
    return pImpl ? pImpl->in_received_.load(std::memory_order_relaxed) : 0;
}

uint64_t Processor::out_slots_written() const noexcept
{
    return pImpl ? pImpl->out_written_.load(std::memory_order_relaxed) : 0;
}

uint64_t Processor::out_drop_count() const noexcept
{
    return pImpl ? pImpl->out_drops_.load(std::memory_order_relaxed) : 0;
}

uint64_t Processor::iteration_count() const noexcept
{
    return pImpl ? pImpl->iteration_count_.load(std::memory_order_relaxed) : 0;
}

// ============================================================================
// close()
// ============================================================================

void Processor::close()
{
    stop();
    // pImpl is kept alive (metrics accessible after stop) — caller destroys us.
}

} // namespace pylabhub::hub
