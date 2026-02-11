#ifndef PYLABHUB_SLOT_RW_ACCESS_HPP
#define PYLABHUB_SLOT_RW_ACCESS_HPP

#include <functional>
#include <stdexcept>
#include <type_traits> // For std::is_trivially_copyable_v
#include <utility>     // For std::forward

#include "slot_rw_coordinator.h" // For SlotRWState, SlotAcquireResult, slot_acquire_result_string

namespace pylabhub
{

class SlotRWAccess
{
  public:
    // === Type-Safe Write Access ===
    template <typename T, typename Func>
    static auto with_typed_write(pylabhub::hub::SlotRWState *rw_state, void *buffer,
                                 size_t buffer_size, Func &&func, int timeout_ms = 100)
        -> std::invoke_result_t<Func, T &>
    {
        // Compile-time checks
        static_assert(std::is_trivially_copyable_v<T>,
                      "Type T must be trivially copyable for shared memory");

        if (buffer_size < sizeof(T))
        {
            throw std::runtime_error("Buffer too small for type T");
        }

        // Acquire write access
        SlotAcquireResult res = slot_rw_acquire_write(rw_state, timeout_ms);
        if (res != SLOT_ACQUIRE_OK)
        {
            throw std::runtime_error(slot_acquire_result_string(res));
        }

        // RAII guard ensures release
        struct Guard
        {
            pylabhub::hub::SlotRWState *rw;
            bool committed = false;
            ~Guard()
            {
                if (committed)
                {
                    slot_rw_commit(rw);
                }
                slot_rw_release_write(rw);
            }
        } guard{rw_state};

        // Invoke user lambda with typed reference
        T &data = *reinterpret_cast<T *>(buffer);
        auto result = std::invoke(std::forward<Func>(func), data);

        guard.committed = true; // Auto-commit on success
        return result;
    }

    // === Type-Safe Read Access ===
    template <typename T, typename Func>
    static auto with_typed_read(pylabhub::hub::SlotRWState *rw_state, const void *buffer,
                                size_t buffer_size, Func &&func, bool validate_generation = true)
        -> std::invoke_result_t<Func, const T &>
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Type T must be trivially copyable for shared memory");

        if (buffer_size < sizeof(T))
        {
            throw std::runtime_error("Buffer too small for type T");
        }

        // Acquire read access
        uint64_t generation = 0;
        SlotAcquireResult res = slot_rw_acquire_read(rw_state, &generation);
        if (res != SLOT_ACQUIRE_OK)
        {
            throw std::runtime_error(slot_acquire_result_string(res));
        }

        // RAII guard ensures release
        struct Guard
        {
            pylabhub::hub::SlotRWState *rw;
            uint64_t gen;
            bool validate;
            ~Guard()
            {
                if (validate && !slot_rw_validate_read(rw, gen))
                {
                    // Log validation failure (data was overwritten)
                    // In a real scenario, this would involve a logger
                }
                slot_rw_release_read(rw);
            }
        } guard{rw_state, generation, validate_generation};

        // Invoke user lambda with typed const reference
        const T &data = *reinterpret_cast<const T *>(buffer);
        return std::invoke(std::forward<Func>(func), data);
    }
};

} // namespace pylabhub

#endif // PYLABHUB_SLOT_RW_ACCESS_HPP
