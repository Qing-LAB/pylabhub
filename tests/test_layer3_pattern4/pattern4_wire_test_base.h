#pragma once
/**
 * @file pattern4_wire_test_base.h
 * @brief Shared base fixture for Pattern 4 broker-wire tests.
 *
 * Pattern 4 = multi-process wire-protocol test — a `BrokerService`
 * subprocess (see `workers/pattern4_broker_protocol_workers.cpp`, which
 * is generic and profile-driven) plus a parent that drives the wire via
 * `BrokerWireClient`.  This base collects the wire helpers that every
 * broker-wire parent test needs so each concern (broker protocol, schema,
 * consumer, metrics, …) gets its own thin driver file instead of
 * duplicating the helpers.
 *
 * Origin: the HubHostBrokerHandle antipattern sweep
 * (`docs/README/README_testing.md` line 565 + HEP-CORE-0036 §7.4
 * single-pumper invariant), task #52.
 *
 * GTest note: fatal-assertion macros (`ASSERT_*`) inside these methods
 * `return;` from the METHOD, not the calling test.  Callers MUST wrap
 * invocations in `ASSERT_NO_FATAL_FAILURE(...)` so a failed REG /
 * HEARTBEAT aborts the test at the correct call site rather than
 * cascading into a misattributed downstream failure.
 */

#include "broker_wire_client.h"
#include "pattern4_helpers.h"
#include "test_patterns.h"

#include "plh_platform.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/security/shm_capability_channel.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pylabhub::tests::pattern4
{

class Pattern4WireTest : public pylabhub::tests::IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::filesystem::path make_test_temp_dir(std::string_view label)
    {
        auto dir = make_temp_dir(label);
        paths_to_clean_.push_back(dir);
        return dir;
    }

    /// Construct a BrokerWireClient masquerading as `role_uid`.  role_uid
    /// becomes the DEALER routing_id per I-DEALER-IDENTITY (HEP-CORE-0046
    /// §8.1); the keypair comes from the shared curve setup so the parent
    /// never touches the process KeyStore (BrokerWireClient is
    /// KeyStore-independent by design).
    BrokerWireClient make_wire_client(zmq::context_t &ctx, const Pattern4Setup &setup,
                                      const std::string &role_uid)
    {
        const auto &kp = setup.curve.role(role_uid);
        BrokerWireClient::Config c;
        c.broker_endpoint = setup.broker_endpoint;
        c.broker_pubkey = setup.curve.hub.public_z85;
        c.client_pubkey = kp.public_z85;
        c.client_seckey = kp.secret_z85;
        c.client_role_uid = role_uid;
        return BrokerWireClient(ctx, c);
    }

    /// Pure REG_REQ body builder (no wire I/O, no assertions).  Lets
    /// error-path tests inspect the reply and success-path tests add
    /// extra body fields (schema_hash, …).  Pubkey comes from the
    /// shared curve setup; `shm=true` selects the SHM capability
    /// transport, else ZMQ on an unused port.
    nlohmann::json producer_reg_body(const Pattern4Setup &setup, const std::string &channel,
                                     const std::string &uid, bool shm,
                                     const std::string &topology = {})
    {
        namespace sec = pylabhub::utils::security;
        pylabhub::hub::ProducerRegInputs in;
        in.channel = channel;
        in.role_uid = uid;
        in.role_name = "test_producer";
        in.role_type = "producer";
        in.channel_topology = topology;
        in.zmq_pubkey = setup.curve.role(uid).public_z85;
        if (shm)
        {
            in.has_shm = true;
            in.shm_capability_endpoint = sec::default_shm_capability_endpoint(channel);
        }
        else
        {
            in.is_zmq_transport = true;
            in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
        }
        return pylabhub::hub::build_producer_reg_payload(in);
    }

    /// Pure CONSUMER_REG_REQ body builder.  `data_transport` is the
    /// REQUIRED transport declaration the broker arbitrates against the
    /// channel's stored transport (HEP-CORE-0036 §5b.6; the retired
    /// `consumer_queue_type` field is no longer read).  Default "zmq"
    /// mirrors the make_cons_opts default; transport-arbitration tests
    /// pass the mismatching / matching value explicitly.
    nlohmann::json consumer_reg_body(const Pattern4Setup &setup, const std::string &channel,
                                     const std::string &uid, const std::string &topology = {},
                                     const std::string &data_transport = "zmq")
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel = channel;
        in.role_uid = uid;
        in.role_name = "test_consumer";
        in.role_type = "consumer";
        in.data_transport = data_transport;
        in.channel_topology = topology;
        in.zmq_pubkey = setup.curve.role(uid).public_z85;
        return pylabhub::hub::build_consumer_reg_payload(in);
    }

    /// Drain unsolicited frames from `client` until one of msg_type
    /// `want` arrives (returns its body) or the budget elapses (returns
    /// nullopt).  Non-matching frames (e.g. an interleaved
    /// CHANNEL_AUTH_CHANGED_NOTIFY) are discarded.  Used to observe
    /// broker-pushed NOTIFYs on a registered member's DEALER without a
    /// request/reply pairing.
    std::optional<nlohmann::json> drain_for(BrokerWireClient &client, std::string_view want,
                                            std::chrono::milliseconds budget)
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + budget;
        while (true)
        {
            const auto now = clock::now();
            if (now >= deadline)
                return std::nullopt;
            auto frame = client.receive(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (!frame)
                return std::nullopt;
            if (frame->first == want)
                return frame->second;
        }
    }

    /// Producer REG_REQ → assert REG_ACK.status=="success".  ZMQ
    /// transport (Pattern 4 convention); the pubkey is the role's curve
    /// identity so ZAP authorizes and the broker records a valid
    /// producer_pubkey (HEP-CORE-0036 §5b.4).  Writes the REG_ACK body
    /// to `*out_ack` when non-null so shape tests can inspect it.
    void register_producer(BrokerWireClient &client, const Pattern4Setup &setup,
                           const std::string &channel, const std::string &uid,
                           nlohmann::json *out_ack = nullptr, const std::string &topology = {})
    {
        auto reply = client.request("REG_REQ",
                                    producer_reg_body(setup, channel, uid, /*shm=*/false, topology),
                                    "REG_ACK", std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "REG_REQ timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "REG_REQ failed for " << uid << "; body=" << reply->dump();
        if (out_ack != nullptr)
            *out_ack = *reply;
    }

    /// Confirm the roster `reg_ack` delivered, so the hub will disclose
    /// this role's inbox coordinates to the roles that version names
    /// (HEP-CORE-0035 §4.9.7, I-INBOX-REACHABLE).
    ///
    /// A live role does this itself: `RoleAPIBase` reports its version on
    /// every adoption and on every tick.  A raw wire client has neither, so
    /// it sends the same `ROSTER_CHECK_NOTIFY` explicitly — this is the
    /// production message, not a test-only shortcut.
    ///
    /// **Call ordering matters.**  Confirm AFTER every role that must reach
    /// this one has registered, because what is being confirmed is the
    /// version this role's REG_ACK carried, and only roles admitted at or
    /// before that version are covered by it.
    ///
    /// The trailing ROLE_PRESENCE_REQ is a barrier, not a query.
    /// ROSTER_CHECK_NOTIFY is fire-and-forget, so nothing else would tell
    /// the caller the broker had consumed it; a reply on the SAME
    /// connection does, because the broker takes one DEALER's messages in
    /// order.  Without it the following ROLE_INFO_REQ — on a different
    /// connection — could overtake the confirmation and fail by timing.
    void confirm_roster(BrokerWireClient &client, const std::string &uid,
                        const nlohmann::json &reg_ack)
    {
        nlohmann::json check;
        check["role_uid"] = uid;
        check["known_roles_version"] = reg_ack.value("known_roles_version", std::uint64_t{0});
        client.send("ROSTER_CHECK_NOTIFY", check);

        nlohmann::json probe;
        probe["role_uid"] = uid;
        auto barrier = client.request("ROLE_PRESENCE_REQ", probe, "ROLE_PRESENCE_ACK",
                                      std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(barrier.has_value())
            << "barrier after ROSTER_CHECK_NOTIFY timed out for " << uid
            << " — the broker did not answer on the connection that carried the confirmation";
    }

    /// Producer HEARTBEAT_NOTIFY (fire-and-forget) → clears the R6
    /// producer-kLive gate that CONSUMER_REG_REQ trips (HEP-CORE-0036
    /// §5.2 / HEP-CORE-0023 §2.5.3).  The old BRC `register_consumer`
    /// masked this with an implicit CHANNEL_NOT_READY retry loop; the
    /// raw wire client gets one reply, so the heartbeat is explicit —
    /// which is what the design requires anyway.  Sleeps briefly so the
    /// broker processes the heartbeat before the consumer REG arrives.
    void producer_heartbeat(BrokerWireClient &client, const std::string &channel,
                            const std::string &uid)
    {
        nlohmann::json hb;
        hb["channel_name"] = channel;
        hb["role_uid"] = uid;
        hb["role_type"] = "producer";
        hb["producer_pid"] = pylabhub::platform::get_pid();
        client.send("HEARTBEAT_NOTIFY", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    /// Consumer CONSUMER_REG_REQ → assert CONSUMER_REG_ACK.status==
    /// "success".  ZMQ transport, curve-identity pubkey for the
    /// channel-scope allowlist (HEP-CORE-0036 §6.5).  Writes the ACK to
    /// `*out_ack` when non-null.  Caller MUST have registered the
    /// producer AND sent a producer heartbeat first (R6 gate).
    void register_consumer(BrokerWireClient &client, const Pattern4Setup &setup,
                           const std::string &channel, const std::string &uid,
                           nlohmann::json *out_ack = nullptr, const std::string &topology = {})
    {
        auto reply =
            client.request("CONSUMER_REG_REQ", consumer_reg_body(setup, channel, uid, topology),
                           "CONSUMER_REG_ACK", std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "CONSUMER_REG_REQ timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "CONSUMER_REG_REQ failed for " << uid << "; body=" << reply->dump();
        if (out_ack != nullptr)
            *out_ack = *reply;
    }

    /// Fan-in PRODUCER join: REG_REQ with topology="fan-in" AND the
    /// schema material matching `register_fanin_owner`'s contract
    /// (SI-3/SI-4: a producer joining a format-carrying channel must
    /// present the matching format).
    void register_fanin_producer(BrokerWireClient &client, const Pattern4Setup &setup,
                                 const std::string &channel, const std::string &uid,
                                 nlohmann::json *out_ack = nullptr)
    {
        auto body = producer_reg_body(setup, channel, uid, /*shm=*/false, "fan-in");
        apply_matching_producer_schema(body);
        auto reply = client.request("REG_REQ", body, "REG_ACK",
                                    std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "REG_REQ (fan-in producer) timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "fan-in producer join failed for " << uid << "; body=" << reply->dump();
        if (out_ack != nullptr)
            *out_ack = *reply;
    }

    /// Fan-in OWNER open: CONSUMER_REG_REQ with topology="fan-in" AND
    /// the mandatory owner citation (SI-1 Option B — a material-free
    /// fan-in open is rejected SCHEMA_REQUIRED; SI-2 — the citation is
    /// self-consistent by construction via `apply_owner_citation`).
    void register_fanin_owner(BrokerWireClient &client, const Pattern4Setup &setup,
                              const std::string &channel, const std::string &uid,
                              nlohmann::json *out_ack = nullptr)
    {
        auto body = consumer_reg_body(setup, channel, uid, /*topology=*/"fan-in");
        apply_owner_citation(body);
        auto reply = client.request("CONSUMER_REG_REQ", body, "CONSUMER_REG_ACK",
                                    std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "CONSUMER_REG_REQ (fan-in owner) timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "fan-in owner open failed for " << uid << "; body=" << reply->dump();
        if (out_ack != nullptr)
            *out_ack = *reply;
    }

    std::vector<std::filesystem::path> paths_to_clean_;
};

} // namespace pylabhub::tests::pattern4
