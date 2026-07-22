#pragma once
/**
 * @file secure_buffer.hpp
 * @brief Stack-only RAII buffer that zeroes itself on destruction
 *        (HEP-CORE-0040 §6.4).
 *
 * Use for SHORT-LIVED plaintext intermediates whose lifetime is bounded
 * by a single function call — e.g. the pack buffer inside
 * `KeyStore::add_identity_from_z85` that holds the (pub_z85 || sec_z85)
 * layout for the brief moment between input and the LockedKey copy.
 *
 * NOT for long-lived storage.  Pages are unlocked (no `mlock`); swap
 * exposure exists for the buffer's lifetime.  For long-lived secrets,
 * use `LockedKey` via `KeyStore`.
 *
 * Header-only template — no .cpp.  Trivially inlinable; pImpl is not
 * needed because the only state is a fixed-size byte array.
 */

#include <array>
#include <cstddef>
#include <span>

#include <sodium.h>

namespace pylabhub::utils::security
{

/// Fixed-size stack buffer; dtor calls `sodium_memzero` (compiler cannot
/// optimize it away).  Move/copy disabled — the only sensible use is
/// "write, hand off, destruct."
template <std::size_t N> class SecureBuffer
{
  public:
    SecureBuffer() = default;

    ~SecureBuffer() noexcept { ::sodium_memzero(data_.data(), N); }

    SecureBuffer(const SecureBuffer &) = delete;
    SecureBuffer &operator=(const SecureBuffer &) = delete;
    SecureBuffer(SecureBuffer &&) = delete;
    SecureBuffer &operator=(SecureBuffer &&) = delete;

    [[nodiscard]] std::span<std::byte> span() noexcept { return std::span<std::byte>(data_); }

    [[nodiscard]] std::span<const std::byte> span() const noexcept
    {
        return std::span<const std::byte>(data_);
    }

    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }

  private:
    std::array<std::byte, N> data_{};
};

} // namespace pylabhub::utils::security
