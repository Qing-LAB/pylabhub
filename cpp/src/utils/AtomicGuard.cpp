// src/utils/AtomicGuard.cpp
#include "utils/AtomicGuard.hpp"

#include <cassert>
#include <fmt/core.h>
#include <mutex>
#include <string>

namespace pylabhub::utils
{

// --- Pimpl Definitions ---

// Implementation for AtomicOwner
struct AtomicOwnerImpl
{
    std::atomic<uint64_t> state_;

    AtomicOwnerImpl() noexcept : state_(0) {}
    explicit AtomicOwnerImpl(uint64_t initial) noexcept : state_(initial) {}
};

// Implementation for AtomicGuard
struct AtomicGuardImpl
{
    // Guard-local fields are atomic so that acquire()/release() remain lock-free.
    std::atomic<AtomicOwner *> owner_{nullptr};
    std::atomic<uint64_t> my_token_{0}; // non-zero after construction

    // Mutex used only for transfer_to / attach / detach to protect multi-field updates.
    mutable std::mutex guard_mtx_;

    // Flag set during destruction to prevent racing transfers. transfer_to checks
    // this flag and will fail if either guard is being destructed.
    std::atomic<bool> being_destructed_{false};

    // Flag to track if this guard believes it is the current owner. This is the
    // source of truth for whether the destructor should attempt a release.
    std::atomic<bool> is_active_{false};
};

// --- AtomicGuard Static Members ---

// Define the static member variable.
std::atomic<uint64_t> AtomicGuard::next_token_{1};

// Define the static token generator function.
uint64_t AtomicGuard::generate_token() noexcept
{
    uint64_t t;
    // Loop until a non-zero token is fetched. This handles the (extremely rare)
    // case where the atomic counter wraps around to zero.
    while ((t = next_token_.fetch_add(1, std::memory_order_relaxed)) == 0)
    {
        // This loop body is intentionally empty.
    }
    return t;
}

// --- AtomicOwner Method Implementations ---

AtomicOwner::AtomicOwner() noexcept : pImpl(std::make_unique<AtomicOwnerImpl>()) {}
AtomicOwner::AtomicOwner(uint64_t initial) noexcept
    : pImpl(std::make_unique<AtomicOwnerImpl>(initial))
{
}
AtomicOwner::~AtomicOwner() = default;
AtomicOwner::AtomicOwner(AtomicOwner &&) noexcept = default;
AtomicOwner &AtomicOwner::operator=(AtomicOwner &&) noexcept = default;

#ifndef NDEBUG
uint64_t AtomicOwner::load() const noexcept
{
    return pImpl->state_.load(std::memory_order_seq_cst);
}
void AtomicOwner::store(uint64_t v) noexcept
{
    pImpl->state_.store(v, std::memory_order_seq_cst);
}
#else
uint64_t AtomicOwner::load() const noexcept
{
    return pImpl->state_.load(std::memory_order_acquire);
}
void AtomicOwner::store(uint64_t v) noexcept
{
    pImpl->state_.store(v, std::memory_order_release);
}
#endif

bool AtomicOwner::compare_exchange_strong(uint64_t &expected, uint64_t desired) noexcept
{
    return pImpl->state_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
}

bool AtomicOwner::is_free() const noexcept
{
    return load() == 0;
}

std::atomic<uint64_t> &AtomicOwner::atomic_ref() noexcept
{
    return pImpl->state_;
}

const std::atomic<uint64_t> &AtomicOwner::atomic_ref() const noexcept
{
    return pImpl->state_;
}

// --- AtomicGuard Method Implementations ---

AtomicGuard::AtomicGuard() noexcept : pImpl(std::make_unique<AtomicGuardImpl>())
{
    uint64_t t = generate_token();
    pImpl->my_token_.store(t, std::memory_order_release);
    pImpl->owner_.store(nullptr, std::memory_order_release);
}

AtomicGuard::AtomicGuard(AtomicOwner *owner, bool tryAcquire) noexcept
    : pImpl(std::make_unique<AtomicGuardImpl>())
{
    uint64_t t = generate_token();
    pImpl->my_token_.store(t, std::memory_order_release);
    pImpl->owner_.store(owner, std::memory_order_release);
    if (tryAcquire && owner)
        (void)acquire(); // lightweight, lock-free attempt
}

AtomicGuard::~AtomicGuard() noexcept
{
    // A moved-from guard will have a null pImpl. Its destructor is a no-op.
    if (!pImpl)
    {
        return;
    }

    pImpl->being_destructed_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(pImpl->guard_mtx_);

    AtomicOwner *own = pImpl->owner_.load(std::memory_order_acquire);
    uint64_t tok = pImpl->my_token_.load(std::memory_order_acquire);

    if (!own || tok == 0)
        return;

    // If this guard does not believe it is the active owner, do nothing. This
    // correctly handles guards that were never active, were explicitly released,
    // or transferred ownership away.
    if (!pImpl->is_active_.load(std::memory_order_acquire))
    {
        return;
    }

    uint64_t expected = tok;
    if (own->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
    {
        pImpl->is_active_.store(false, std::memory_order_release);
        return; // Successfully released.
    }

    // Critical error: The guard believed it was the owner, but the underlying
    // resource state was unexpectedly changed by another entity.
    std::string err_msg =
        fmt::format("AtomicGuard(token={}): Invariant violation on destruction. "
                    "Guard was active but owner state was {} instead of self ({}).",
                    tok, tok, expected);
    PANIC(err_msg);
}

AtomicGuard::AtomicGuard(AtomicGuard &&) noexcept = default;
AtomicGuard &AtomicGuard::operator=(AtomicGuard &&) noexcept = default;

void AtomicGuard::attach(AtomicOwner *owner) noexcept
{
    std::lock_guard<std::mutex> lk(pImpl->guard_mtx_);
    pImpl->owner_.store(owner, std::memory_order_release);
}

void AtomicGuard::detach_no_release() noexcept
{
    std::lock_guard<std::mutex> lk(pImpl->guard_mtx_);
    pImpl->owner_.store(nullptr, std::memory_order_release);
}

bool AtomicGuard::acquire() noexcept
{
    AtomicOwner *own = pImpl->owner_.load(std::memory_order_acquire);
    if (!own)
        return false;

    uint64_t tok = pImpl->my_token_.load(std::memory_order_acquire);
    assert(tok != 0 && "my_token_ must be non-zero");

    uint64_t expected = 0;
    if (own->atomic_ref().compare_exchange_strong(expected, tok, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
    {
        pImpl->is_active_.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

bool AtomicGuard::release() noexcept
{
    AtomicOwner *own = pImpl->owner_.load(std::memory_order_acquire);
    if (!own)
        return false;

    uint64_t tok = pImpl->my_token_.load(std::memory_order_acquire);
    assert(tok != 0 && "my_token_ must be non-zero");

    uint64_t expected = tok;
    if (own->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
    {
        pImpl->is_active_.store(false, std::memory_order_release);
        return true;
    }
    return false;
}

bool AtomicGuard::attach_and_acquire(AtomicOwner *owner) noexcept
{
    std::lock_guard<std::mutex> lk(pImpl->guard_mtx_);
    pImpl->owner_.store(owner, std::memory_order_release);
    return acquire();
}

bool AtomicGuard::active() const noexcept
{
    AtomicOwner *own = pImpl->owner_.load(std::memory_order_acquire);
    if (!own)
        return false;
    uint64_t tok = pImpl->my_token_.load(std::memory_order_acquire);
    if (tok == 0)
        return false;
    return own->atomic_ref().load(std::memory_order_acquire) == tok;
}

uint64_t AtomicGuard::token() const noexcept
{
    return pImpl->my_token_.load(std::memory_order_acquire);
}

std::mutex &AtomicGuard::guard_mutex() const noexcept
{
    return pImpl->guard_mtx_;
}

bool AtomicGuard::transfer_to(AtomicGuard &dest) noexcept
{
    if (pImpl->being_destructed_.load(std::memory_order_acquire) ||
        dest.pImpl->being_destructed_.load(std::memory_order_acquire))
    {
        return false;
    }

    std::scoped_lock lk(pImpl->guard_mtx_, dest.pImpl->guard_mtx_);

    if (pImpl->being_destructed_.load(std::memory_order_acquire) ||
        dest.pImpl->being_destructed_.load(std::memory_order_acquire))
    {
        return false;
    }

    AtomicOwner *own = pImpl->owner_.load(std::memory_order_acquire);
    uint64_t mytok = pImpl->my_token_.load(std::memory_order_acquire);

    if (!own || mytok == 0)
        return false;

    AtomicOwner *d_own = dest.pImpl->owner_.load(std::memory_order_acquire);
    if (d_own && d_own != own)
        return false;

    uint64_t dest_tok = dest.pImpl->my_token_.load(std::memory_order_acquire);
    assert(dest_tok != 0 && "dest token must be non-zero");

    uint64_t expected = mytok;
    if (!own->atomic_ref().compare_exchange_strong(expected, dest_tok, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
    {
        return false;
    }

    dest.pImpl->owner_.store(own, std::memory_order_release);
    this->pImpl->is_active_.store(false, std::memory_order_release);
    dest.pImpl->is_active_.store(true, std::memory_order_release);

    return true;
}

} // namespace pylabhub::utils