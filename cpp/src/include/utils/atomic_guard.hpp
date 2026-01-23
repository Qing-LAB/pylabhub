// AtomicGuard.hpp
#pragma once
// Header only for atomic guard

namespace pylabhub::basics
{

class AtomicGuard; // Forward-declaration

/**
 * @class AtomicOwner
 * @brief Represents the owner of a lock, holding the single, authoritative atomic state.
 *
 * Design Principle: This class enforces a "Walled Garden" approach. The lock state can
 * ONLY be manipulated by a friend class (`AtomicGuard`). This compiler-enforced rule
 * ensures a clean, cooperative framework where direct, un-guarded access is impossible.
 *
 * The lock state is represented by a 64-bit integer: 0 means "free", and any other
 * value is the unique token of the `AtomicGuard` that currently holds the lock.
 */
class AtomicOwner
{
  public:
    /** @brief Default constructor, initializes the lock to a free state (0). */
    AtomicOwner() noexcept : state_(0) {}

    /** @brief Checks if the lock is currently free (state is 0). */
    bool is_free() const noexcept { return state_.load(std::memory_order_acquire) == 0; }

    // Deleted copy/move operations, as an atomic state is a unique resource.
    AtomicOwner(const AtomicOwner &) = delete;
    AtomicOwner &operator=(const AtomicOwner &) = delete;
    AtomicOwner(AtomicOwner &&) noexcept = delete;
    AtomicOwner &operator=(AtomicOwner &&) noexcept = delete;

  private:
    friend class AtomicGuard; // Grant exclusive access to AtomicGuard

    // Private manipulation API, for `AtomicGuard` use only.
    uint64_t load() const noexcept { return state_.load(std::memory_order_acquire); }

    bool compare_exchange_strong(uint64_t &expected, uint64_t desired) noexcept
    {
        return state_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    std::atomic<uint64_t> state_;
};

#ifndef NDEBUG
#define DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN() debug_enter()
#define DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END()   debug_leave()
#else
#define DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN() ((void)0)
#define DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END()   ((void)0)
#endif

/**
 * @class AtomicGuard
 * @brief A move-only, RAII-style guard for acquiring and releasing a lock from an AtomicOwner.
 *
 * Design Principle: This guard is stateless. It holds no "belief" about whether it is
 * active. Its status is derived directly and authoritatively from the `AtomicOwner`.
 * This eliminates a class of bugs related to state desynchronization.
 *
 * The destructor provides an unconditionally safe, best-effort, atomic release.
 * Ownership can be transferred between guards using move semantics.
 */
class AtomicGuard
{
  public:
    /** @brief Default constructor. Creates a detached guard with a unique token. */
    AtomicGuard() noexcept : owner_(nullptr), token_(generate_token()) {}

    /**
     * @brief Constructs a guard and attaches it to an owner, optionally acquiring the lock.
     * @param owner The `AtomicOwner` to manage.
     * @param tryAcquire If `true`, the guard will attempt to acquire the lock upon construction.
     */
    explicit AtomicGuard(AtomicOwner *owner, bool tryAcquire = false) noexcept
        : owner_(owner), token_(generate_token())
    {
        if (tryAcquire)
        {
            (void)acquire();
        }
    }

    /**
     * @brief Destructor. Provides an unconditionally safe, best-effort RAII release.
     *
     * If this guard is the current owner of the lock, the lock will be released.
     * If not, this operation does nothing. It cannot panic or fail unexpectedly.
     */
    ~AtomicGuard() noexcept
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        if (owner_)
        {
            uint64_t expected = token_;
            // Atomically set owner to 0 IF AND ONLY IF the owner is us.
            // This is a single, safe, best-effort release.
            owner_->compare_exchange_strong(expected, 0);
        }
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
    }

    // Deleted copy operations, as a guard represents unique ownership.
    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;

    /**
     * @brief Move constructor. Transfers ownership of a lock from another guard.
     * @param other The source guard to move from. The source is left in a detached state.
     */
    AtomicGuard(AtomicGuard &&other) noexcept
        : owner_(other.owner_), token_(other.token_)
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        // Source is now detached and can no longer interact with the owner.
        other.owner_ = nullptr;
        // The source guard keeps its token, but it is now inert.
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
    }

    /**
     * @brief Move assignment operator.
     *
     * Releases the lock held by this guard (if any), then takes ownership from the source.
     * @param other The source guard to move from. The source is left in a detached state.
     */
    AtomicGuard &operator=(AtomicGuard &&other) noexcept
    {
        if (this == &other)
            return *this;

        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();

        // Best-effort release of our own lock, if we hold it.
        if (owner_)
        {
            uint64_t expected = token_;
            owner_->compare_exchange_strong(expected, 0);
        }

        // Take ownership from source.
        owner_ = other.owner_;
        token_ = other.token_;

        // Leave source detached.
        other.owner_ = nullptr;

        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();

        return *this;
    }

    /**
     * @brief Attempts to acquire the lock.
     * @return `true` if the lock was acquired, `false` otherwise.
     */
    [[nodiscard]] bool acquire() noexcept
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        bool result = false;
        if (owner_)
        {
            uint64_t expected = 0; // Acquire only if lock is free.
            result = owner_->compare_exchange_strong(expected, token_);
        }
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
        return result;
    }

    /**
     * @brief Attempts to release the lock.
     * @return `true` if the lock was successfully released, `false` otherwise.
     */
    [[nodiscard]] bool release() noexcept
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        bool result = false;
        if (owner_)
        {
            uint64_t expected = token_; // Release only if we are the owner.
            result = owner_->compare_exchange_strong(expected, 0);
        }
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
        return result;
    }

    /**
     * @brief Attaches the guard to an owner and then attempts to acquire the lock.
     * @param owner A pointer to the `AtomicOwner` to manage.
     * @return `true` if the lock was acquired successfully, `false` otherwise.
     */
    [[nodiscard]] bool attach_and_acquire(AtomicOwner *owner) noexcept
    {
        attach(owner);
        return acquire();
    }

    /**
     * @brief Authoritatively checks if this guard currently holds the lock.
     *
     * This method directly queries the `AtomicOwner`, providing the ground truth.
     * @return `true` if this guard's token is the one in the `AtomicOwner`.
     */
    bool active() const noexcept
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        bool result = false;
        if (owner_)
        {
            result = (owner_->load() == token_);
        }
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
        return result;
    }

    /**
     * @brief Returns the unique token for this guard.
     * This can be used as a handle for advanced coordination between guards.
     */
    uint64_t token() const noexcept
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        uint64_t result = token_;
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
        return result;
    }

    /**
     * @brief Attaches the guard to a new `AtomicOwner`.
     * @warning It is a logic error to call this on a guard that is currently active on
     *          another owner, as the original lock will be leaked.
     * @param owner The new `AtomicOwner` to manage.
     */
    void attach(AtomicOwner *owner) noexcept
    {
        // Note: This method intentionally does not use the DEBUG_ASSERT macros.
        // Its own internal logic check for active() needs to trigger a panic
        // for testing purposes, and the concurrent access check interferes with that.
#ifndef NDEBUG
        if (active())
        {
            PLH_PANIC("attach() called on an active guard. The original lock is now leaked. "
                      "Release the lock before attaching to a new owner.");
        }
#endif
        owner_ = owner;
    }

    /**
     * @brief Detaches the guard from its owner. The guard becomes inert.
     * The destructor will no longer release the lock for this guard.
     * @warning If called on an active guard, the lock is leaked.
     */
    void detach() noexcept
    {
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_BEGIN();
        owner_ = nullptr;
        DEBUG_ASSERT_NO_CONCURRENT_ACCESS_END();
    }

  private:
    /**
     * @brief Generates a new unique, non-zero token.
     */
    static uint64_t generate_token() noexcept
    {
        static std::atomic<uint64_t> next{1};
        uint64_t t;
        do
        {
            // Relaxed ordering is sufficient, as this just needs to be a unique number.
            t = next.fetch_add(1, std::memory_order_relaxed);
        } while (t == 0); // Must not be 0, which signifies "free".
        return t;
    }

    AtomicOwner *owner_;
    uint64_t token_;

#ifndef NDEBUG
    // Used to detect concurrent access to THIS AtomicGuard instance.
    // Guard objects themselves are not thread-safe.
    mutable std::atomic_flag access_flag_ = ATOMIC_FLAG_INIT;

    void debug_enter() const noexcept
    {
        if (access_flag_.test_and_set(std::memory_order_acquire))
        {
            PLH_PANIC("Concurrent access detected on a single AtomicGuard instance. "
                      "Guard objects must not be shared between threads without external locking.");
        }
    }

    void debug_leave() const noexcept { access_flag_.clear(std::memory_order_release); }
#endif
};

} // namespace pylabhub::basics
