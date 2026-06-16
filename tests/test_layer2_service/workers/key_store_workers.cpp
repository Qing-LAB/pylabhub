/**
 * @file key_store_workers.cpp
 * @brief Worker bodies for the KeyStore L2 test suite (HEP-CORE-0040 §170).
 *
 * Pattern 3 — each worker runs in a fresh subprocess with its own
 * LifecycleGuard (Logger).  Subprocess isolation is required because
 * `SecureMemorySubsystem` and `KeyStore` are process singletons —
 * running multiple scenarios serially in one process would have the
 * second ctor throw "already constructed."
 */
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_buffer.hpp"
#include "utils/security/secure_memory_subsystem.hpp"

#include <sodium.h>  // sodium_malloc / sodium_free — used by sodium_smoke

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;
using pylabhub::utils::security::KeyStore;
using pylabhub::utils::security::key_store;
using pylabhub::utils::security::key_store_ready;
using pylabhub::utils::security::SecureBuffer;
using pylabhub::utils::security::SecureMemorySubsystem;
using pylabhub::utils::security::secure_memory_subsystem_ready;

namespace
{

// Canonical 80-byte identity payload (pub_z85 || sec_z85, 40+40).  All
// printable ASCII so byte-equal assertions are readable in test output.
struct TestKeypair
{
    std::array<std::byte, 80> bytes{};

    static TestKeypair make(char pub_fill, char sec_fill)
    {
        TestKeypair k;
        for (std::size_t i = 0; i < 40; ++i)
            k.bytes[i] = static_cast<std::byte>(pub_fill);
        for (std::size_t i = 40; i < 80; ++i)
            k.bytes[i] = static_cast<std::byte>(sec_fill);
        return k;
    }
};

} // namespace

// ── Scenarios ──────────────────────────────────────────────────────────────

namespace
{

// ── libsodium smoke: catches a broken allocator before KeyStore tests fire
//
// Why this exists.  The downstream KeyStore tests all begin by constructing
// `SecureMemorySubsystem` (sodium_init) + `KeyStore` (dynamic module load)
// and then calling `add_identity`, which is the first site that exercises
// `sodium_malloc`.  If libsodium's guarded-allocator is misconfigured for
// the build / runtime environment (wrong page size assumption, ASan
// pointer-tag mismatch, missing HAVE_ALIGNED_MALLOC etc.) the very first
// `sodium_malloc` aborts with `assert(_unprotected_ptr_from_user_ptr(
// user_ptr) == unprotected_ptr)` in `third_party/libsodium/.../utils.c`.
// This single environmental failure casts the same SIGABRT across every
// KeyStore test that reaches an allocation, producing a 12-test storm
// with a cryptic library-internal error.  This smoke scenario provides
// a SINGLE clear failure: "sodium_malloc is broken in this build" —
// independent of any KeyStore-level contract.  If THIS test fails, the
// downstream KeyStore failures are downstream of an environmental bug,
// not 12 distinct regressions.
//
// What it covers.  Allocates + frees each of the three sizes the
// production code actually uses: 32 (HEP-CORE-0038 symmetric secrets),
// 40 (CURVE seckey halves), 80 (full 40+40 identity payload).  A size-
// specific page-protection bug (e.g. an off-by-one on the canary
// boundary that only triggers at certain sizes) would manifest here
// before it manifests in a KeyStore scenario.
int sodium_smoke(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            // SMS ctor runs sodium_init() (idempotent inside libsodium).
            // We are NOT constructing a KeyStore here — the smoke test is
            // strictly about the allocator, not about KeyStore behavior.

            for (std::size_t sz : {std::size_t{32},
                                   std::size_t{40},
                                   std::size_t{80}})
            {
                void *p = ::sodium_malloc(sz);
                ASSERT_NE(p, nullptr)
                    << "sodium_malloc(" << sz << ") returned NULL — "
                    << "library allocator broken in this build/env.  "
                    << "Investigate RLIMIT_MEMLOCK, page-size, or "
                    << "libsodium build flags BEFORE chasing KeyStore "
                    << "test failures.";
                // Touch each byte so a page-protection corruption that
                // sodium_malloc itself wouldn't catch would SIGSEGV here,
                // before sodium_free's own assertions run.
                std::memset(p, 0xA5, sz);
                ::sodium_free(p);
            }
        },
        "key_store::sodium_smoke",
        Logger::GetLifecycleModule());
}

int ordering_check_keystore_requires_secure_memory(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            // SecureMemorySubsystem NOT constructed.
            EXPECT_FALSE(secure_memory_subsystem_ready());

            // Validate exception message content — proves the
            // SecureMemorySubsystem-ready check ran (NOT some other
            // logic_error path firing accidentally).
            bool threw_with_right_message = false;
            try {
                KeyStore("role", "test-role-uid");
            } catch (const std::logic_error &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("SecureMemorySubsystem"), std::string::npos)
                    << "exception what() should name the subsystem: " << what;
                EXPECT_NE(what.find("HEP-CORE-0040"), std::string::npos)
                    << "exception what() should cite the design contract: " << what;
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);

            // KeyStore did not get registered — observable post-condition.
            EXPECT_FALSE(key_store_ready());
            // No-singleton-state side-effects: key_store() must still throw.
            EXPECT_THROW(key_store(), std::runtime_error);
        },
        "key_store::ordering_check_keystore_requires_secure_memory",
        Logger::GetLifecycleModule());
}

int second_construction_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks_first("role", "test-role-uid-1");
            EXPECT_TRUE(key_store_ready());

            // Populate the first instance so we can verify it survives.
            auto sentinel = TestKeypair::make('A', 'Z');
            ks_first.add_identity("role_identity",
                                   std::span<std::byte>(sentinel.bytes));
            EXPECT_EQ(ks_first.size(), 1u);

            // Validate exception message content.
            bool threw_with_right_message = false;
            try {
                KeyStore("role", "test-role-uid-2");
            } catch (const std::logic_error &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("already constructed"), std::string::npos)
                    << "exception what() should name the violation: " << what;
                EXPECT_NE(what.find("HEP-CORE-0040"), std::string::npos)
                    << "exception what() should cite the design contract: " << what;
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);

            // First instance must be UNCHANGED — verify the failed
            // second-ctor didn't mutate the singleton pointer or the
            // map.  Reading the seckey via with_seckey + comparing
            // every byte proves nothing got swapped or zeroed.
            EXPECT_TRUE(key_store_ready());
            EXPECT_EQ(&key_store(), &ks_first);  // same instance
            EXPECT_EQ(key_store().size(), 1u);
            EXPECT_TRUE(key_store().has("role_identity"));
            const auto pub = key_store().pubkey("role_identity");
            ASSERT_EQ(pub.size(), 40u);
            for (char c : pub) EXPECT_EQ(c, 'A');
            std::string sec_after;
            key_store().with_seckey("role_identity",
                [&](std::string_view sv) {
                    sec_after.assign(sv.data(), sv.size());
                });
            ASSERT_EQ(sec_after.size(), 40u);
            for (char c : sec_after) EXPECT_EQ(c, 'Z');
        },
        "key_store::second_construction_throws",
        Logger::GetLifecycleModule());
}

int key_store_not_initialized_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            EXPECT_FALSE(key_store_ready());

            // Validate message — proves the not-initialized branch
            // fired, not some other accidental runtime_error.
            bool threw_with_right_message = false;
            try {
                (void)key_store();
            } catch (const std::runtime_error &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("key_store"), std::string::npos)
                    << "exception what() should name the accessor: " << what;
                EXPECT_NE(what.find("not been constructed"), std::string::npos)
                    << "exception what() should name the cause: " << what;
                EXPECT_NE(what.find("HEP-CORE-0040"), std::string::npos)
                    << "exception what() should cite the design contract: " << what;
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);
        },
        "key_store::key_store_not_initialized_throws",
        Logger::GetLifecycleModule());
}

int key_store_ready_probe_lifecycle(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            EXPECT_FALSE(key_store_ready());
            {
                KeyStore ks("role", "test-role-uid");
                EXPECT_TRUE(key_store_ready());
            }
            EXPECT_FALSE(key_store_ready());
        },
        "key_store::key_store_ready_probe_lifecycle",
        Logger::GetLifecycleModule());
}

int add_identity_then_pubkey_and_with_seckey_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            // Distinct per-byte pattern (NOT uniform fills) — catches
            // off-by-one slice errors, swap between pub/sec halves,
            // and direction reversal.  pub byte i = 0x20 + i (' '..'G');
            // sec byte i = 0x60 + i ('`'..'~').  Disjoint ranges so any
            // contamination is immediately visible.
            std::array<std::byte, 80> packed{};
            for (std::size_t i = 0; i < 40; ++i)
                packed[i] = static_cast<std::byte>(0x20 + i);
            for (std::size_t i = 0; i < 40; ++i)
                packed[40 + i] = static_cast<std::byte>(0x60 + i);

            ks.add_identity("role_identity", std::span<std::byte>(packed));

            // pubkey() returns first 40 bytes — verify byte-by-byte.
            const auto pub = key_store().pubkey("role_identity");
            ASSERT_EQ(pub.size(), 40u);
            for (std::size_t i = 0; i < 40; ++i)
            {
                EXPECT_EQ(static_cast<unsigned char>(pub[i]),
                          static_cast<unsigned char>(0x20 + i))
                    << "pub byte " << i << " corrupted";
            }

            // with_seckey() invokes callback with last 40 bytes —
            // verify byte-by-byte AND verify callback was invoked
            // (not just "no exception thrown").
            bool        callback_fired = false;
            std::string captured;
            key_store().with_seckey("role_identity",
                [&](std::string_view sec) {
                    callback_fired = true;
                    captured.assign(sec.data(), sec.size());
                });
            EXPECT_TRUE(callback_fired)
                << "with_seckey returned without invoking the callback";
            ASSERT_EQ(captured.size(), 40u);
            for (std::size_t i = 0; i < 40; ++i)
            {
                EXPECT_EQ(static_cast<unsigned char>(captured[i]),
                          static_cast<unsigned char>(0x60 + i))
                    << "sec byte " << i << " corrupted";
            }
        },
        "key_store::add_identity_then_pubkey_and_with_seckey_roundtrip",
        Logger::GetLifecycleModule());
}

int add_identity_zeros_source(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            SecureBuffer<80> buf;
            // Fill with non-zero pattern.
            std::span<std::byte> sp = buf.span();
            for (std::size_t i = 0; i < sp.size(); ++i)
                sp[i] = static_cast<std::byte>(0xAB);

            ks.add_identity("role_identity", sp);

            // Source buffer must be zeroed.
            for (std::size_t i = 0; i < sp.size(); ++i)
                EXPECT_EQ(static_cast<unsigned>(sp[i]), 0u)
                    << "byte " << i << " not zeroed";
        },
        "key_store::add_identity_zeros_source",
        Logger::GetLifecycleModule());
}

int pubkey_on_missing_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            // Populate one entry so we can verify the missing-key path
            // didn't accidentally mutate map state.
            auto kp = TestKeypair::make('A', 'Z');
            ks.add_identity("role_identity", std::span<std::byte>(kp.bytes));
            EXPECT_EQ(ks.size(), 1u);

            bool threw_with_right_message = false;
            try {
                (void)key_store().pubkey("missing-key-xyz");
            } catch (const std::out_of_range &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("pubkey"), std::string::npos)
                    << "exception what() should name the accessor: " << what;
                EXPECT_NE(what.find("missing-key-xyz"), std::string::npos)
                    << "exception what() should echo the missing name: " << what;
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);

            // Post-throw state unchanged.
            EXPECT_EQ(ks.size(), 1u);
            EXPECT_TRUE(ks.has("role_identity"));
            EXPECT_FALSE(ks.has("missing-key-xyz"));
        },
        "key_store::pubkey_on_missing_throws",
        Logger::GetLifecycleModule());
}

int with_seckey_on_missing_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            auto kp = TestKeypair::make('A', 'Z');
            ks.add_identity("role_identity", std::span<std::byte>(kp.bytes));

            bool callback_invoked = false;
            bool threw_with_right_message = false;
            try {
                key_store().with_seckey("missing-key-xyz",
                    [&](std::string_view) { callback_invoked = true; });
            } catch (const std::out_of_range &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("with_seckey"), std::string::npos);
                EXPECT_NE(what.find("missing-key-xyz"), std::string::npos);
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);
            EXPECT_FALSE(callback_invoked)
                << "callback must NEVER fire on missing-key path";

            // Post-throw state unchanged.
            EXPECT_EQ(ks.size(), 1u);
            EXPECT_TRUE(ks.has("role_identity"));
        },
        "key_store::with_seckey_on_missing_throws",
        Logger::GetLifecycleModule());
}

int lookup_raw_on_missing_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            std::array<std::byte, 8> dummy{};
            ks.add_raw("vault:x", std::span<std::byte>(dummy));

            bool threw_with_right_message = false;
            try {
                (void)key_store().lookup_raw("missing-key-xyz");
            } catch (const std::out_of_range &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("lookup_raw"), std::string::npos);
                EXPECT_NE(what.find("missing-key-xyz"), std::string::npos);
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);

            EXPECT_EQ(ks.size(), 1u);
            EXPECT_TRUE(ks.has("vault:x"));
        },
        "key_store::lookup_raw_on_missing_throws",
        Logger::GetLifecycleModule());
}

int pubkey_on_raw_entry_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            // Fill raw with a recognizable pattern to verify it survives.
            std::array<std::byte, 16> raw;
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>(0x40 + i);

            ks.add_raw("vault:script-secret", std::span<std::byte>(raw));

            bool threw_with_right_message = false;
            try {
                (void)key_store().pubkey("vault:script-secret");
            } catch (const std::out_of_range &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("pubkey"), std::string::npos);
                EXPECT_NE(what.find("vault:script-secret"), std::string::npos);
                EXPECT_NE(what.find("lookup_raw"), std::string::npos)
                    << "message should hint at the correct API: " << what;
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);

            // Entry still exists and is unchanged — pubkey throwing
            // must not have mutated the LockedKey storage.
            EXPECT_TRUE(ks.has("vault:script-secret"));
            const auto bytes = key_store().lookup_raw("vault:script-secret");
            ASSERT_EQ(bytes.size(), 16u);
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                EXPECT_EQ(static_cast<unsigned>(bytes[i]),
                          static_cast<unsigned>(0x40 + i))
                    << "raw byte " << i << " corrupted by failed pubkey() call";
            }
        },
        "key_store::pubkey_on_raw_entry_throws",
        Logger::GetLifecycleModule());
}

int with_seckey_on_raw_entry_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            std::array<std::byte, 16> raw;
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>(0x80 + i);
            ks.add_raw("vault:script-secret", std::span<std::byte>(raw));

            bool callback_invoked = false;
            bool threw_with_right_message = false;
            try {
                key_store().with_seckey("vault:script-secret",
                    [&](std::string_view) { callback_invoked = true; });
            } catch (const std::out_of_range &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("with_seckey"), std::string::npos);
                EXPECT_NE(what.find("vault:script-secret"), std::string::npos);
                EXPECT_NE(what.find("lookup_raw"), std::string::npos);
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);
            EXPECT_FALSE(callback_invoked)
                << "callback must NEVER fire on wrong-type path";

            // Entry survives byte-for-byte.
            EXPECT_TRUE(ks.has("vault:script-secret"));
            const auto bytes = key_store().lookup_raw("vault:script-secret");
            ASSERT_EQ(bytes.size(), 16u);
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                EXPECT_EQ(static_cast<unsigned>(bytes[i]),
                          static_cast<unsigned>(0x80 + i));
            }
        },
        "key_store::with_seckey_on_raw_entry_throws",
        Logger::GetLifecycleModule());
}

int add_identity_duplicate_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            auto kp1 = TestKeypair::make('A', 'B');
            ks.add_identity("role_identity", std::span<std::byte>(kp1.bytes));

            auto kp2 = TestKeypair::make('C', 'D');
            EXPECT_THROW(
                ks.add_identity("role_identity", std::span<std::byte>(kp2.bytes)),
                std::runtime_error);

            // Original entry preserved.
            for (char c : key_store().pubkey("role_identity"))
                EXPECT_EQ(c, 'A');
        },
        "key_store::add_identity_duplicate_throws",
        Logger::GetLifecycleModule());
}

int add_identity_wrong_size_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            // Too small.
            {
                std::array<std::byte, 40> half{};
                EXPECT_THROW(
                    ks.add_identity("a", std::span<std::byte>(half)),
                    std::runtime_error);
            }
            // Too big.
            {
                std::array<std::byte, 96> big{};
                EXPECT_THROW(
                    ks.add_identity("b", std::span<std::byte>(big)),
                    std::runtime_error);
            }
            EXPECT_EQ(ks.size(), 0u);
        },
        "key_store::add_identity_wrong_size_throws",
        Logger::GetLifecycleModule());
}

int add_raw_then_lookup_raw_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            std::array<std::byte, 32> raw;
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>(i + 1);

            ks.add_raw("vault:s", std::span<std::byte>(raw));

            // Source zeroed.
            for (std::size_t i = 0; i < raw.size(); ++i)
                EXPECT_EQ(static_cast<unsigned>(raw[i]), 0u)
                    << "raw byte " << i << " not zeroed";

            auto out = key_store().lookup_raw("vault:s");
            ASSERT_EQ(out.size(), 32u);
            for (std::size_t i = 0; i < out.size(); ++i)
                EXPECT_EQ(static_cast<unsigned>(out[i]), i + 1);
        },
        "key_store::add_raw_then_lookup_raw_roundtrip",
        Logger::GetLifecycleModule());
}

int remove_makes_subsequent_lookup_throw(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            auto kp = TestKeypair::make('A', 'B');
            ks.add_identity("role_identity", std::span<std::byte>(kp.bytes));
            EXPECT_TRUE(key_store().has("role_identity"));

            ks.remove("role_identity");
            EXPECT_FALSE(key_store().has("role_identity"));
            EXPECT_THROW(key_store().pubkey("role_identity"),
                         std::out_of_range);
            EXPECT_THROW(
                key_store().with_seckey("role_identity",
                    [](std::string_view) { FAIL(); }),
                std::out_of_range);

            // remove on absent name is a no-op.
            EXPECT_NO_THROW(ks.remove("role_identity"));
        },
        "key_store::remove_makes_subsequent_lookup_throw",
        Logger::GetLifecycleModule());
}

int has_and_size_track_entries(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            EXPECT_EQ(ks.size(), 0u);
            EXPECT_FALSE(ks.has("anything"));

            auto kp = TestKeypair::make('A', 'B');
            ks.add_identity("role_identity", std::span<std::byte>(kp.bytes));
            EXPECT_EQ(ks.size(), 1u);
            EXPECT_TRUE(ks.has("role_identity"));

            std::array<std::byte, 16> raw{};
            ks.add_raw("vault:x", std::span<std::byte>(raw));
            EXPECT_EQ(ks.size(), 2u);
            EXPECT_TRUE(ks.has("vault:x"));

            ks.remove("role_identity");
            EXPECT_EQ(ks.size(), 1u);
            EXPECT_FALSE(ks.has("role_identity"));
        },
        "key_store::has_and_size_track_entries",
        Logger::GetLifecycleModule());
}

int parallel_reads_do_not_block(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            auto kp = TestKeypair::make('P', 'S');
            ks.add_identity("role_identity", std::span<std::byte>(kp.bytes));

            constexpr int kNumThreads = 4;

            // Barrier: all reader threads must be inside their callback
            // simultaneously.  If `with_seckey` serialised readers, this
            // would deadlock (one reader holding the shared lock while
            // others wait → can't all reach the barrier).
            std::mutex                    bar_mu;
            std::condition_variable       bar_cv;
            int                           inside = 0;
            std::atomic<bool>             release{false};

            auto reader = [&]() {
                key_store().with_seckey("role_identity",
                    [&](std::string_view sec) {
                        EXPECT_EQ(sec.size(), 40u);
                        // Signal arrival.
                        {
                            std::lock_guard<std::mutex> lk(bar_mu);
                            ++inside;
                            bar_cv.notify_all();
                        }
                        // Wait for release.
                        while (!release.load(std::memory_order_acquire))
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(100));
                    });
            };

            std::vector<std::thread> threads;
            threads.reserve(kNumThreads);
            for (int i = 0; i < kNumThreads; ++i)
                threads.emplace_back(reader);

            // Wait for all readers to be simultaneously inside their
            // callbacks (proves parallel reads).  Timeout deliberately
            // generous (30s, not 5s): if `with_seckey` flips to
            // exclusive locking, the barrier predicate is UNSATISFIABLE
            // and the timeout fires immediately on contract violation —
            // the wait duration only matters when the scheduler hasn't
            // run all threads.  A short timeout would conflate "lock
            // serialised readers" (the contract failure we want to
            // catch) with "CI scheduler starved the readers under -jN
            // load" (environmental).  30s is far more than any sane
            // scheduler delay; if THIS fires, the environment is broken,
            // not the lock contract.
            {
                std::unique_lock<std::mutex> lk(bar_mu);
                ASSERT_TRUE(bar_cv.wait_for(lk,
                    std::chrono::seconds(30),
                    [&]() { return inside == kNumThreads; }))
                    << "Readers serialised — only " << inside
                    << "/" << kNumThreads << " reached the barrier "
                    << "within 30s.  At this duration this almost "
                    << "certainly means `with_seckey` no longer holds "
                    << "the lock in SHARED mode (regression to "
                    << "exclusive lock).  Environmental scheduling delay "
                    << "alone would not exhaust 30s under any realistic "
                    << "CI load.";
            }
            release.store(true, std::memory_order_release);
            for (auto &t : threads) t.join();
        },
        "key_store::parallel_reads_do_not_block",
        Logger::GetLifecycleModule());
}

int remove_blocks_behind_in_flight_with_seckey(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            SecureMemorySubsystem sms;
            KeyStore              ks("role", "test-role-uid");

            auto kp = TestKeypair::make('P', 'S');
            ks.add_identity("role_identity", std::span<std::byte>(kp.bytes));

            std::atomic<bool> reader_inside{false};
            std::atomic<bool> reader_release{false};
            // `remove_entered` is set BEFORE ks.remove() — proves the
            // remover thread was actually scheduled.  Without this, a
            // late-scheduled remover would leave `remove_returned`
            // false during the blocking check and the test would
            // misread "scheduler-delayed" as "lock contract held."
            std::atomic<bool> remove_entered{false};
            std::atomic<bool> remove_returned{false};

            std::thread reader([&]() {
                key_store().with_seckey("role_identity",
                    [&](std::string_view) {
                        reader_inside.store(true, std::memory_order_release);
                        while (!reader_release.load(std::memory_order_acquire))
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(100));
                    });
            });

            // Wait until reader is inside its callback (holds shared lock).
            while (!reader_inside.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::microseconds(100));

            std::thread remover([&]() {
                remove_entered.store(true, std::memory_order_release);
                ks.remove("role_identity");
                remove_returned.store(true, std::memory_order_release);
            });

            // Wait until the remover thread is past the entry barrier,
            // so the subsequent blocking check is observing an actually-
            // in-flight remove() and not a yet-to-be-scheduled thread.
            // 5s is a generous schedule timeout: if it fires the test
            // environment is broken, not the contract.
            {
                const auto deadline = std::chrono::steady_clock::now()
                                    + std::chrono::seconds(5);
                while (!remove_entered.load(std::memory_order_acquire))
                {
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        ADD_FAILURE() << "remover thread never reached "
                            "entry barrier within 5s — CI scheduler "
                            "starvation, not a contract regression.  "
                            "If recurrent, investigate test environment "
                            "before changing this assertion.";
                        // Best-effort cleanup before failing out.
                        reader_release.store(true, std::memory_order_release);
                        reader.join();
                        remover.join();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }

            // remove should NOT return while reader holds shared lock.
            // The 100ms window is the contract-violation detector: if
            // the lock contract were broken, ks.remove() would return
            // immediately, well within this window.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            EXPECT_FALSE(remove_returned.load(std::memory_order_acquire))
                << "remove() returned while a with_seckey callback was "
                   "still in flight — shared/exclusive lock contract "
                   "violated.  Confirmed in-flight by remove_entered=true "
                   "before this check.";

            // Release reader; remove should then unblock.
            reader_release.store(true, std::memory_order_release);
            reader.join();
            remover.join();
            EXPECT_TRUE(remove_returned.load(std::memory_order_acquire));
            EXPECT_FALSE(key_store().has("role_identity"));
        },
        "key_store::remove_blocks_behind_in_flight_with_seckey",
        Logger::GetLifecycleModule());
}

int secure_buffer_dtor_zeroes(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            // Place a SecureBuffer in heap-controlled raw storage that
            // we own.  Capture a pointer into its byte array, fill with
            // a pattern, then EXPLICITLY destruct the SecureBuffer.
            // Reading the bytes via the captured pointer is defined —
            // the underlying `storage` array is still alive, the
            // destructor's `sodium_memzero` ran on those same bytes.
            //
            // This avoids the UB of reading post-scope stack memory
            // that the earlier (weaker) version of this test had.
            using Buf = SecureBuffer<32>;
            alignas(Buf) std::array<std::byte, sizeof(Buf)> storage{};

            Buf       *buf       = new (storage.data()) Buf();
            std::byte *bytes_ptr = nullptr;
            std::size_t len      = 0;

            {
                auto sp = buf->span();
                bytes_ptr = sp.data();
                len       = sp.size();
                ASSERT_EQ(len, 32u);

                // Distinct per-byte pattern — verifies the fill
                // actually happened (a no-op fill would still leave
                // pre-init zeros and the post-destruct check would
                // pass for the wrong reason).
                for (std::size_t i = 0; i < len; ++i)
                    sp[i] = static_cast<std::byte>(0xC0 + (i % 0x40));

                // Confirm the pattern landed.
                for (std::size_t i = 0; i < len; ++i)
                {
                    EXPECT_EQ(static_cast<unsigned>(bytes_ptr[i]),
                              static_cast<unsigned>(0xC0 + (i % 0x40)))
                        << "pre-destruct byte " << i;
                }
            }

            // EXPLICIT destructor call (no scope exit needed) — the
            // SecureBuffer's data_ array's sodium_memzero must run on
            // the bytes that `bytes_ptr` aliases.
            buf->~Buf();

            // Read via captured pointer.  `storage` is still alive;
            // the bytes at bytes_ptr are within `storage`'s lifetime,
            // so this read is defined behavior.  Verify EVERY byte was
            // zeroed.
            for (std::size_t i = 0; i < len; ++i)
            {
                EXPECT_EQ(static_cast<unsigned>(bytes_ptr[i]), 0u)
                    << "SecureBuffer dtor failed to zero byte " << i
                    << " (was 0x" << std::hex
                    << static_cast<unsigned>(bytes_ptr[i]) << ")";
            }
        },
        "key_store::secure_buffer_dtor_zeroes",
        Logger::GetLifecycleModule());
}

} // namespace

// ── Dispatcher + registrar ─────────────────────────────────────────────────

int dispatch_key_store(int argc, char **argv)
{
    if (argc < 2) return -1;
    std::string mode = argv[1];
    const auto dot = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "key_store") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr, "key_store.%s: missing <tmpdir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *tmpdir = argv[2];

    if (scenario == "sodium_smoke")
        return sodium_smoke(tmpdir);
    if (scenario == "ordering_check_keystore_requires_secure_memory")
        return ordering_check_keystore_requires_secure_memory(tmpdir);
    if (scenario == "second_construction_throws")
        return second_construction_throws(tmpdir);
    if (scenario == "key_store_not_initialized_throws")
        return key_store_not_initialized_throws(tmpdir);
    if (scenario == "key_store_ready_probe_lifecycle")
        return key_store_ready_probe_lifecycle(tmpdir);
    if (scenario == "add_identity_then_pubkey_and_with_seckey_roundtrip")
        return add_identity_then_pubkey_and_with_seckey_roundtrip(tmpdir);
    if (scenario == "add_identity_zeros_source")
        return add_identity_zeros_source(tmpdir);
    if (scenario == "pubkey_on_missing_throws")
        return pubkey_on_missing_throws(tmpdir);
    if (scenario == "with_seckey_on_missing_throws")
        return with_seckey_on_missing_throws(tmpdir);
    if (scenario == "lookup_raw_on_missing_throws")
        return lookup_raw_on_missing_throws(tmpdir);
    if (scenario == "pubkey_on_raw_entry_throws")
        return pubkey_on_raw_entry_throws(tmpdir);
    if (scenario == "with_seckey_on_raw_entry_throws")
        return with_seckey_on_raw_entry_throws(tmpdir);
    if (scenario == "add_identity_duplicate_throws")
        return add_identity_duplicate_throws(tmpdir);
    if (scenario == "add_identity_wrong_size_throws")
        return add_identity_wrong_size_throws(tmpdir);
    if (scenario == "add_raw_then_lookup_raw_roundtrip")
        return add_raw_then_lookup_raw_roundtrip(tmpdir);
    if (scenario == "remove_makes_subsequent_lookup_throw")
        return remove_makes_subsequent_lookup_throw(tmpdir);
    if (scenario == "has_and_size_track_entries")
        return has_and_size_track_entries(tmpdir);
    if (scenario == "parallel_reads_do_not_block")
        return parallel_reads_do_not_block(tmpdir);
    if (scenario == "remove_blocks_behind_in_flight_with_seckey")
        return remove_blocks_behind_in_flight_with_seckey(tmpdir);
    if (scenario == "secure_buffer_dtor_zeroes")
        return secure_buffer_dtor_zeroes(tmpdir);

    std::fprintf(stderr, "key_store: unknown scenario '%s'\n", scenario.c_str());
    return 1;
}

struct KeyStoreWorkerRegistrar
{
    KeyStoreWorkerRegistrar()
    {
        ::register_worker_dispatcher(dispatch_key_store);
    }
};
static KeyStoreWorkerRegistrar g_key_store_registrar;
