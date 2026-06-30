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

} // anon
