/**
 * @file test_admin_session.cpp
 * @brief Sealed admin-console session identity (HEP-CORE-0033 §11.0.5).
 *
 * Pattern 1+ (BinaryLifecycleEnvironment) — needs Logger + SecureSubsystem
 * up so `secure()` / KeyStore are live.  Pins the design contract:
 *   - a session id round-trips only when the message's observed connection
 *     facts match the sealed facts (anti-hijack);
 *   - a tampered id, a foreign-instance key, and malformed input are all
 *     rejected (the AEAD seal is the integrity boundary);
 *   - `origin_uid` is the stable provenance stamp.
 *
 * Derived from the design (§11.0.5), not from current behaviour: the
 * assertions encode what the contract promises (fact-bound, sealed,
 * per-instance), so a regression that dropped the fact check or the seal
 * would fail here.
 */

#include "utils/admin_session.hpp"

#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/logger.hpp"

#include "binary_lifecycle.h"

#include <gtest/gtest.h>

#include <string>

using pylabhub::admin::AdminSessionFacts;
using pylabhub::admin::ensure_session_seal_key;
using pylabhub::admin::kAdminSessionSealKeyName;
using pylabhub::admin::open_session_id;
using pylabhub::admin::origin_uid;
using pylabhub::admin::seal_session_id;
using pylabhub::admin::verify_session_id;

PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

namespace
{

AdminSessionFacts make_facts()
{
    return AdminSessionFacts{/*label=*/"alice-laptop",
                             /*peer_address=*/"127.0.0.1:52012",
                             /*routing_id=*/"R-abc123",
                             /*issued_at_ms=*/1721000000000ULL};
}

class AdminSessionTest : public ::testing::Test
{
protected:
    void SetUp() override { ensure_session_seal_key(); }
};

// Round-trip: verify succeeds only against the facts the id was sealed with,
// and returns them intact (all four fields).
TEST_F(AdminSessionTest, RoundTrip_SealThenVerify_ReturnsSameFacts)
{
    const auto f = make_facts();
    const std::string id = seal_session_id(f);
    ASSERT_FALSE(id.empty());

    const auto got = verify_session_id(id, f.peer_address, f.routing_id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->label, f.label);
    EXPECT_EQ(got->peer_address, f.peer_address);
    EXPECT_EQ(got->routing_id, f.routing_id);
    EXPECT_EQ(got->issued_at_ms, f.issued_at_ms);
}

// Anti-hijack: a valid id replayed from a different connection (peer or
// routing id) is rejected, even though the seal itself opens fine.
TEST_F(AdminSessionTest, WrongPeer_Rejected_ButRawOpenSucceeds)
{
    const auto f = make_facts();
    const std::string id = seal_session_id(f);

    EXPECT_TRUE(open_session_id(id).has_value()); // seal is intact...
    EXPECT_FALSE(verify_session_id(id, "10.0.0.9:1", f.routing_id).has_value());
}

TEST_F(AdminSessionTest, WrongRouting_Rejected)
{
    const auto f = make_facts();
    const std::string id = seal_session_id(f);
    EXPECT_FALSE(verify_session_id(id, f.peer_address, "R-other").has_value());
}

// The AEAD tag is the integrity boundary: a single flipped hex nibble fails
// to open.
TEST_F(AdminSessionTest, Tampered_Rejected)
{
    const auto f = make_facts();
    std::string id = seal_session_id(f);
    ASSERT_FALSE(id.empty());
    char &last = id.back();
    last = (last == 'a') ? 'b' : 'a';
    EXPECT_FALSE(open_session_id(id).has_value());
}

// Per-instance sealing: an id sealed under one instance's key does not open
// after the key is rotated (simulating a hub restart / different instance).
TEST_F(AdminSessionTest, ForeignInstanceKey_Rejected)
{
    const auto f = make_facts();
    const std::string id = seal_session_id(f);

    pylabhub::utils::security::secure().keys().remove(kAdminSessionSealKeyName);
    ensure_session_seal_key(); // fresh random per-instance key

    EXPECT_FALSE(open_session_id(id).has_value());
}

TEST_F(AdminSessionTest, MalformedInput_Rejected)
{
    EXPECT_FALSE(open_session_id("").has_value());     // decodes shorter than nonce+MAC
    EXPECT_FALSE(open_session_id("zzz").has_value());  // odd length
    EXPECT_FALSE(open_session_id("zzzz").has_value()); // non-hex characters
}

TEST_F(AdminSessionTest, OriginUid_StableStamp)
{
    EXPECT_EQ(origin_uid(make_facts()),
              "alice-laptop@127.0.0.1:52012#1721000000000");
}

} // namespace
