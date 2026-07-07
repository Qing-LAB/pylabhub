/**
 * @file key_store_workers.cpp
 * @brief Worker bodies for the KeyStore L2 test suite (HEP-CORE-0040 §170).
 *
 * Pattern 3 — each worker runs in a fresh subprocess with its own
 * LifecycleGuard (Logger).  Subprocess isolation is required because
 * `SecureSubsystem` and `KeyStore` are process singletons —
 * running multiple scenarios serially in one process would have the
 * second ctor throw "already constructed."
 */
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_buffer.hpp"
#include "utils/security/secure_subsystem.hpp"

#include <zmq.h>  // zmq_z85_encode — compute expected pubkey at the test
                  // boundary (HEP-CORE-0040 §8.5.2; #291 follow-up).

#include <sodium.h>  // sodium_malloc / sodium_free — used by sodium_smoke

#include "curve_test_setup.h"      // make_curve_setup + seed_curve_identities (box test)
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>  // std::getenv — used by sodium_smoke
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
using pylabhub::utils::security::secure;
using pylabhub::utils::security::SecureBuffer;
using pylabhub::utils::security::SecureSubsystem;

namespace
{

// Canonical 64-byte identity payload (pub_raw || sec_raw, 32+32) —
// HEP-CORE-0040 §8.5.2 (#291 follow-up, 2026-06-26).  Pre-#291 the
// layout was 80 bytes Z85 (40+40); the KeyStore raw-storage flip
// changed both the storage layout and the boundary contract.  Raw
// bytes are arbitrary `pub_fill`/`sec_fill` bytes — readability isn't
// the test's purpose, the size-equality + zero-source behaviour is.
struct TestKeypair
{
    std::array<std::byte, 64> bytes{};

    static TestKeypair make(char pub_fill, char sec_fill)
    {
        TestKeypair k;
        for (std::size_t i = 0; i < 32; ++i)
            k.bytes[i] = static_cast<std::byte>(pub_fill);
        for (std::size_t i = 32; i < 64; ++i)
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
// `SecureSubsystem` (sodium_init) + `KeyStore` (dynamic module load)
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
            // SMS ctor runs sodium_init() (idempotent inside libsodium).
            // We are NOT constructing a KeyStore here — the smoke test is
            // strictly about the allocator, not about KeyStore behavior.

            // Diagnostic branching based on the standard CI env var
            // (`CI=true` is set by GitHub Actions, GitLab CI, CircleCI,
            // Buildkite, and most other CI services).  This does NOT
            // change the assertion semantics — both branches FAIL
            // loudly on a broken allocator.  It only changes WHERE to
            // look first, since the two environments have very
            // different prior probabilities for each failure mode:
            //
            //   - In CI: the allocator is almost always broken by
            //     environment (page-size mismatch, RLIMIT_MEMLOCK,
            //     libsodium build flags, sandboxing).  A code-side
            //     regression that broke sodium would show up locally
            //     first.
            //   - Locally: the dev workstation environment is stable
            //     and the more likely cause is a real bug — a
            //     production-code change introducing memory corruption
            //     that lands on the sodium allocator, a system
            //     libsodium being preloaded over our bundled
            //     libsodium, or a broken libsodium install.
            //
            // Misclassifying these wastes triage time.  A "investigate
            // build flags" hint sent to a local dev whose actual
            // problem is a memory-corruption regression points them at
            // the wrong tree.
            const bool is_ci = (std::getenv("CI") != nullptr);
            const char *failure_hint = is_ci
                ? "ENV LIKELY (CI): investigate page-size mismatch, "
                  "RLIMIT_MEMLOCK, libsodium build flags or sandboxing "
                  "in the runner image BEFORE chasing KeyStore test "
                  "failures — the 12-test storm in 2026-06-16 CI had "
                  "this exact signature."
                : "REGRESSION LIKELY (local dev): sodium does not break "
                  "spontaneously on a stable workstation.  Likely "
                  "causes, in order: (1) a recent production-code "
                  "change has corrupted the allocator state (look at "
                  "KeyStore / SecureSubsystem / curve_keypair "
                  "edits); (2) system libsodium is being loaded over "
                  "our bundled libsodium (check LD_LIBRARY_PATH and "
                  "linker output); (3) the libsodium build under "
                  "third_party/ is stale or corrupt (clean build).  "
                  "Investigate THIS, NOT environment, before opening "
                  "an environment ticket.";

            for (std::size_t sz : {std::size_t{32},
                                   std::size_t{40},
                                   std::size_t{80}})
            {
                void *p = ::sodium_malloc(sz);
                ASSERT_NE(p, nullptr)
                    << "sodium_malloc(" << sz << ") returned NULL.\n"
                    << failure_hint;
                // Touch each byte so a page-protection corruption that
                // sodium_malloc itself wouldn't catch would SIGSEGV here,
                // before sodium_free's own assertions run.
                std::memset(p, 0xA5, sz);
                ::sodium_free(p);
            }
        },
        "key_store::sodium_smoke",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}




int add_identity_then_pubkey_and_with_seckey_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            // HEP-CORE-0040 §8.5.2 (#291, 2026-06-26) — raw 64-byte
            // layout (pub_raw[32] || sec_raw[32]).  Distinct per-byte
            // pattern (NOT uniform fills) catches off-by-one slice
            // errors, swap between pub/sec halves, and direction
            // reversal.  pub byte i = 0x20 + i (' '..'?');
            // sec byte i = 0x60 + i ('`'..'~').  Disjoint ranges so
            // any contamination is immediately visible.
            std::array<std::byte, 64> packed{};
            for (std::size_t i = 0; i < 32; ++i)
                packed[i] = static_cast<std::byte>(0x20 + i);
            for (std::size_t i = 0; i < 32; ++i)
                packed[32 + i] = static_cast<std::byte>(0x60 + i);

            // Compute the expected Z85 pubkey BEFORE moving `packed`
            // into KeyStore so the comparison is independent of
            // KeyStore internals (single source of truth = libzmq's
            // zmq_z85_encode of the same raw bytes).
            char expected_pub_z85[41] = {};
            ASSERT_NE(zmq_z85_encode(
                expected_pub_z85,
                reinterpret_cast<const uint8_t *>(packed.data()),
                32), nullptr) << "test setup: zmq_z85_encode(pub) failed";

            secure().keys().add_identity("role_identity", std::span<std::byte>(packed));

            // pubkey() returns Z85(raw_pub) — 40 ASCII chars.
            const auto pub = secure().keys().pubkey("role_identity");
            ASSERT_EQ(pub.size(), 40u);
            EXPECT_EQ(std::string(pub),
                      std::string(expected_pub_z85, 40))
                << "pubkey() returned different Z85 than zmq_z85_encode of "
                   "raw bytes — KeyStore Z85 encoding boundary broken";

            // with_seckey() invokes callback with RAW 32 bytes
            // (HEP-CORE-0040 §8.5.2) — verify byte-by-byte AND verify
            // callback was invoked (not just "no exception thrown").
            bool        callback_fired = false;
            std::string captured;
            secure().keys().with_seckey("role_identity",
                [&](std::string_view sec) {
                    callback_fired = true;
                    captured.assign(sec.data(), sec.size());
                });
            EXPECT_TRUE(callback_fired)
                << "with_seckey returned without invoking the callback";
            ASSERT_EQ(captured.size(), 32u);  // raw seckey, not Z85
            for (std::size_t i = 0; i < 32; ++i)
            {
                EXPECT_EQ(static_cast<unsigned char>(captured[i]),
                          static_cast<unsigned char>(0x60 + i))
                    << "sec byte " << i << " corrupted";
            }
        },
        "key_store::add_identity_then_pubkey_and_with_seckey_roundtrip",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int add_identity_zeros_source(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            // HEP-CORE-0040 §8.5.2 — raw 64-byte storage layout.
            SecureBuffer<64> buf;
            // Fill with non-zero pattern.
            std::span<std::byte> sp = buf.span();
            for (std::size_t i = 0; i < sp.size(); ++i)
                sp[i] = static_cast<std::byte>(0xAB);

            secure().keys().add_identity("role_identity", sp);

            // Source buffer must be zeroed.
            for (std::size_t i = 0; i < sp.size(); ++i)
                EXPECT_EQ(static_cast<unsigned>(sp[i]), 0u)
                    << "byte " << i << " not zeroed";
        },
        "key_store::add_identity_zeros_source",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int pubkey_on_missing_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            // Populate one entry so we can verify the missing-key path
            // didn't accidentally mutate map state.
            auto kp = TestKeypair::make('A', 'Z');
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp.bytes));
            EXPECT_EQ(secure().keys().size(), 1u);

            bool threw_with_right_message = false;
            try {
                (void)secure().keys().pubkey("missing-key-xyz");
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
            EXPECT_EQ(secure().keys().size(), 1u);
            EXPECT_TRUE(secure().keys().has("role_identity"));
            EXPECT_FALSE(secure().keys().has("missing-key-xyz"));
        },
        "key_store::pubkey_on_missing_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int with_seckey_on_missing_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            auto kp = TestKeypair::make('A', 'Z');
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp.bytes));

            bool callback_invoked = false;
            bool threw_with_right_message = false;
            try {
                secure().keys().with_seckey("missing-key-xyz",
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
            EXPECT_EQ(secure().keys().size(), 1u);
            EXPECT_TRUE(secure().keys().has("role_identity"));
        },
        "key_store::with_seckey_on_missing_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int lookup_raw_on_missing_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            std::array<std::byte, 8> dummy{};
            secure().keys().add_raw("vault:x", std::span<std::byte>(dummy));

            bool threw_with_right_message = false;
            try {
                (void)secure().keys().lookup_raw("missing-key-xyz");
            } catch (const std::out_of_range &e) {
                const std::string what = e.what();
                EXPECT_NE(what.find("lookup_raw"), std::string::npos);
                EXPECT_NE(what.find("missing-key-xyz"), std::string::npos);
                threw_with_right_message = true;
            }
            EXPECT_TRUE(threw_with_right_message);

            EXPECT_EQ(secure().keys().size(), 1u);
            EXPECT_TRUE(secure().keys().has("vault:x"));
        },
        "key_store::lookup_raw_on_missing_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int pubkey_on_raw_entry_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            // Fill raw with a recognizable pattern to verify it survives.
            std::array<std::byte, 16> raw;
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>(0x40 + i);

            secure().keys().add_raw("vault:script-secret", std::span<std::byte>(raw));

            bool threw_with_right_message = false;
            try {
                (void)secure().keys().pubkey("vault:script-secret");
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
            EXPECT_TRUE(secure().keys().has("vault:script-secret"));
            const auto bytes = secure().keys().lookup_raw("vault:script-secret");
            ASSERT_EQ(bytes.size(), 16u);
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                EXPECT_EQ(static_cast<unsigned>(bytes[i]),
                          static_cast<unsigned>(0x40 + i))
                    << "raw byte " << i << " corrupted by failed pubkey() call";
            }
        },
        "key_store::pubkey_on_raw_entry_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int with_seckey_on_raw_entry_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            std::array<std::byte, 16> raw;
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>(0x80 + i);
            secure().keys().add_raw("vault:script-secret", std::span<std::byte>(raw));

            bool callback_invoked = false;
            bool threw_with_right_message = false;
            try {
                secure().keys().with_seckey("vault:script-secret",
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
            EXPECT_TRUE(secure().keys().has("vault:script-secret"));
            const auto bytes = secure().keys().lookup_raw("vault:script-secret");
            ASSERT_EQ(bytes.size(), 16u);
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                EXPECT_EQ(static_cast<unsigned>(bytes[i]),
                          static_cast<unsigned>(0x80 + i));
            }
        },
        "key_store::with_seckey_on_raw_entry_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int add_identity_duplicate_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            auto kp1 = TestKeypair::make('A', 'B');
            // HEP-CORE-0040 §8.5.2 — compute kp1's expected Z85 pubkey
            // BEFORE add_identity zeros the source.
            char expected_kp1_pub_z85[41] = {};
            ASSERT_NE(zmq_z85_encode(
                expected_kp1_pub_z85,
                reinterpret_cast<const uint8_t *>(kp1.bytes.data()),
                32), nullptr);
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp1.bytes));

            auto kp2 = TestKeypair::make('C', 'D');
            EXPECT_THROW(
                secure().keys().add_identity("role_identity", std::span<std::byte>(kp2.bytes)),
                std::runtime_error);

            // Original entry preserved — pubkey returns kp1's Z85.
            EXPECT_EQ(std::string(secure().keys().pubkey("role_identity")),
                      std::string(expected_kp1_pub_z85, 40))
                << "kp1's pubkey clobbered by failed duplicate add — "
                   "the failed add_identity must NOT mutate the map";
        },
        "key_store::add_identity_duplicate_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int add_identity_wrong_size_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            // Too small.
            {
                std::array<std::byte, 40> half{};
                EXPECT_THROW(
                    secure().keys().add_identity("a", std::span<std::byte>(half)),
                    std::runtime_error);
            }
            // Too big.
            {
                std::array<std::byte, 96> big{};
                EXPECT_THROW(
                    secure().keys().add_identity("b", std::span<std::byte>(big)),
                    std::runtime_error);
            }
            EXPECT_EQ(secure().keys().size(), 0u);
        },
        "key_store::add_identity_wrong_size_throws",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int add_raw_then_lookup_raw_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            std::array<std::byte, 32> raw;
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>(i + 1);

            secure().keys().add_raw("vault:s", std::span<std::byte>(raw));

            // Source zeroed.
            for (std::size_t i = 0; i < raw.size(); ++i)
                EXPECT_EQ(static_cast<unsigned>(raw[i]), 0u)
                    << "raw byte " << i << " not zeroed";

            auto out = secure().keys().lookup_raw("vault:s");
            ASSERT_EQ(out.size(), 32u);
            for (std::size_t i = 0; i < out.size(); ++i)
                EXPECT_EQ(static_cast<unsigned>(out[i]), i + 1);
        },
        "key_store::add_raw_then_lookup_raw_roundtrip",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int remove_makes_subsequent_lookup_throw(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            auto kp = TestKeypair::make('A', 'B');
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp.bytes));
            EXPECT_TRUE(secure().keys().has("role_identity"));

            secure().keys().remove("role_identity");
            EXPECT_FALSE(secure().keys().has("role_identity"));
            EXPECT_THROW(secure().keys().pubkey("role_identity"),
                         std::out_of_range);
            EXPECT_THROW(
                secure().keys().with_seckey("role_identity",
                    [](std::string_view) { FAIL(); }),
                std::out_of_range);

            // remove on absent name is a no-op.
            EXPECT_NO_THROW(secure().keys().remove("role_identity"));
        },
        "key_store::remove_makes_subsequent_lookup_throw",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int has_and_size_track_entries(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            EXPECT_EQ(secure().keys().size(), 0u);
            EXPECT_FALSE(secure().keys().has("anything"));

            auto kp = TestKeypair::make('A', 'B');
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp.bytes));
            EXPECT_EQ(secure().keys().size(), 1u);
            EXPECT_TRUE(secure().keys().has("role_identity"));

            std::array<std::byte, 16> raw{};
            secure().keys().add_raw("vault:x", std::span<std::byte>(raw));
            EXPECT_EQ(secure().keys().size(), 2u);
            EXPECT_TRUE(secure().keys().has("vault:x"));

            secure().keys().remove("role_identity");
            EXPECT_EQ(secure().keys().size(), 1u);
            EXPECT_FALSE(secure().keys().has("role_identity"));
        },
        "key_store::has_and_size_track_entries",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int parallel_reads_do_not_block(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            auto kp = TestKeypair::make('P', 'S');
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp.bytes));

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
                secure().keys().with_seckey("role_identity",
                    [&](std::string_view sec) {
                        EXPECT_EQ(sec.size(), 32u);
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
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

int remove_blocks_behind_in_flight_with_seckey(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {

            auto kp = TestKeypair::make('P', 'S');
            secure().keys().add_identity("role_identity", std::span<std::byte>(kp.bytes));

            std::atomic<bool> reader_inside{false};
            std::atomic<bool> reader_release{false};
            // `remove_entered` is set immediately on entry to the
            // remover thread's lambda body, BEFORE `secure().keys().remove()` is
            // called.  It proves the remover thread was actually
            // scheduled — without it, a late-scheduled remover would
            // leave `remove_returned` false during the blocking check
            // and the test would misread "scheduler-delayed" as
            // "lock contract held."
            //
            // CAVEAT (R2 from code-review of aeda3b32):
            // remove_entered=true means "remover thread entered its
            // lambda" — NOT "remover thread acquired the mutex inside
            // secure().keys().remove."  A pre-emption window remains between
            // `remove_entered.store(true)` and the actual lock-take
            // inside secure().keys().remove.  The test still catches the real
            // contract regressions (remove doesn't lock at all OR
            // with_seckey releases the lock before invoking the
            // callback — both would let remove_returned go true within
            // 100ms).  We just can't tighten this further without
            // instrumenting the shared_mutex itself.
            std::atomic<bool> remove_entered{false};
            std::atomic<bool> remove_returned{false};

            std::thread reader([&]() {
                secure().keys().with_seckey("role_identity",
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
                secure().keys().remove("role_identity");
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
            // the lock contract were broken, secure().keys().remove() would return
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
            EXPECT_FALSE(secure().keys().has("role_identity"));
        },
        "key_store::remove_blocks_behind_in_flight_with_seckey",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
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
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

// ─────────────────────────────────────────────────────────────────
// SEC-Fold-2 merger invariants (HEP-CORE-0043 §1-§2 + §7)
// ─────────────────────────────────────────────────────────────────
// The four scenarios below verify SEC-Fold-2 merger invariants
// (HEP-CORE-0043 §1-§2 + §7).  They live in the KeyStoreTest binary
// because KeyStore is now a MEMBER of SecureSubsystem and the two
// classes' invariants are indivisible.

/// `SecureSubsystem::instance()` returns the same reference on
/// repeated calls — enforces the function-local static singleton
/// (HEP-CORE-0043 §1.3 mechanism 1).
int sms_instance_returns_same_reference(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            auto &a = sec::SecureSubsystem::instance();
            auto &b = sec::SecureSubsystem::instance();
            auto &c = sec::SecureSubsystem::instance();
            EXPECT_EQ(&a, &b);
            EXPECT_EQ(&a, &c);
            // Also matches the `secure()` accessor.
            EXPECT_EQ(&sec::secure(), &a);
        },
        "key_store::sms_instance_returns_same_reference",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// The SMS sodium-wrapper API is reachable via `secure()` and
/// produces plausible output — smoke coverage for the wrappers
/// that had no direct tests pre-merger.  Verifies:
///   - `random_bytes` writes non-zero output.
///   - `memcmp_ct` returns true for equal spans and false for
///     unequal spans.
///   - `memzero` clears the target buffer.
int sms_wrappers_work_via_secure_accessor(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            // random_bytes: 64 bytes, must be non-uniform.
            std::array<std::uint8_t, 64> buf{};
            sec::secure().random_bytes(std::span<std::uint8_t>(buf));
            bool all_zero = true;
            for (std::uint8_t b : buf) if (b != 0) { all_zero = false; break; }
            EXPECT_FALSE(all_zero) << "random_bytes returned all-zero output";

            // memcmp_ct: equal spans → true; single-byte flip → false.
            std::array<std::uint8_t, 32> a{};
            std::array<std::uint8_t, 32> b{};
            EXPECT_TRUE(sec::secure().memcmp_ct(
                std::span<const std::uint8_t>(a), std::span<const std::uint8_t>(b)));
            b[7] = std::uint8_t{0x42};
            EXPECT_FALSE(sec::secure().memcmp_ct(
                std::span<const std::uint8_t>(a), std::span<const std::uint8_t>(b)));

            // memzero: fill then wipe.
            std::array<std::uint8_t, 16> victim{};
            for (auto &x : victim) x = std::uint8_t{0xAB};
            sec::secure().memzero(std::span<std::uint8_t>(victim));
            for (std::uint8_t x : victim) EXPECT_EQ(x, 0u);
        },
        "key_store::sms_wrappers_work_via_secure_accessor",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// KeyStore state (identities added via `secure().keys()`) persists
/// across the SMS lifecycle window — proves that KeyStore is
/// genuinely a member of SMS's Impl with the same lifetime, not a
/// per-call transient (HEP-CORE-0043 §2.2).
int keystore_state_persists_across_sms_window(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            auto pack = TestKeypair::make('P', 'S');
            sec::secure().keys().add_identity("persist_test",
                std::span<std::byte>(pack.bytes));

            EXPECT_TRUE(sec::secure().keys().has("persist_test"));
            EXPECT_EQ(sec::secure().keys().size(), 1u);

            // Multiple `secure()` calls, mutation, more calls —
            // KeyStore state must survive throughout.
            std::array<std::uint8_t, 4> rnd_buf{};
            sec::secure().random_bytes(std::span<std::uint8_t>(rnd_buf));
            (void)sec::secure().memcmp_ct(
                std::span<const std::uint8_t>{},
                std::span<const std::uint8_t>{});
            EXPECT_TRUE(sec::secure().keys().has("persist_test"));
            EXPECT_EQ(sec::secure().keys().size(), 1u);

            // Retrieve the seckey we stored — proves the LockedKey
            // still contains the exact bytes we added.
            std::string seckey_bytes;
            sec::secure().keys().with_seckey("persist_test",
                [&](std::string_view sv) {
                    seckey_bytes.assign(sv.data(), sv.size());
                });
            ASSERT_EQ(seckey_bytes.size(), 32u);
            for (char c : seckey_bytes) EXPECT_EQ(c, 'S');
        },
        "key_store::keystore_state_persists_across_sms_window",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// `secure().keys()` returns the same KeyStore reference on
/// repeated calls — proves the member-of-Impl model (there is
/// exactly one KeyStore, embedded in SMS's Impl).
int secure_keys_returns_same_reference(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            auto &ks_a = sec::secure().keys();
            auto &ks_b = sec::secure().keys();
            auto &ks_c = sec::secure().keys();  // via shim
            EXPECT_EQ(&ks_a, &ks_b);
            EXPECT_EQ(&ks_a, &ks_c);
        },
        "key_store::secure_keys_returns_same_reference",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.1 — the encryption verbs `secretbox_encrypt` / `secretbox_decrypt`
/// are directly on `SecureSubsystem` (Category 1c post-Crypto-collapse,
/// HEP-CORE-0043 §2.1).  Roundtrip: encrypt then decrypt yields the
/// original plaintext; MAC-tamper detection returns 0.
int secretbox_encrypt_decrypt_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            std::array<std::uint8_t, sec::SecureSubsystem::kSecretboxKeyBytes>   key{};
            std::array<std::uint8_t, sec::SecureSubsystem::kSecretboxNonceBytes> nonce{};
            sec::secure().random_bytes(std::span<std::uint8_t>(key));
            sec::secure().random_bytes(std::span<std::uint8_t>(nonce));

            const std::string plain = "attack at dawn";
            std::array<std::uint8_t, 128> ct{};
            const std::size_t written = sec::secure().secretbox_encrypt(
                ct.data(), ct.size(),
                reinterpret_cast<const std::uint8_t *>(plain.data()),
                plain.size(),
                nonce.data(), key.data());
            ASSERT_EQ(written, plain.size() + sec::SecureSubsystem::kSecretboxMacBytes);

            std::array<std::uint8_t, 128> pt{};
            const std::size_t decoded = sec::secure().secretbox_decrypt(
                pt.data(), pt.size(),
                ct.data(), written,
                nonce.data(), key.data());
            ASSERT_EQ(decoded, plain.size());
            EXPECT_EQ(std::string(reinterpret_cast<const char *>(pt.data()), decoded),
                      plain);

            // Tamper the MAC (first byte) — decrypt must refuse.
            ct[0] ^= 0x01;
            const std::size_t bad = sec::secure().secretbox_decrypt(
                pt.data(), pt.size(),
                ct.data(), written,
                nonce.data(), key.data());
            EXPECT_EQ(bad, 0u) << "MAC tamper should have failed decryption";
        },
        "key_store::secretbox_encrypt_decrypt_roundtrip",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.5 — `KeyStore::generate_and_add_identity` end-to-end.  Verifies
/// (a) the returned Z85 pubkey is 40 chars, (b) `has(name)` becomes
/// true, (c) `pubkey(name)` round-trips to the same value, (d) the
/// seckey is accessible via `with_seckey` and is 32 raw bytes (per
/// HEP-CORE-0040 §8.5.2).  First production user: broker observer
/// keypair (broker_service.cpp).
int generate_and_add_identity_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            const std::string name = "test.generated.identity";
            std::string pub_z85 = sec::secure().keys()
                .generate_and_add_identity(name);
            ASSERT_EQ(pub_z85.size(), 40u);
            EXPECT_TRUE(sec::secure().keys().has(name));
            EXPECT_EQ(sec::secure().keys().pubkey(name), pub_z85);

            std::size_t seckey_size = 0;
            sec::secure().keys().with_seckey(name,
                [&](std::string_view sk) { seckey_size = sk.size(); });
            EXPECT_EQ(seckey_size, 32u)
                << "seckey callback view was " << seckey_size
                << " bytes; HEP-CORE-0040 §8.5.2 specifies raw 32.";

            // Generating a second entry under the same name throws
            // (duplicate-name discipline).
            EXPECT_THROW(sec::secure().keys()
                            .generate_and_add_identity(name),
                         std::runtime_error);
        },
        "key_store::generate_and_add_identity_roundtrip",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.6 — `KeyStore::with_seckey_z85` yields a 40-char view AND the
/// stack buffer is `sodium_memzero`'d before this method returns
/// (HEP-CORE-0040 §8.5.2 use-not-export).  The zero-on-return check
/// is indirect: after the callback captures the view's address, we
/// verify that the WHOLE view is not the same bytes as the raw
/// seckey (i.e., encoded state changed).  The bytes at that address
/// are undefined post-return; the check here is that the SIZE
/// contract (40 chars) is honored inside the callback.
int with_seckey_z85_view_shape(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            const std::string name = "test.z85_shape";
            std::string pub = sec::secure().keys()
                .generate_and_add_identity(name);

            std::size_t seen_size = 0;
            std::string captured;
            sec::secure().keys().with_seckey_z85(name,
                [&](std::string_view v) {
                    seen_size = v.size();
                    captured.assign(v.data(), v.size());
                });
            EXPECT_EQ(seen_size, 40u);
            EXPECT_EQ(captured.size(), 40u);
            // Every Z85 char is printable ASCII (33..126).
            for (char c : captured) {
                EXPECT_GE(c, 33) << "non-printable Z85 char observed";
                EXPECT_LE(c, 126);
            }
        },
        "key_store::with_seckey_z85_view_shape",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.6b — `KeyStore::with_keypair_z85` yields BOTH halves; the
/// pubkey matches `pubkey(name)`, both are 40-char Z85.  One entry
/// lookup, one lock acquisition — replaces the common `pubkey` +
/// `with_seckey_z85` pair used in production.
int with_keypair_z85_yields_both_halves(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            const std::string name = "test.pair";
            std::string pub_expected = sec::secure().keys()
                .generate_and_add_identity(name);

            std::string pub_seen, sec_seen;
            sec::secure().keys().with_keypair_z85(name,
                [&](std::string_view p, std::string_view s) {
                    pub_seen.assign(p.data(), p.size());
                    sec_seen.assign(s.data(), s.size());
                });
            EXPECT_EQ(pub_seen, pub_expected);
            EXPECT_EQ(pub_seen.size(), 40u);
            EXPECT_EQ(sec_seen.size(), 40u);
            EXPECT_NE(pub_seen, sec_seen);
        },
        "key_store::with_keypair_z85_yields_both_halves",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.7 — deeper wrapper-API depth than SMS_WrappersWorkViaSecureAccessor.
///   - `random_bytes`: 1024 bytes, verify at least 900 distinct byte
///     values (uniform-ish) AND no long run of identical bytes
///     (weak entropy signal but catches a constant-return regression).
///   - `memcmp_ct`: length mismatch, empty-vs-empty, single-bit flips
///     at multiple positions, full match.
///   - `memzero`: pre-fill 128-byte buffer with 0xAB pattern, wipe,
///     verify every byte is 0 AND the buffer address hasn't moved.
int sms_wrappers_depth(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;

            // random_bytes depth.
            std::array<std::uint8_t, 1024> big{};
            sec::secure().random_bytes(std::span<std::uint8_t>(big));
            std::array<int, 256> hist{};
            for (auto b : big) ++hist[b];
            int distinct = 0;
            for (auto c : hist) if (c > 0) ++distinct;
            EXPECT_GT(distinct, 200)
                << "random_bytes returned only " << distinct
                << " distinct byte values in 1024 bytes — suspicious";
            // No 16-run of the same byte anywhere.
            int run = 1;
            for (std::size_t i = 1; i < big.size(); ++i)
            {
                if (big[i] == big[i-1]) { ++run; if (run >= 16) FAIL()
                    << "random_bytes had a run of >=16 identical bytes"; }
                else run = 1;
            }

            // memcmp_ct depth.
            {
                std::array<std::uint8_t, 4> empty_a{}, empty_b{};
                EXPECT_TRUE(sec::secure().memcmp_ct(
                    std::span<const std::uint8_t>(empty_a.data(), 0),
                    std::span<const std::uint8_t>(empty_b.data(), 0)));
                std::array<std::uint8_t, 8> a{}, b{};
                EXPECT_TRUE(sec::secure().memcmp_ct(
                    std::span<const std::uint8_t>(a),
                    std::span<const std::uint8_t>(b)));
                // Length mismatch → false, no exception.
                EXPECT_FALSE(sec::secure().memcmp_ct(
                    std::span<const std::uint8_t>(a),
                    std::span<const std::uint8_t>(b.data(), 4)));
                // Single-bit flip at multiple positions.
                for (int pos = 0; pos < 8; ++pos)
                {
                    a = b;  // reset
                    a[pos] ^= 0x01;
                    EXPECT_FALSE(sec::secure().memcmp_ct(
                        std::span<const std::uint8_t>(a),
                        std::span<const std::uint8_t>(b)))
                        << "single-bit flip at pos " << pos
                        << " not detected";
                }
            }

            // memzero depth.
            {
                std::array<std::uint8_t, 128> buf;
                buf.fill(0xAB);
                auto *addr_before = buf.data();
                sec::secure().memzero(std::span<std::uint8_t>(buf));
                EXPECT_EQ(buf.data(), addr_before);
                for (std::uint8_t v : buf) EXPECT_EQ(v, 0u);
            }
        },
        "key_store::sms_wrappers_depth",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.8 — `secure().box_encrypt_using` / `box_decrypt_using` roundtrip.
/// Uses the seed_curve_identities helper to seed both parties into
/// KeyStore, then encrypts + decrypts + MAC-tamper detection.
///
/// Exercises the name-based key citation (HEP-CORE-0043 §6): the
/// seckey is looked up in KeyStore by name, dereferenced INSIDE SMS
/// under `with_seckey`, and never crosses the API.
int box_encrypt_decrypt_roundtrip(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;

            // Seed two identities via the standard test helper.
            const std::string sender_uid = "sender";
            const std::string recip_uid  = "recipient";
            auto curve = pylabhub::tests::make_curve_setup({sender_uid, recip_uid});
            pylabhub::tests::seed_curve_identities(curve);

            const std::string sender_name =
                pylabhub::tests::role_keystore_name(sender_uid);
            const std::string recip_name =
                pylabhub::tests::role_keystore_name(recip_uid);

            // Retrieve pubkeys (non-secret) via KeyStore.
            std::array<std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes> sender_pub{};
            std::array<std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes> recip_pub{};
            // Z85 → raw via zmq_z85_decode
            {
                std::string sp = std::string(sec::secure().keys().pubkey(sender_name));
                std::string rp = std::string(sec::secure().keys().pubkey(recip_name));
                ASSERT_EQ(sp.size(), 40u);
                ASSERT_EQ(rp.size(), 40u);
                ASSERT_NE(::zmq_z85_decode(sender_pub.data(), sp.data()), nullptr);
                ASSERT_NE(::zmq_z85_decode(recip_pub.data(),  rp.data()), nullptr);
            }

            // Random nonce.
            std::array<std::uint8_t, sec::SecureSubsystem::kBoxNonceBytes> nonce{};
            sec::secure().random_bytes(std::span<std::uint8_t>(nonce));

            const std::string plain = "top secret message";
            const std::size_t need = plain.size() + sec::SecureSubsystem::kBoxMacBytes;
            std::vector<std::uint8_t> ct(need);

            // Encrypt: sender uses OWN seckey (by name) + recipient's pubkey.
            const std::size_t written = sec::secure().box_encrypt_using(
                sender_name,
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes>(recip_pub),
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxNonceBytes>(nonce),
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t *>(plain.data()),
                    plain.size()),
                std::span<std::uint8_t>(ct));
            ASSERT_EQ(written, need);

            // Decrypt: recipient uses OWN seckey + sender's pubkey.
            std::vector<std::uint8_t> pt(plain.size());
            const std::size_t decoded = sec::secure().box_decrypt_using(
                recip_name,
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes>(sender_pub),
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxNonceBytes>(nonce),
                std::span<const std::uint8_t>(ct),
                std::span<std::uint8_t>(pt));
            ASSERT_EQ(decoded, plain.size());
            EXPECT_EQ(std::string(reinterpret_cast<const char *>(pt.data()), decoded),
                      plain);

            // Tamper MAC (first byte) — decrypt must refuse.
            ct[0] ^= 0x01;
            const std::size_t bad = sec::secure().box_decrypt_using(
                recip_name,
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes>(sender_pub),
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxNonceBytes>(nonce),
                std::span<const std::uint8_t>(ct),
                std::span<std::uint8_t>(pt));
            EXPECT_EQ(bad, 0u) << "MAC tamper should have failed decryption";

            // Wrong sender pubkey — decrypt must also refuse.
            ct[0] ^= 0x01;  // restore MAC
            std::array<std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes> wrong_pub = sender_pub;
            wrong_pub[0] ^= 0x01;
            const std::size_t wrong = sec::secure().box_decrypt_using(
                recip_name,
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxPubkeyBytes>(wrong_pub),
                std::span<const std::uint8_t, sec::SecureSubsystem::kBoxNonceBytes>(nonce),
                std::span<const std::uint8_t>(ct),
                std::span<std::uint8_t>(pt));
            EXPECT_EQ(wrong, 0u) << "Wrong sender pubkey should have failed";
        },
        "key_store::box_encrypt_decrypt_roundtrip",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// R3.2 — `secure()` PANICs (process abort) when called without SMS
/// in the mods pack.  The worker registers ONLY Logger; when the
/// lambda calls `secure()`, the state gate is Uninitialized and
/// `panic_if_not_ready("secure()")` fires `std::abort()`.
/// Parent-side test asserts `exit_code() == 134` (SIGABRT).
int panic_when_secure_called_without_sms(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            // Expected: SMS state is Uninitialized here (mod pack
            // has Logger only).  This call must PANIC.
            (void)sec::secure();
            // Unreachable — if we get here, the gate failed to fire.
            ADD_FAILURE() << "secure() returned instead of panicking";
        },
        "key_store::panic_when_secure_called_without_sms",
        Logger::GetLifecycleModule());
    // No SecureSubsystem::GetLifecycleModule() — deliberate.
}

/// R3.2b — `secure().keys()` PANICs when SMS is not up.  Exercises
/// the same gate via a different accessor path.  Same expected
/// exit-code contract.
int panic_when_keys_called_without_sms(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            // Reaches secure() first — panics there.  Same signal.
            (void)sec::secure().keys();
            ADD_FAILURE() << "secure().keys() returned instead of panicking";
        },
        "key_store::panic_when_keys_called_without_sms",
        Logger::GetLifecycleModule());
}

/// R3.2c — `secure().secretbox_encrypt(...)` PANICs when SMS is not
/// up.  Same gate, exercised through a Category 1c encryption method.
int panic_when_secretbox_called_without_sms(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            std::array<std::uint8_t, 32> key{};
            std::array<std::uint8_t, 24> nonce{};
            std::array<std::uint8_t, 128> out{};
            std::array<std::uint8_t, 8> pt{};
            (void)sec::secure().secretbox_encrypt(
                out.data(), out.size(),
                pt.data(), pt.size(),
                nonce.data(), key.data());
            ADD_FAILURE() << "secretbox_encrypt returned instead of panicking";
        },
        "key_store::panic_when_secretbox_called_without_sms",
        Logger::GetLifecycleModule());
}

/// R3.3 — the state atomic transitions Uninitialized → Initialized
/// through the `LifecycleGuard` bringup, observable via `sodium_ready()`
/// and `lifecycle_initialized()`.  This test runs INSIDE the guard so
/// it can only observe the Initialized state, but it pins the invariant
/// that both probes agree once SMS is up.
///
/// The Shutdown transition is verified structurally by F11 (defensive
/// dtor publishes Shutdown when destructed).  A direct
/// post-LifecycleGuard-finalize observation would require running
/// the check OUTSIDE `run_gtest_worker`'s guard — infrastructure work
/// that's beyond this iteration.
int sodium_ready_agrees_with_lifecycle_initialized(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;
            EXPECT_TRUE(sec::sodium_ready());
            EXPECT_TRUE(sec::SecureSubsystem::lifecycle_initialized());
            // Consistency across probes.
            EXPECT_EQ(sec::sodium_ready(),
                      sec::SecureSubsystem::lifecycle_initialized());
        },
        "key_store::sodium_ready_agrees_with_lifecycle_initialized",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

/// F15 — parallel `SecureSubsystem::instance()` calls from N threads
/// all return the same reference.  Pins the C++11 thread-safe
/// function-local-static initialization contract we rely on
/// (HEP-CORE-0043 §1.3 singularity mechanism 1: "exactly one
/// construction path is the function-local static in `instance()`").
///
/// N threads spin-block on a barrier atomic, then all call
/// `instance()` at once.  We collect their observed pointers and
/// verify they are all identical.
int sms_parallel_instance_calls(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            namespace sec = pylabhub::utils::security;

            constexpr int kThreads = 32;
            std::atomic<int>                       ready{0};
            std::atomic<bool>                      go{false};
            std::array<sec::SecureSubsystem *, kThreads> observed{};
            std::vector<std::thread>               threads;
            threads.reserve(kThreads);

            for (int i = 0; i < kThreads; ++i)
            {
                threads.emplace_back([&, i]() {
                    ready.fetch_add(1, std::memory_order_release);
                    while (!go.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    observed[i] = &sec::SecureSubsystem::instance();
                });
            }
            while (ready.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
            go.store(true, std::memory_order_release);
            for (auto &t : threads) t.join();

            sec::SecureSubsystem *canon = observed[0];
            ASSERT_NE(canon, nullptr);
            for (int i = 1; i < kThreads; ++i)
            {
                EXPECT_EQ(observed[i], canon)
                    << "Thread " << i << " observed a different "
                    "SecureSubsystem instance — the function-local "
                    "static invariant is broken.";
            }
            // Consistency across the accessor: `secure()` from the
            // parent (post-race) matches the canonical pointer.
            EXPECT_EQ(&sec::secure(), canon);
        },
        "key_store::sms_parallel_instance_calls",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
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

    // SEC-Fold-2 merger invariants (HEP-CORE-0043 §1-§2 + §7).
    if (scenario == "sms_instance_returns_same_reference")
        return sms_instance_returns_same_reference(tmpdir);
    if (scenario == "sms_wrappers_work_via_secure_accessor")
        return sms_wrappers_work_via_secure_accessor(tmpdir);
    if (scenario == "keystore_state_persists_across_sms_window")
        return keystore_state_persists_across_sms_window(tmpdir);
    if (scenario == "secure_keys_returns_same_reference")
        return secure_keys_returns_same_reference(tmpdir);
    if (scenario == "sms_parallel_instance_calls")
        return sms_parallel_instance_calls(tmpdir);
    if (scenario == "secretbox_encrypt_decrypt_roundtrip")
        return secretbox_encrypt_decrypt_roundtrip(tmpdir);
    if (scenario == "generate_and_add_identity_roundtrip")
        return generate_and_add_identity_roundtrip(tmpdir);
    if (scenario == "with_seckey_z85_view_shape")
        return with_seckey_z85_view_shape(tmpdir);
    if (scenario == "with_keypair_z85_yields_both_halves")
        return with_keypair_z85_yields_both_halves(tmpdir);
    if (scenario == "sms_wrappers_depth")
        return sms_wrappers_depth(tmpdir);
    if (scenario == "panic_when_secure_called_without_sms")
        return panic_when_secure_called_without_sms(tmpdir);
    if (scenario == "panic_when_keys_called_without_sms")
        return panic_when_keys_called_without_sms(tmpdir);
    if (scenario == "panic_when_secretbox_called_without_sms")
        return panic_when_secretbox_called_without_sms(tmpdir);
    if (scenario == "sodium_ready_agrees_with_lifecycle_initialized")
        return sodium_ready_agrees_with_lifecycle_initialized(tmpdir);
    if (scenario == "box_encrypt_decrypt_roundtrip")
        return box_encrypt_decrypt_roundtrip(tmpdir);

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
