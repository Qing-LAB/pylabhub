/**
 * @file test_key_store.cpp
 * @brief Pattern 3 driver — KeyStore L2 test suite (HEP-CORE-0040 §170).
 *
 * Each TEST_F spawns a fresh subprocess that owns its own LifecycleGuard
 * (Logger).  Subprocess isolation is required because
 * `SecureSubsystem` and `KeyStore` are PROCESS SINGLETONS (HEP-0040
 * §4.1 + §5.1) — running multiple scenarios serially in one process
 * would have the second ctor throw "already constructed."
 *
 * Worker bodies in `workers/key_store_workers.cpp`.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class KeyStoreTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::string unique_dir(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_keystore_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

// F13 — SMS-invariant tests live under their own fixture (not KeyStoreTest)
// so their subject is unambiguous.  Same subprocess dispatch surface as
// KeyStoreTest — shares the same tmpdir hygiene + worker binary — but
// each TEST_F reports under `SecureSubsystemTest.*` so a `-R
// SecureSubsystem` filter selects the SMS surface directly.
class SecureSubsystemTest : public KeyStoreTest
{
};

} // namespace

// ── Environmental smoke (runs FIRST by declaration order) ─────────────────
//
// Catches a broken libsodium guarded-allocator before the KeyStore
// scenarios fire and produce a confusing failure storm with cryptic
// library-internal SIGABRT messages.  If THIS fails, the environment
// is broken; downstream KeyStore failures are downstream consequences,
// not distinct regressions.  See worker docstring for rationale.
TEST_F(KeyStoreTest, SodiumSmoke_AllocatorWorksForKeyStoreSizes)
{
    auto w = SpawnWorker("key_store.sodium_smoke",
                         {unique_dir("sodium_smoke")});
    ExpectWorkerOk(w);
}

// ── Lifecycle / singleton invariants ───────────────────────────────────────
//
// Post-SEC-Fold-2 (HEP-CORE-0043 §2.2) KeyStore is a MEMBER of
// SecureSubsystem — the four retired tests below tested contract that
// no longer exists STRUCTURALLY:
//   - `OrderingCheck_KeyStoreRequiresSecureMemory` — KeyStore ctor is
//     private (F8); reached only via `SecureSubsystem::Impl`'s member-
//     init.  No external ordering to check.
//   - `SecondConstruction_Throws` — KeyStore ctor is private; second
//     construction is a compile error, not a runtime throw.
//   - `KeyStoreNotInitialized_Throws` — the `key_store()` free-function
//     shim was deleted in Phase D.  Access is `secure().keys()`, which
//     PANICs on the SMS state gate; the panic path is exercised by the
//     `SecureSubsystemTest` suite (see below).
//   - `KeyStoreReady_ProbeLifecycle` — `key_store_ready()` was deleted
//     alongside the shim.  `sodium_ready()` is the sole readiness probe.
// The invariants these tests pinned are now enforced at compile time
// (private ctor) + gate-panic (accessor).

// ── Identity-key happy paths ───────────────────────────────────────────────

TEST_F(KeyStoreTest, AddIdentity_PubkeyAndWithSeckeyRoundtrip)
{
    auto w = SpawnWorker(
        "key_store.add_identity_then_pubkey_and_with_seckey_roundtrip",
        {unique_dir("identity_roundtrip")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, AddIdentity_ZerosSource)
{
    auto w = SpawnWorker("key_store.add_identity_zeros_source",
                         {unique_dir("zeros_source")});
    ExpectWorkerOk(w);
}

// ── Error paths — missing / wrong-type / duplicate / wrong-size ────────────

TEST_F(KeyStoreTest, Pubkey_OnMissing_Throws)
{
    auto w = SpawnWorker("key_store.pubkey_on_missing_throws",
                         {unique_dir("pubkey_missing")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, WithSeckey_OnMissing_Throws)
{
    auto w = SpawnWorker("key_store.with_seckey_on_missing_throws",
                         {unique_dir("seckey_missing")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, LookupRaw_OnMissing_Throws)
{
    auto w = SpawnWorker("key_store.lookup_raw_on_missing_throws",
                         {unique_dir("raw_missing")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, Pubkey_OnRawEntry_Throws)
{
    auto w = SpawnWorker("key_store.pubkey_on_raw_entry_throws",
                         {unique_dir("pubkey_raw_type")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, WithSeckey_OnRawEntry_Throws)
{
    auto w = SpawnWorker("key_store.with_seckey_on_raw_entry_throws",
                         {unique_dir("seckey_raw_type")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, AddIdentity_Duplicate_Throws)
{
    auto w = SpawnWorker("key_store.add_identity_duplicate_throws",
                         {unique_dir("duplicate")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, AddIdentity_WrongSize_Throws)
{
    auto w = SpawnWorker("key_store.add_identity_wrong_size_throws",
                         {unique_dir("wrong_size")});
    ExpectWorkerOk(w);
}

// ── Raw secret (HEP-0038) happy path ───────────────────────────────────────

TEST_F(KeyStoreTest, AddRaw_LookupRawRoundtrip)
{
    auto w = SpawnWorker("key_store.add_raw_then_lookup_raw_roundtrip",
                         {unique_dir("raw_roundtrip")});
    ExpectWorkerOk(w);
}

// ── remove + has / size ────────────────────────────────────────────────────

TEST_F(KeyStoreTest, Remove_MakesSubsequentLookupThrow)
{
    auto w = SpawnWorker("key_store.remove_makes_subsequent_lookup_throw",
                         {unique_dir("remove")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, HasAndSize_TrackEntries)
{
    auto w = SpawnWorker("key_store.has_and_size_track_entries",
                         {unique_dir("has_size")});
    ExpectWorkerOk(w);
}

// ── Thread-safety — parallel reads + remove vs in-flight callback ──────────

TEST_F(KeyStoreTest, ParallelReads_DoNotBlock)
{
    auto w = SpawnWorker("key_store.parallel_reads_do_not_block",
                         {unique_dir("parallel_reads")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, Remove_BlocksBehindInFlightWithSeckey)
{
    auto w = SpawnWorker(
        "key_store.remove_blocks_behind_in_flight_with_seckey",
        {unique_dir("remove_blocks")});
    ExpectWorkerOk(w);
}

// ── SecureBuffer<N> ────────────────────────────────────────────────────────

TEST_F(KeyStoreTest, SecureBuffer_DtorZeroes)
{
    auto w = SpawnWorker("key_store.secure_buffer_dtor_zeroes",
                         {unique_dir("secure_buffer")});
    ExpectWorkerOk(w);
}

// ── SEC-Fold-2 merger invariants (HEP-CORE-0043 §1-§2 + §7) ────────────────
// Fixture: SecureSubsystemTest — SMS-specific invariants surface under
// their own class so test filters (`-R SecureSubsystem`) select this
// group cleanly.  Worker bodies still live in `key_store_workers.cpp`
// because they share the same subprocess dispatch surface.

TEST_F(SecureSubsystemTest, InstanceReturnsSameReference)
{
    auto w = SpawnWorker("key_store.sms_instance_returns_same_reference",
                         {unique_dir("sms_instance")});
    ExpectWorkerOk(w);
}

TEST_F(SecureSubsystemTest, WrappersWorkViaSecureAccessor)
{
    auto w = SpawnWorker("key_store.sms_wrappers_work_via_secure_accessor",
                         {unique_dir("sms_wrappers")});
    ExpectWorkerOk(w);
}

TEST_F(SecureSubsystemTest, KeyStoreState_PersistsAcrossSMSWindow)
{
    auto w = SpawnWorker("key_store.keystore_state_persists_across_sms_window",
                         {unique_dir("state_persists")});
    ExpectWorkerOk(w);
}

TEST_F(SecureSubsystemTest, SecureKeys_ReturnsSameReference)
{
    auto w = SpawnWorker("key_store.secure_keys_returns_same_reference",
                         {unique_dir("secure_keys_ref")});
    ExpectWorkerOk(w);
}

// F15 — Parallel `SecureSubsystem::instance()` calls from N threads
// all return the same reference.  Pins the C++11 thread-safe
// function-local-static initialization contract we rely on
// (HEP-CORE-0043 §1.3 singularity mechanism 1).
TEST_F(SecureSubsystemTest, ParallelInstanceCallsReturnSameReference)
{
    auto w = SpawnWorker("key_store.sms_parallel_instance_calls",
                         {unique_dir("sms_parallel")});
    ExpectWorkerOk(w);
}

// R3.1 — `secretbox_encrypt/decrypt` roundtrip (HEP-CORE-0043 §2.1 Cat 1c).
TEST_F(SecureSubsystemTest, SecretboxEncryptDecrypt_Roundtrip)
{
    auto w = SpawnWorker("key_store.secretbox_encrypt_decrypt_roundtrip",
                         {unique_dir("secretbox_roundtrip")});
    ExpectWorkerOk(w);
}

// R3.7 — deeper wrapper-API coverage (random distribution sanity,
// memcmp_ct single-bit flips, memzero pattern verify).
TEST_F(SecureSubsystemTest, Wrappers_Depth)
{
    auto w = SpawnWorker("key_store.sms_wrappers_depth",
                         {unique_dir("sms_wrappers_depth")});
    ExpectWorkerOk(w);
}

// R3.5 — `generate_and_add_identity` end-to-end (broker observer path).
TEST_F(KeyStoreTest, GenerateAndAddIdentity_Roundtrip)
{
    auto w = SpawnWorker("key_store.generate_and_add_identity_roundtrip",
                         {unique_dir("generate_identity")});
    ExpectWorkerOk(w);
}

// R3.6 — `with_seckey_z85` view shape (40-char Z85 printable).
TEST_F(KeyStoreTest, WithSeckeyZ85_ViewShape)
{
    auto w = SpawnWorker("key_store.with_seckey_z85_view_shape",
                         {unique_dir("seckey_z85_shape")});
    ExpectWorkerOk(w);
}

// R3.6b — `with_keypair_z85` yields both halves consistently.
TEST_F(KeyStoreTest, WithKeypairZ85_YieldsBothHalves)
{
    auto w = SpawnWorker("key_store.with_keypair_z85_yields_both_halves",
                         {unique_dir("keypair_z85")});
    ExpectWorkerOk(w);
}

// R3.8 — `box_encrypt_using` / `box_decrypt_using` roundtrip via
// name-based key citation (HEP-CORE-0043 §6).  Seckey never crosses
// the API boundary; peer pubkey passed as raw bytes.
TEST_F(SecureSubsystemTest, BoxEncryptDecrypt_Roundtrip)
{
    auto w = SpawnWorker("key_store.box_encrypt_decrypt_roundtrip",
                         {unique_dir("box_roundtrip")});
    ExpectWorkerOk(w);
}

// R3.2 — the ONLY gated accessor is `secure().keys()` — it panics
// (process abort → exit code 134 = 128 + SIGABRT) when SMS is not in
// the mods pack.  All other SMS methods (`secure()`,
// `random_bytes`, `compute_blake2b`, `secretbox_encrypt`,
// `pwhash_argon2id`, ...) are stateless wrappers on libsodium that
// libsodium self-initializes; they succeed without SMS bringup.
// Gate policy documented in `secure_subsystem.cpp` "Gate policy"
// block.
namespace  // anon — SIGABRT expected-exit-code check
{
constexpr int kSigabrtExitCode = 128 + 6;  // 134

void expect_sigabrt(pylabhub::tests::helper::WorkerProcess &w,
                    const char *accessor)
{
    w.wait_for_exit();
    EXPECT_EQ(w.exit_code(), kSigabrtExitCode)
        << "Expected " << accessor << " to panic (SIGABRT = exit "
        << kSigabrtExitCode << "), got exit " << w.exit_code();
    // The panic message must reference the state gate + accessor name.
    // No completion markers required (worker aborted mid-lambda).
    const std::string err = w.get_stderr();
    EXPECT_NE(err.find("SecureSubsystem::"), std::string::npos)
        << "SIGABRT'd worker's stderr missing the SMS panic tag";
    EXPECT_NE(err.find("expected Initialized"), std::string::npos)
        << "SIGABRT'd worker's stderr missing the state-gate message";
}
} // namespace

TEST_F(SecureSubsystemTest, KeysAccessor_PanicsWhenSmsNotInPack)
{
    auto w = SpawnWorker("key_store.panic_when_keys_called_without_sms",
                         {unique_dir("panic_keys")});
    expect_sigabrt(w, "secure().keys()");
}

// R3.9 — `pwhash_argon2id` end-to-end: deterministic under (password,
// salt); different password OR different salt → different key; null
// arguments return false.  The three null-pointer sub-cases each emit
// one ERROR log line — expected substrings are declared so the fixture
// doesn't treat them as unexpected regressions.
TEST_F(SecureSubsystemTest, PwhashArgon2id_Roundtrip)
{
    auto w = SpawnWorker("key_store.pwhash_argon2id_roundtrip",
                         {unique_dir("pwhash_argon2id")});
    ExpectWorkerOk(w, {}, {
        "pwhash_argon2id: null pointer argument",
        "pwhash_argon2id: null pointer argument",
        "pwhash_argon2id: null pointer argument",
    });
}

// R3.10 — `derive_pwhash_salt` is DETERMINISTIC.  Load-bearing for
// vault reproducibility: if this ever gained entropy, existing vault
// files would silently become undecryptable.
TEST_F(SecureSubsystemTest, DerivePwhashSalt_Deterministic)
{
    auto w = SpawnWorker("key_store.derive_pwhash_salt_deterministic",
                         {unique_dir("pwhash_salt_det")});
    ExpectWorkerOk(w, {}, {
        "derive_pwhash_salt: null output pointer",
    });
}

// R3.11 — `bin2hex` produces lowercase hex + null terminator; empty
// input is safe; null pointers must not crash.
TEST_F(SecureSubsystemTest, Bin2Hex_Roundtrip)
{
    auto w = SpawnWorker("key_store.bin2hex_roundtrip",
                         {unique_dir("bin2hex")});
    ExpectWorkerOk(w, {}, {
        "bin2hex: null pointer argument",
        "bin2hex: null pointer argument",
    });
}

// R3.12 — `verify_blake2b` accepts the correct hash, rejects tamper.
TEST_F(SecureSubsystemTest, VerifyBlake2b_Roundtrip)
{
    auto w = SpawnWorker("key_store.verify_blake2b_roundtrip",
                         {unique_dir("verify_blake2b")});
    ExpectWorkerOk(w, {}, {
        "verify_blake2b: null pointer argument",
        "verify_blake2b: null pointer argument",
    });
}

// R3.3/R3.4 — `sodium_ready()` and `SecureSubsystem::lifecycle_initialized()`
// agree once SMS is up.  Both probes read the same atomic; a divergence
// would signal a state-machine drift.
TEST_F(SecureSubsystemTest, SodiumReady_AgreesWith_LifecycleInitialized)
{
    auto w = SpawnWorker(
        "key_store.sodium_ready_agrees_with_lifecycle_initialized",
        {unique_dir("probe_agreement")});
    ExpectWorkerOk(w);
}
