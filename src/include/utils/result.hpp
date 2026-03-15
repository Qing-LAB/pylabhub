/**
 * @file result.hpp
 * @brief Generic Result<T, E> type for error handling without exceptions
 * 
 * @copyright Copyright (c) 2024-2026 PyLabHub Project
 * 
 * Part of Phase 3: C++ RAII Layer
 * Provides type-safe error handling for operations that can fail in expected ways.
 * 
 * Design Philosophy:
 * - Distinguishes between success (T) and expected failures (E)
 * - Forces explicit error handling at call sites
 * - No implicit conversions to bool (prevents accidental misuse)
 * - [[nodiscard]] prevents ignoring errors
 */

#pragma once

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace pylabhub::hub
{

/**
 * @brief Error type for slot acquisition operations
 * 
 * Represents the three expected failure modes when acquiring a slot:
 * - Timeout: Operation timed out waiting for slot availability
 * - NoSlot: No slot available (non-blocking mode)
 * - Error: Fatal/unrecoverable error (should end iteration)
 */
enum class SlotAcquireError
{
    Timeout, ///< Timed out waiting for slot (expected, retry possible)
    NoSlot,  ///< No slot available (non-blocking, retry possible)
    Error    ///< Fatal error (unrecoverable, should terminate)
};

/**
 * @brief Convert SlotAcquireError to string for logging/debugging
 */
inline const char *to_string(SlotAcquireError err) noexcept
{
    switch (err)
    {
    case SlotAcquireError::Timeout:
        return "Timeout";
    case SlotAcquireError::NoSlot:
        return "NoSlot";
    case SlotAcquireError::Error:
        return "Error";
    default:
        return "Unknown";
    }
}

/**
 * @class Result
 * @brief Generic Result<T, E> type for operations that can fail in expected ways
 * 
 * @tparam T Success value type
 * @tparam E Error enum type (should be an enum or enum class)
 * 
 * Inspired by Rust's Result<T, E> and C++23's std::expected<T, E>.
 * 
 * Usage:
 * @code
 * Result<int, ErrorCode> compute() {
 *     if (condition) {
 *         return Result<int, ErrorCode>::ok(42);
 *     }
 *     return Result<int, ErrorCode>::error(ErrorCode::InvalidInput);
 * }
 * 
 * auto result = compute();
 * if (result.is_ok()) {
 *     int value = result.content();
 *     // use value
 * } else {
 *     ErrorCode err = result.error();
 *     // handle error
 * }
 * @endcode
 * 
 * Thread Safety: Result objects are not thread-safe. Use separate Result
 * instances per thread or external synchronization.
 */
template <typename T, typename E>
class Result
{
  public:
    using value_type = T;
    using error_type = E;

    // ====================================================================
    // Construction - Use static factory methods for clarity
    // ====================================================================

    /**
     * @brief Create a successful Result containing a value
     * @param value The success value (moved into Result)
     * @return Result in success state
     */
    [[nodiscard]] static Result ok(T value)
    {
        Result result;
        result.m_data = std::move(value);
        return result;
    }

    /**
     * @brief Create a failed Result containing an error
     * @param err The error enum value
     * @param code Optional detailed error code (default 0)
     * @return Result in error state
     */
    [[nodiscard]] static Result error(E err, int code = 0)
    {
        Result result;
        result.m_data = ErrorData{err, code};
        return result;
    }

    // Default constructible (starts in error state with default error)
    Result() : m_data(ErrorData{E{}, 0}) {}

    // Movable but not copyable (to avoid accidental copies of large values)
    Result(Result &&) noexcept = default;
    Result &operator=(Result &&) noexcept = default;

    // Explicitly deleted copy to prevent accidental expensive copies
    // If copying is needed, use explicit .clone() method
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;

    // ====================================================================
    // State Queries
    // ====================================================================

    /**
     * @brief Check if Result contains a success value
     * @return true if success, false if error
     */
    [[nodiscard]] bool is_ok() const noexcept { return std::holds_alternative<T>(m_data); }

    /**
     * @brief Check if Result contains an error
     * @return true if error, false if success
     */
    [[nodiscard]] bool is_error() const noexcept { return !is_ok(); }

    // ====================================================================
    // Value Access
    // ====================================================================

    /**
     * @brief Get the success content (mutable reference)
     * @return Reference to the contained value
     * @throws std::logic_error if Result is in error state
     * 
     * Always check is_ok() before calling content().
     * 
     * Note: Renamed from value() to content() to better convey that this
     * returns the contained object, not a primitive value.
     */
    [[nodiscard]] T &content() &
    {
        if (!is_ok())
        {
            throw std::logic_error("Result::content() called on error state");
        }
        return std::get<T>(m_data);
    }

    /**
     * @brief Get the success content (const reference)
     * @return Const reference to the contained value
     * @throws std::logic_error if Result is in error state
     */
    [[nodiscard]] const T &content() const &
    {
        if (!is_ok())
        {
            throw std::logic_error("Result::content() called on error state");
        }
        return std::get<T>(m_data);
    }

    /**
     * @brief Move the success content out of Result
     * @return Value moved from Result
     * @throws std::logic_error if Result is in error state
     * 
     * After this call, Result is left in a valid but unspecified state.
     */
    [[nodiscard]] T &&content() &&
    {
        if (!is_ok())
        {
            throw std::logic_error("Result::content() called on error state");
        }
        return std::get<T>(std::move(m_data));
    }

    /**
     * @brief Get the success value or a default if error
     * @param default_value Value to return if Result is error
     * @return Contained value if ok, default_value if error
     */
    [[nodiscard]] T value_or(T default_value) const &
    {
        return is_ok() ? std::get<T>(m_data) : std::move(default_value);
    }

    // ====================================================================
    // Error Access
    // ====================================================================

    /**
     * @brief Get the error enum value
     * @return Error enum
     * @throws std::logic_error if Result is in success state
     * 
     * Always check is_error() before calling error().
     */
    [[nodiscard]] E error() const
    {
        if (is_ok())
        {
            throw std::logic_error("Result::error() called on success state");
        }
        return std::get<ErrorData>(m_data).error_enum;
    }

    /**
     * @brief Get the detailed error code
     * @return Error code (0 if not set)
     * @throws std::logic_error if Result is in success state
     */
    [[nodiscard]] int error_code() const
    {
        if (is_ok())
        {
            throw std::logic_error("Result::error_code() called on success state");
        }
        return std::get<ErrorData>(m_data).error_code;
    }

  private:
    // Internal error storage
    struct ErrorData
    {
        E error_enum;
        int error_code;
    };

    // Storage: either T (success) or ErrorData (failure)
    std::variant<T, ErrorData> m_data;
};

// ====================================================================
// Convenience Aliases
// ====================================================================

/**
 * @brief Result type for slot acquisition operations
 * 
 * Used by the RAII layer's slot iterator to distinguish between:
 * - Success: SlotRef<T> available
 * - Timeout: No slot available within timeout period
 * - NoSlot: No slot available (non-blocking)
 * - Error: Fatal error occurred
 * 
 * @tparam SlotRefT Type of slot reference (WriteSlotRef or ReadSlotRef)
 * 
 * @note Renamed to avoid conflict with C API enum SlotAcquireResult
 * @note Name indicates this is specifically for iterator results
 */
template <typename SlotRefT>
using IterSlotResult = Result<SlotRefT, SlotAcquireError>;

} // namespace pylabhub::hub
