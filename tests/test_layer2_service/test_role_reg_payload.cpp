/**
 * @file test_role_reg_payload.cpp
 * @brief L2 unit tests for `build_producer_reg_payload` /
 *        `build_consumer_reg_payload` invariants.
 *
 * Pattern 1 — pure value-function tests; no LOGGER_*, no lifecycle.
 *
 * The wire-shape side of the contract (REG_REQ field shape per
 * HEP-CORE-0036 §5b + HEP-CORE-0041 §5.1) is exercised end-to-end by
 * the L3 broker tests.  This file pins the input-side invariants —
 * specifically the §5b transport discriminator: callers MUST set
 * exactly one of `is_zmq_transport` / `has_shm`, and the builder
 * MUST fail loudly if neither is set (the case that previously slipped
 * through to the broker as an INVALID_REQUEST with a misleading
 * wire-shape diagnostic instead of a config-pointing one).
 *
 * Origin: 2026-06-30 workflow code-review finding [4] CONFIRMED,
 * task #303.  Pre-fix the builder silently omitted `data_transport`
 * when both flags were false; broker rejected; role host treated
 * fatal and tore down — diagnostic pointed away from the actual
 * config mismatch (`shm.enabled=false` with `transport=="shm"` default).
 */

#include "utils/role_reg_payload.hpp"
#include "plh_version_registry.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using pylabhub::hub::ProducerRegInputs;
using pylabhub::hub::build_producer_reg_payload;

namespace
{

ProducerRegInputs make_baseline(bool has_shm, bool is_zmq_transport)
{
    ProducerRegInputs in;
    in.channel               = "test.channel";
    in.role_uid              = "prod.test.uid00000001";
    in.role_name             = "test_producer";
    in.role_type             = "producer";
    in.has_shm               = has_shm;
    in.is_zmq_transport      = is_zmq_transport;
    in.zmq_node_endpoint     = "tcp://127.0.0.1:5555";
    in.zmq_pubkey            = "01234567890123456789012345678901234567";
    in.shm_capability_endpoint = "unix:///tmp/test.cap.sock";
    return in;
}

// ── ZMQ branch happy path ───────────────────────────────────────────────────

TEST(RoleRegPayload, ZmqBranch_EmitsDataTransportZmq)
{
    auto in  = make_baseline(/*has_shm=*/false, /*is_zmq_transport=*/true);
    auto reg = build_producer_reg_payload(in);
    EXPECT_EQ(reg.value("data_transport", std::string{}), "zmq");
    EXPECT_EQ(reg.value("zmq_node_endpoint", std::string{}),
              "tcp://127.0.0.1:5555");
    EXPECT_FALSE(reg.contains("shm_capability_endpoint"));
}

// ── SHM branch happy path ───────────────────────────────────────────────────

TEST(RoleRegPayload, ShmBranch_EmitsDataTransportShm)
{
    auto in  = make_baseline(/*has_shm=*/true, /*is_zmq_transport=*/false);
    auto reg = build_producer_reg_payload(in);
    EXPECT_EQ(reg.value("data_transport", std::string{}), "shm");
    EXPECT_EQ(reg.value("shm_capability_endpoint", std::string{}),
              "unix:///tmp/test.cap.sock");
    EXPECT_FALSE(reg.contains("zmq_node_endpoint"));
}

// ── §5b discriminator invariant: neither flag set MUST throw ────────────────
//
// Mutation pin: revert the `else { throw }` arm in role_reg_payload.hpp
// → this test fails (no exception) AND the L3 broker tests fail on
// INVALID_REQUEST with a wire-shape error message that points away
// from the real config issue.  This local guard surfaces the
// config-side root cause directly.

TEST(RoleRegPayload, NeitherTransportFlag_ThrowsLogicError)
{
    auto in = make_baseline(/*has_shm=*/false, /*is_zmq_transport=*/false);
    EXPECT_THROW(build_producer_reg_payload(in), std::logic_error);
}

TEST(RoleRegPayload, NeitherTransportFlag_DiagnosticPointsAtConfig)
{
    auto in = make_baseline(/*has_shm=*/false, /*is_zmq_transport=*/false);
    try
    {
        (void) build_producer_reg_payload(in);
        FAIL() << "Expected std::logic_error";
    }
    catch (const std::logic_error &e)
    {
        const std::string msg = e.what();
        // The diagnostic must point at the config layer (shm.enabled
        // and transport fields), NOT at the wire field `data_transport`.
        // This is what makes the error message actionable for operators.
        EXPECT_NE(msg.find("shm.enabled"), std::string::npos)
            << "Diagnostic must name the `shm.enabled` config field — got: "
            << msg;
        EXPECT_NE(msg.find("transport"), std::string::npos)
            << "Diagnostic must name the `transport` config field — got: "
            << msg;
    }
}

// ── HEP-CORE-0032 §8 ABI fingerprint wire binding ──────────────────────
//
// Task #326.  Pins the shape and roundtrip of the `abi_fingerprint`
// wire field carried on REG_REQ / CONSUMER_REG_REQ per §8.2.  A
// refactor that renames a field or omits the envelope breaks these
// tests immediately.

using pylabhub::hub::ConsumerRegInputs;
using pylabhub::hub::build_consumer_reg_payload;

ConsumerRegInputs make_consumer_baseline()
{
    ConsumerRegInputs in;
    in.channel   = "test.channel";
    in.role_uid  = "cons.test.uid00000001";
    in.role_name = "test_consumer";
    in.zmq_pubkey = "01234567890123456789012345678901234567";
    return in;
}

TEST(RoleRegPayload, ProducerReg_CarriesAbiFingerprintEnvelope)
{
    auto reg = build_producer_reg_payload(
        make_baseline(/*has_shm=*/false, /*is_zmq_transport=*/true));

    ASSERT_TRUE(reg.contains("abi_fingerprint"))
        << "REG_REQ MUST carry `abi_fingerprint` per HEP-CORE-0032 §8.2";
    ASSERT_TRUE(reg["abi_fingerprint"].is_object());

    // Every REQUIRED axis field per §8.2 must be present.  Refactor
    // that drops one of these silently breaks §8.5 verify_peer_versions
    // classification — pin the whole set.
    const auto &fp = reg["abi_fingerprint"];
    EXPECT_TRUE(fp.contains("library_major"));
    EXPECT_TRUE(fp.contains("library_minor"));
    EXPECT_TRUE(fp.contains("library_rolling"));
    EXPECT_TRUE(fp.contains("shm_major"));
    EXPECT_TRUE(fp.contains("shm_minor"));
    EXPECT_TRUE(fp.contains("broker_proto_major"));
    EXPECT_TRUE(fp.contains("broker_proto_minor"));
    EXPECT_TRUE(fp.contains("zmq_frame_major"));
    EXPECT_TRUE(fp.contains("zmq_frame_minor"));
    EXPECT_TRUE(fp.contains("script_api_major"));
    EXPECT_TRUE(fp.contains("script_api_minor"));
    EXPECT_TRUE(fp.contains("script_engine_major"));
    EXPECT_TRUE(fp.contains("script_engine_minor"));
    EXPECT_TRUE(fp.contains("config_major"));
    EXPECT_TRUE(fp.contains("config_minor"));
}

TEST(RoleRegPayload, ConsumerReg_CarriesAbiFingerprintEnvelope)
{
    auto reg = build_consumer_reg_payload(make_consumer_baseline());

    ASSERT_TRUE(reg.contains("abi_fingerprint"))
        << "CONSUMER_REG_REQ MUST carry `abi_fingerprint` per §8.2";
    ASSERT_TRUE(reg["abi_fingerprint"].is_object());

    // Same 15-field envelope check.  Refactor divergence between
    // producer + consumer builders is a silent-bug class this catches.
    const auto &fp = reg["abi_fingerprint"];
    EXPECT_TRUE(fp.contains("library_major"));
    EXPECT_TRUE(fp.contains("shm_major"));
    EXPECT_TRUE(fp.contains("broker_proto_major"));
    EXPECT_TRUE(fp.contains("zmq_frame_major"));
    EXPECT_TRUE(fp.contains("script_api_major"));
    EXPECT_TRUE(fp.contains("script_engine_major"));
    EXPECT_TRUE(fp.contains("config_major"));
}

TEST(RoleRegPayload, ProducerReg_AbiFingerprintMatchesCurrentComponentVersions)
{
    // Round-trip: envelope on the wire must parse back to the same
    // ComponentVersions the running library reports via current().
    // A refactor that hardcodes a value or drifts from current()
    // breaks this pin.
    auto reg = build_producer_reg_payload(
        make_baseline(/*has_shm=*/false, /*is_zmq_transport=*/true));
    const auto parsed = pylabhub::version::from_json_object(
        reg["abi_fingerprint"]);
    const auto cur = pylabhub::version::current();

    EXPECT_EQ(parsed.library_major,       cur.library_major);
    EXPECT_EQ(parsed.library_minor,       cur.library_minor);
    EXPECT_EQ(parsed.library_rolling,     cur.library_rolling);
    EXPECT_EQ(parsed.shm_major,           cur.shm_major);
    EXPECT_EQ(parsed.shm_minor,           cur.shm_minor);
    EXPECT_EQ(parsed.broker_proto_major,  cur.broker_proto_major);
    EXPECT_EQ(parsed.broker_proto_minor,  cur.broker_proto_minor);
    EXPECT_EQ(parsed.zmq_frame_major,     cur.zmq_frame_major);
    EXPECT_EQ(parsed.zmq_frame_minor,     cur.zmq_frame_minor);
    EXPECT_EQ(parsed.script_api_major,    cur.script_api_major);
    EXPECT_EQ(parsed.script_api_minor,    cur.script_api_minor);
    EXPECT_EQ(parsed.script_engine_major, cur.script_engine_major);
    EXPECT_EQ(parsed.script_engine_minor, cur.script_engine_minor);
    EXPECT_EQ(parsed.config_major,        cur.config_major);
    EXPECT_EQ(parsed.config_minor,        cur.config_minor);
}

TEST(RoleRegPayload, ProducerReg_BuildIdIsSiblingNotNested)
{
    // §8.2 last paragraph — build_id is a SIBLING string field next
    // to abi_fingerprint, not nested inside it.  This lets a receiver
    // omit the build_id check independently of the envelope check.
    // Refactor that nests build_id inside the envelope silently
    // breaks the receiver-side comparator contract.
    auto reg = build_producer_reg_payload(
        make_baseline(/*has_shm=*/false, /*is_zmq_transport=*/true));

    EXPECT_FALSE(reg["abi_fingerprint"].contains("build_id"))
        << "build_id MUST NOT live inside the abi_fingerprint envelope; "
        << "it's a sibling top-level field per HEP-CORE-0032 §8.2.";

    // Conditional: present iff the library reports one.  This mirrors
    // the emit-side logic in role_reg_payload.hpp.
    if (const char *bid = pylabhub::version::build_id())
    {
        EXPECT_TRUE(reg.contains("build_id"))
            << "Library reports build_id='" << bid
            << "' — REG_REQ must carry the sibling field.";
        EXPECT_EQ(reg.value("build_id", std::string{}),
                  std::string(bid));
    }
    else
    {
        EXPECT_FALSE(reg.contains("build_id"))
            << "Library has no build_id — REG_REQ MUST NOT emit the "
            << "sibling field with an empty value (would confuse the "
            << "broker's `nullptr → skip build_id check` rule).";
    }
}

TEST(RoleRegPayload, ConsumerReg_BuildIdIsSiblingNotNested)
{
    // Same sibling-field contract on CONSUMER_REG_REQ.  This catches
    // the same drift class as the producer test above but on the
    // consumer path.
    auto reg = build_consumer_reg_payload(make_consumer_baseline());

    EXPECT_FALSE(reg["abi_fingerprint"].contains("build_id"));

    if (const char *bid = pylabhub::version::build_id())
    {
        EXPECT_TRUE(reg.contains("build_id"));
        EXPECT_EQ(reg.value("build_id", std::string{}),
                  std::string(bid));
    }
    else
    {
        EXPECT_FALSE(reg.contains("build_id"));
    }
}

} // anon
