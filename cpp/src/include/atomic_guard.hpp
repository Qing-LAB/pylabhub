// AtomicGuard.hpp
#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

namespace pylabhub::basics
{

/*******************************************************************************
 * Minimal header-only AtomicOwner + AtomicGuard
 *
 * Design & semantics highlights:
 * - No internal mutex.
 * - Move-only guard semantics: move ctor/assign transfer ownership state.
 * - After a move, the source is left valid, detached, inactive, AND receives
 *   a fresh token so tokens remain unique.
 * - `active()` is authoritative: it checks the AtomicOwner's current token.
 *
 * Thread-safety contract:
 * - It is UB to concurrently access the *same* AtomicGuard object from multiple
 *   threads without external synchronization. Moving a guard must not race with
 *   other operations on that same guard object.
 ******************************************************************************/

/**
 * @class AtomicOwner
 * @brief Represents the owner of a lock, holding the atomic state.
 *
 * This class encapsulates an atomic 64-bit integer that represents the lock state.
 * A value of 0 indicates the lock is free. Any other value is a token representing
 * the `AtomicGuard` that currently holds the lock.
 */
class AtomicOwner
{
  public:
    /** @brief Default constructor, initializes the lock to a free state (0). */
    AtomicOwner() noexcept : state_(0) {}
    /** @brief Constructor to initialize the lock with a specific value. */
    explicit AtomicOwner(uint64_t initial) noexcept : state_(initial) {}

    AtomicOwner(const AtomicOwner &) = delete;
    AtomicOwner &operator=(const AtomicOwner &) = delete;
    AtomicOwner(AtomicOwner &&) noexcept = delete;            // atomic state cannot be moved
    AtomicOwner &operator=(AtomicOwner &&) noexcept = delete; // atomic state cannot be moved

    /** @brief Atomically loads the current state (token). */
    uint64_t load() const noexcept { return state_.load(std::memory_order_acquire); }
    /** @brief Atomically stores a new state (token). */
    void store(uint64_t v) noexcept { state_.store(v, std::memory_order_release); }

    /**
     * @brief Atomically compares the current state with an expected value and, if they match,
     * replaces the state with a desired value.
     * @param expected A reference to the value expected to be found in the state.
     * @param desired The value to store if the comparison is successful.
     * @return `true` if the exchange was successful, `false` otherwise.
     */
    bool compare_exchange_strong(uint64_t &expected, uint64_t desired) noexcept
    {
        return state_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    /** @brief Checks if the lock is currently free (state is 0). */
    bool is_free() const noexcept { return load() == 0; }

    /** @brief Returns a mutable reference to the underlying atomic state. */
    std::atomic<uint64_t> &atomic_ref() noexcept { return state_; }

    /** @brief Returns a const reference to the underlying atomic state. */
    const std::atomic<uint64_t> &atomic_ref() const noexcept { return state_; }

  private:
    std::atomic<uint64_t> state_;
};

/**
 * @class AtomicGuard
 * @brief A move-only, RAII-style guard for acquiring and releasing a lock from an AtomicOwner.
 *
 * An `AtomicGuard` attempts to acquire a lock by writing its unique token into an `AtomicOwner`.
 * The lock is held as long as the guard object exists and is active. The lock is automatically
 * released when the guard is destroyed. Ownership of the lock can be transferred between
 * guards using move semantics.
 */
class AtomicGuard
{
  public:
    /** @brief Default constructor. Creates a detached guard with a unique token. */
    AtomicGuard() noexcept : owner_(nullptr), token_(generate_token()), is_active_(false) {}

    /**
     * @brief Constructs a guard and attaches it to an owner.
     * @param owner A pointer to the `AtomicOwner` to manage.
     * @param tryAcquire If `true`, the guard will attempt to acquire the lock upon construction.
     */
    explicit AtomicGuard(AtomicOwner *owner, bool tryAcquire = false) noexcept
        : owner_(owner), token_(generate_token()), is_active_(false)
    {
        if (tryAcquire && owner_)
            (void)acquire();
    }

    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;

    /**
     * @brief Move constructor. Transfers ownership of a lock from another guard.
     * @param o The source guard to move from. The source guard is left in a detached,
     *          inactive state with a new unique token.
     */
    AtomicGuard(AtomicGuard &&o) noexcept
        : owner_(o.owner_), token_(o.token_), is_active_(o.is_active_)
    {
        // detach source and give it a fresh token
        o.owner_ = nullptr;
        o.is_active_ = false;
        o.token_ = generate_token();
    }

    /**
     * @brief Move assignment operator. Releases any active lock held by this guard and
     * then takes ownership of the lock from the source guard.
     * @param o The source guard to move from. The source guard is left in a detached,
     *          inactive state with a new unique token.
     */
    AtomicGuard &operator=(AtomicGuard &&o) noexcept
    {
        if (this == &o)
            return *this;

        // Release our own active lock (best-effort) if held.
        if (is_active_ && owner_ != nullptr)
        {
            uint64_t expected = token_;
            owner_->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire);
            is_active_ = false;
        }

        // Take ownership from source
        owner_ = o.owner_;
        token_ = o.token_;
        is_active_ = o.is_active_;

        // Leave source detached/inactive and give it a fresh token
        o.owner_ = nullptr;
        o.is_active_ = false;
        o.token_ = generate_token();

        return *this;
    }

    /**
     * @brief Destructor. Automatically releases the lock if it is active.
     *
     * This is a best-effort release. It will only release the lock if the `AtomicOwner`'s
     * token matches this guard's token, preventing accidental release of a lock
     * acquired by another guard.
     */
    ~AtomicGuard() noexcept
    {
        // Best-effort release ONLY IF we believe we hold the lock.
        if (!is_active_ || !owner_)
        {
            return;
        }

        // Compare owner state to token; if it matches, attempt CAS -> 0.
        uint64_t expected = token_;
        if (owner_->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
        {
            is_active_ = false;
            return;
        }

#ifndef NDEBUG
        // In debug builds assert, to catch logic errors early.
        // If we get here, it means is_active_ was true, but the owner's token
        // did not match ours. This is a true invariant violation.
        assert(false && "AtomicGuard destructor: invariant violation (is_active_ is true, but "
                        "owner has different token).");
#endif
        // In release builds, be robust: do nothing (the lock is effectively leaked).
    }

    /** @brief Attaches the guard to a new `AtomicOwner`. Does not acquire the lock. */
    void attach(AtomicOwner *owner) noexcept { owner_ = owner; }

    /** @brief Detaches the guard from its owner without releasing the lock. The guard becomes
     * inactive. */
    void detach_no_release() noexcept
    {
        owner_ = nullptr;
        is_active_ = false;
    }

    /**
     * @brief Attempts to acquire the lock.
     * @return `true` if the lock was acquired successfully, `false` otherwise.
     */
    [[nodiscard]] bool acquire() noexcept
    {
        if (!owner_)
            return false;
        uint64_t expected = 0;
        if (owner_->atomic_ref().compare_exchange_strong(
                expected, token_, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            is_active_ = true;
            return true;
        }
        return false;
    }

    /**
     * @brief Attempts to release the lock.
     *
     * The release is only successful if this guard currently holds the lock (i.e., if the
     * `AtomicOwner`'s state matches this guard's token).
     * @return `true` if the lock was released successfully, `false` otherwise.
     */
    [[nodiscard]] bool release() noexcept
    {
        if (!owner_)
            return false;
        uint64_t expected = token_;
        if (owner_->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
        {
            is_active_ = false;
            return true;
        }
        return false;
    }

    /**
     * @brief Attaches the guard to an owner and then attempts to acquire the lock.
     * @param owner A pointer to the `AtomicOwner` to manage.
     * @return `true` if the lock was acquired successfully, `false` otherwise.
     */
    [[nodiscard]] bool attach_and_acquire(AtomicOwner *owner) noexcept
    {
        owner_ = owner;
        return acquire();
    }

    /**
     * @brief Authoritatively checks if this guard currently holds the lock.
     *
     * This method directly queries the `AtomicOwner`'s state, rather than relying on
     * the guard's internal `is_active_` flag, providing the most up-to-date status.
     * @return `true` if the `AtomicOwner`'s token matches this guard's token, `false` otherwise.
     */
    bool active() const noexcept
    {
        if (!owner_)
            return false;
        return owner_->load() == token_;
    }

    /** @brief Returns the unique token associated with this guard. */
    uint64_t token() const noexcept { return token_; }

  private:
    /** @brief Generates a new unique, non-zero token. */
    static uint64_t generate_token() noexcept
    {
        static std::atomic<uint64_t> next{1};
        uint64_t t;
        while ((t = next.fetch_add(1, std::memory_order_relaxed)) == 0)
        {
            // wrap-around guard (extremely unlikely)
        }
        return t;
    }

    // Members
    AtomicOwner *owner_;
    uint64_t token_;
    // `is_active_` is a plain `bool` because the AtomicGuard object itself is not designed
    // for concurrent access by multiple threads. The class contract states: "It is UB to
    // concurrently access the *same* AtomicGuard object from multiple threads without
    // external synchronization." Therefore, accesses to `is_active_` do not need to be
    // atomic, as they occur within an execution path that has exclusive access to
    // this specific AtomicGuard instance.
    bool is_active_;
};

} // namespace pylabhub::basics
