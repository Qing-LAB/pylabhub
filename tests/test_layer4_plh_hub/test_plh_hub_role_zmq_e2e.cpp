/**
 * @file test_plh_hub_role_zmq_e2e.cpp
 * @brief AUTH-7 — L4 end-to-end ZMQ-transport auth-gated data flow.
 *
 * Sibling of `test_plh_hub_role_shm_e2e.cpp`.  Same scenario shape
 * (authorized data flow + unauthorized denial), ZMQ transport.
 *
 * Why ZMQ alongside SHM.  HEP-CORE-0036 §I11 + §5.2 spec the data-
 * plane CURVE gate as transport-transparent: a consumer reaches the
 * `Live` data-flow state iff its keypair is in the producer's
 * allowlist; the broker enforces this at REG / CONSUMER_REG.  The
 * SHM e2e (#258) pins this for SHM; this file pins it for ZMQ.
 * Together they close AUTH-7 — "data flows iff consumer is in
 * producer's allowlist; data stops iff consumer dereg."
 *
 * Markers used (all transport-AGNOSTIC, defined in role_api_base /
 * broker_service):
 *   - hub:  `Broker: listening on tcp://...`
 *           `event=RegReqAccepted role='<prod>' channel='<chan>'`
 *           `event=ConsumerRegReqAccepted role='<cons>' channel='<chan>'`
 *   - role: `event=RegAckReceived ... status=success`
 *           `event=ConsumerRegAckReceived ... status=success`
 *           `event=PresenceStateTransition ... to=Live`
 *   - data: `cons_test: complete N=<n>` (script-side count)
 *
 * No SHM-specific markers (`ShmCapabilityTransportBound` et al.) —
 * the L4 e2e pins the protocol contract, not transport internals.
 *
 * Doc anchors:
 *   - `docs/HEP/HEP-CORE-0036-Channel-Auth.md` §I11 (e2e flow)
 *   - `docs/README/README_testing.md` § "L4 end-to-end binary-driven
 *     tests"
 *   - `tests/test_layer4_plh_hub/role_e2e_harness.h` (shared helpers)
 */
#include "plh_hub_fixture.h"
#include "role_e2e_harness.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;
using pylabhub::tests::plh_role_e2e::read_hub_log;
using pylabhub::tests::plh_role_e2e::wait_for_hub_marker;
using pylabhub::tests::plh_role_e2e::read_role_log;
using pylabhub::tests::plh_role_e2e::wait_for_role_marker;
using pylabhub::tests::plh_role_e2e::extract_bound_endpoint;
using pylabhub::tests::plh_role_e2e::plh_role_binary;
using pylabhub::tests::plh_role_e2e::keygen_role_and_read_pubkey;
using pylabhub::tests::plh_role_e2e::add_known_role;

namespace
{

// ─── Producer config + script (ZMQ TX) ──────────────────────────────────────
//
// Config shape mirrors share/py-demo-single-processor-zmq/producer/
// producer.json with two adjustments for test isolation:
//   - ephemeral port (tcp://127.0.0.1:0) so concurrent test runs
//     don't fight over a fixed port; producer binds + publishes the
//     resolved endpoint via ENDPOINT_UPDATE_REQ (HEP-CORE-0021 §16).
//   - tight target_period_ms so the 5-slot run completes in <1 s.

void write_zmq_producer_config(const fs::path &cfg_path,
                                const fs::path &hub_dir,
                                const std::string &uid,
                                const std::string &channel,
                                int prod_port,
                                const std::string &channel_topology = "")
{
    nlohmann::json j;
    j["producer"]["uid"]       = uid;
    j["producer"]["name"]      = "L4ZmqProducer";
    j["producer"]["log_level"] = "info";
    j["producer"]["auth"]["keyfile"] = "vault/" + uid + ".vault";

    j["out_hub_dir"]      = hub_dir.string();
    j["out_channel"]      = channel;
    // 2026-07-08 topology migration — declare fan-in / fan-out explicitly
    // for tests that need those cardinalities.  Empty = don't set the
    // field; broker defaults to one-to-one per HEP-CORE-0018 §5.3.
    if (!channel_topology.empty())
    {
        j["out_channel_topology"] = channel_topology;
    }
    j["loop_timing"]      = "fixed_rate";
    j["target_period_ms"] = 50;

    // ZMQ TX.  Producer's bind direction comes from `out_channel_topology`
    // per HEP-CORE-0017 §3.3.0 (writer + one-to-one or fan-out →
    // binding; writer + fan-in → dialing).  The `out_zmq_bind` legacy
    // field is no longer read by `build_tx_queue`.  Endpoint hint is
    // used on the binding side only; dialing side receives its dial
    // target on REG_ACK.
    j["out_transport"]            = "zmq";
    j["out_zmq_endpoint"]         = "tcp://127.0.0.1:" + std::to_string(prod_port);
    j["out_zmq_buffer_depth"]     = 256;
    j["out_zmq_overflow_policy"]  = "drop";

    j["out_slot_schema"]["packing"] = "aligned";
    j["out_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_flexzone_schema"] = nullptr;

    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = true;
    j["stop_on_script_error"] = false;
    j["script"]["type"]       = "python";
    j["script"]["path"]       = ".";

    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    std::ofstream f(cfg_path);
    f << j.dump(2);
}

/// Producer script — writes slots in a LOOP (cycles 0..N-1
/// indefinitely) until SIGTERM.  The loop is necessary because ZMQ
/// PUSH/PULL drops messages sent before the consumer's CURVE/ZAP
/// admission completes (the producer's PUSH allowlist arrives via
/// the CHANNEL_AUTH_CHANGED_NOTIFY → GET_CHANNEL_AUTH chain about
/// ~110 ms AFTER the consumer's PULL goes Active — see
/// HEP-CORE-0036 §6.5).  The SHM e2e doesn't have this issue
/// because SHM is in-memory random-access.  `prod_test:` log prefix
/// preserved for parity with the SHM script.
void write_zmq_producer_script(const fs::path &script_dir, int n_slots)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_N_SLOTS = " << n_slots << "\n"
      << "_iter = [0]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'prod_test: init')\n"
         "\n"
         "def on_produce(tx, msgs, api):\n"
         "    if tx.slot is None:\n"
         "        return False\n"
         "    n = _iter[0] % _N_SLOTS\n"
         "    tx.slot.value = float(n)\n"
         "    # Log only the first 2*N iterations to avoid log bloat.\n"
         "    if _iter[0] < 2 * _N_SLOTS:\n"
         "        api.log('info', 'prod_test: wrote slot N=' + str(n) +\n"
         "                ' iter=' + str(_iter[0]))\n"
         "    _iter[0] += 1\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'prod_test: stop iter=' + str(_iter[0]))\n";
}

// ─── Consumer config + script (ZMQ RX) ──────────────────────────────────────
//
// Consumer learns the producer's resolved ZMQ endpoint via the
// CONSUMER_REG_ACK `producers[]` array (HEP-0036 §5b.7 — unified
// across SHM + ZMQ).  No `in_zmq_endpoint` field — the role host
// fills it from the ACK.

void write_zmq_consumer_config(const fs::path &cfg_path,
                                const fs::path &hub_dir,
                                const std::string &uid,
                                const std::string &channel,
                                const std::string &channel_topology = "")
{
    nlohmann::json j;
    j["consumer"]["uid"]       = uid;
    j["consumer"]["name"]      = "L4ZmqConsumer";
    j["consumer"]["log_level"] = "info";
    j["consumer"]["auth"]["keyfile"] = "vault/" + uid + ".vault";

    j["in_hub_dir"]       = hub_dir.string();
    j["in_channel"]       = channel;
    // 2026-07-08 topology migration — same convention as producer helper.
    if (!channel_topology.empty())
    {
        j["in_channel_topology"] = channel_topology;
    }
    j["loop_timing"]      = "fixed_rate";
    j["target_period_ms"] = 50;

    j["in_transport"]           = "zmq";
    // Consumer's bind direction comes from `in_channel_topology` per
    // HEP-CORE-0017 §3.3.0 (reader + fan-in → binding; reader +
    // fan-out or one-to-one → dialing).  The `in_zmq_bind` legacy
    // field is no longer read by `build_rx_queue`.  Endpoint is a
    // placeholder — dialing side ignores it (peer arrives on
    // REG_ACK.initial_allowlist); binding side (fan-in consumer)
    // uses it as a bind hint and `port=0` means "any free port,"
    // the resolved port is published via ENDPOINT_UPDATE_REQ.  The
    // config parser at `transport_config.hpp:96-99` currently
    // requires the field regardless of side (known gap); until it
    // grows a topology-aware check, the placeholder satisfies both
    // roles.
    j["in_zmq_endpoint"]        = "tcp://127.0.0.1:0";
    j["in_zmq_buffer_depth"]    = 256;
    j["in_zmq_overflow_policy"] = "drop";

    j["in_slot_schema"]["packing"] = "aligned";
    j["in_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });

    j["checksum"]             = "manual";
    j["stop_on_script_error"] = false;
    j["script"]["type"]       = "python";
    j["script"]["path"]       = ".";

    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    std::ofstream f(cfg_path);
    f << j.dump(2);
}

/// Multi-producer variant of `write_zmq_producer_script`.  Each
/// producer's script writes a producer-specific value range so the
/// consumer can distinguish which producer a received slot came
/// from: `value = value_offset + (iter % n_slots)`.  Distinct
/// non-overlapping offsets (e.g. 0 vs 100) let the consumer verify
/// slots arrived from BOTH producers, not just one — the load-
/// bearing assertion for fan-in coverage per HEP-CORE-0042 §7.1.
///
/// Otherwise byte-identical to the single-producer variant: infinite
/// write loop (necessary because ZMQ PUSH/PULL drops pre-attach
/// slots per HEP-CORE-0036 §6.5), `prod_test:` log prefix, first
/// 2*N iterations logged then quiet.
void write_zmq_producer_script_with_offset(const fs::path &script_dir,
                                            int n_slots,
                                            int value_offset)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_N_SLOTS = " << n_slots << "\n"
      << "_OFFSET  = " << value_offset << "\n"
      << "_iter = [0]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'prod_test: init offset=' + str(_OFFSET))\n"
         "\n"
         "def on_produce(tx, msgs, api):\n"
         "    if tx.slot is None:\n"
         "        return False\n"
         "    n = _iter[0] % _N_SLOTS\n"
         "    tx.slot.value = float(_OFFSET + n)\n"
         "    if _iter[0] < 2 * _N_SLOTS:\n"
         "        api.log('info', 'prod_test: wrote slot N=' + str(n) +\n"
         "                ' value=' + str(_OFFSET + n) +\n"
         "                ' iter=' + str(_iter[0]))\n"
         "    _iter[0] += 1\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'prod_test: stop iter=' + str(_iter[0]))\n";
}

/// Multi-producer variant of `write_zmq_consumer_script`.  The
/// consumer tracks which per-producer VALUE RANGES it has observed
/// via a script-local `seen_offsets` set derived from
/// `int(value // 100)`.  When (a) `expected_slots` total slots have
/// arrived AND (b) the observed offset set matches
/// `required_offsets` (comma-separated string of ints), emits
/// `cons_test: complete N=<n> offsets_seen=<sorted_list>`.
///
/// This design pins the HEP-CORE-0042 §7.1 fan-in contract at L4:
/// the loop admitted N producers → data ACTUALLY flows from all N,
/// not just from a subset.  A regression that admits N producers
/// but only wires one into the queue fails here even if slot count
/// alone reaches the target.
void write_zmq_multi_producer_consumer_script(const fs::path &script_dir,
                                               int expected_slots,
                                               const std::string &required_offsets)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_EXPECTED  = " << expected_slots << "\n"
      << "_REQUIRED  = set(int(x) for x in \"" << required_offsets
      <<   "\".split(',') if x)\n"
      << "_received  = [0]\n"
         "_seen      = set()\n"
         "_done      = [False]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'cons_test: init required_offsets=' +\n"
         "                    ','.join(str(x) for x in sorted(_REQUIRED)))\n"
         "\n"
         "def on_consume(rx, msgs, api):\n"
         "    if rx.slot is None:\n"
         "        return True\n"
         "    _received[0] += 1\n"
         "    off = int(rx.slot.value // 100) * 100\n"
         "    _seen.add(off)\n"
         "    api.log('info', 'cons_test: read slot N=' + str(_received[0]) +\n"
         "                    ' value=' + str(rx.slot.value) +\n"
         "                    ' offset=' + str(off))\n"
         "    if (_received[0] >= _EXPECTED and\n"
         "        _seen >= _REQUIRED and not _done[0]):\n"
         "        _done[0] = True\n"
         "        api.log('info', 'cons_test: complete N=' + str(_received[0]) +\n"
         "                        ' offsets_seen=' +\n"
         "                        ','.join(str(x) for x in sorted(_seen)))\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'cons_test: stop offsets_seen=' +\n"
         "                    ','.join(str(x) for x in sorted(_seen)))\n";
}

/// Consumer script — counts slots received, emits
/// `cons_test: complete N=<n>` when target reached.  Same shape as
/// the SHM e2e consumer script.
void write_zmq_consumer_script(const fs::path &script_dir,
                                int expected_slots)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_EXPECTED = " << expected_slots << "\n"
      << "_received = [0]\n"
         "_done = [False]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'cons_test: init')\n"
         "\n"
         "def on_consume(rx, msgs, api):\n"
         "    if rx.slot is None:\n"
         "        return True\n"
         "    _received[0] += 1\n"
         "    api.log('info', 'cons_test: read slot N=' + str(_received[0]) +\n"
         "                    ' value=' + str(rx.slot.value))\n"
         "    if _received[0] >= _EXPECTED and not _done[0]:\n"
         "        _done[0] = True\n"
         "        api.log('info', 'cons_test: complete N=' + str(_received[0]))\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'cons_test: stop')\n";
}

// ─── Processor config + script (ZMQ two-channel) ─────────────────────────
//
// Processor has BOTH `in_channel` and `out_channel`.  This helper
// mirrors the producer/consumer helpers above but writes the
// two-channel processor config shape (see
// `share/py-demo-single-processor-zmq/processor/processor.json`),
// with topology declared per-side so the broker's admission path
// sees fan-in / fan-out / one-to-one consistently on both channels.
//
// The topology args pin the SIDE resolution the processor's
// `RoleAPIBase` must get right for `finalize_channel_connect`,
// `handle_channel_auth_notifies`, and `channel_admission_populated`
// (HEP-CORE-0036 §I9.1 layer contract).  The load-bearing test
// below uses (in="fan-in", out="fan-in"): processor binds PULL on
// IN and dials PUSH on OUT, so the pre-cleanup channel-to-writer
// resolution would have wrongly returned OUT's tx_queue for a
// `finalize_channel_connect(in_channel)` call → CHECK_PEER_READY_REQ
// against the WRONG channel → broker rejects with
// NOT_A_ROLE_OF_CHANNEL → processor aborts startup.  Under the
// post-cleanup resolution the IN call is a legitimate no-op and
// the OUT call polls its own channel.
void write_zmq_processor_config(const fs::path &cfg_path,
                                 const fs::path &hub_dir,
                                 const std::string &uid,
                                 const std::string &in_channel,
                                 const std::string &out_channel,
                                 const std::string &in_topology,
                                 const std::string &out_topology,
                                 int in_port_hint,
                                 int out_port_hint)
{
    nlohmann::json j;
    j["processor"]["uid"]       = uid;
    j["processor"]["name"]      = "L4ZmqProcessor";
    j["processor"]["log_level"] = "info";
    j["processor"]["auth"]["keyfile"] = "vault/" + uid + ".vault";

    j["in_hub_dir"]     = hub_dir.string();
    j["out_hub_dir"]    = hub_dir.string();
    j["in_channel"]     = in_channel;
    j["out_channel"]    = out_channel;
    if (!in_topology.empty())  j["in_channel_topology"]  = in_topology;
    if (!out_topology.empty()) j["out_channel_topology"] = out_topology;
    j["loop_timing"]      = "fixed_rate";
    j["target_period_ms"] = 25;

    // Endpoints are placeholders per the same convention as the
    // producer / consumer helpers: binding side uses port=0 for
    // "any free port" (resolved endpoint published via
    // ENDPOINT_UPDATE_REQ per HEP-CORE-0021 §16); dialing side
    // ignores the field (target arrives on REG_ACK).
    j["in_transport"]           = "zmq";
    j["in_zmq_endpoint"]        = "tcp://127.0.0.1:" +
                                    std::to_string(in_port_hint);
    j["in_zmq_buffer_depth"]    = 256;
    j["in_zmq_overflow_policy"] = "drop";

    j["out_transport"]           = "zmq";
    j["out_zmq_endpoint"]        = "tcp://127.0.0.1:" +
                                     std::to_string(out_port_hint);
    j["out_zmq_buffer_depth"]    = 256;
    j["out_zmq_overflow_policy"] = "drop";

    // Single-field schemas — value in / value out — for symmetry with
    // the producer + consumer helpers above.  The processor forwards
    // input value + 1000 so the consumer script can verify that data
    // truly flowed through processor (not directly producer→consumer).
    j["in_slot_schema"]["packing"] = "aligned";
    j["in_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_slot_schema"]["packing"] = "aligned";
    j["out_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });

    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = false;
    j["stop_on_script_error"] = false;
    j["script"]["type"]       = "python";
    j["script"]["path"]       = ".";

    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    std::ofstream f(cfg_path);
    f << j.dump(2);
}

/// Processor script — forwards each input slot to the output side
/// with `value += PROC_MARK`.  `proc_test:` prefix distinguishes
/// processor lifecycle events from producer/consumer lines in the
/// merged post-mortem dump.
void write_zmq_processor_script(const fs::path &script_dir,
                                 float proc_mark)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_MARK = " << proc_mark << "\n"
      << "_iter = [0]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'proc_test: init mark=' + str(_MARK))\n"
         "\n"
         "def on_process(rx, tx, messages, api):\n"
         "    if rx.slot is None or tx.slot is None:\n"
         "        return False\n"
         "    v = float(rx.slot.value) + _MARK\n"
         "    tx.slot.value = v\n"
         "    if _iter[0] < 20:\n"
         "        api.log('info', 'proc_test: forwarded iter=' + str(_iter[0]) +\n"
         "                        ' in=' + str(rx.slot.value) +\n"
         "                        ' out=' + str(v))\n"
         "    _iter[0] += 1\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'proc_test: stop iter=' + str(_iter[0]))\n";
}

} // namespace

// ─── Scenario A: authorized producer + consumer over ZMQ ────────────────────
//
// Verifies the full AUTH-7 happy path:
//   - hub init / keygen / known_roles for 2 roles
//   - producer REG_REQ accepted, first heartbeat → kLive
//   - consumer CONSUMER_REG_REQ accepted, unified ACK shape
//   - consumer dials producer's ZMQ_CURVE endpoint, handshake succeeds
//   - N slots flow producer → consumer
//   - clean SIGTERM shutdown, no [ERROR ] in any log
//
// DEFERRED 2026-06-30 (handoff #246).  The ZMQ data-plane has a
// known sequencing race vs the consumer's CURVE/ZAP admission: the
// broker sends `CONSUMER_REG_ACK` to the consumer (consumer dials
// producer) BEFORE the producer's CHANNEL_AUTH_CHANGED_NOTIFY →
// GET_CHANNEL_AUTH chain seeds the producer's PUSH allowlist with
// the new consumer's pubkey.  libzmq's initial CURVE handshake
// fails on the empty allowlist and the consumer never receives
// data (observed: producer allowlist update fires ~110 ms AFTER
// consumer PULL goes Active).  HEP-CORE-0036 §6.5 already specifies
// the pre-confirm fix; #246 implements it.  Until #246 ships, the
// scenario is skipped.  The deny scenario below works without
// depending on this gate (broker denies CONSUMER_REG before any
// dial).

TEST_F(PlhHubCliTest, ZmqE2E_AuthorizedConsumerReceivesAllSlots)
{
    // Un-skipped 2026-07-02 as part of task #246 Phase 3 close-out
    // review remediation.  The HEP-CORE-0042 ZMQ pre-attach coord-
    // ination chain (producer emission Phase 3a + consumer §7.1
    // loop Phase 3b) is now production-live and this test is the
    // designated L4 happy-path pin for §7.1 (consumer registers,
    // pre-attaches, dials, receives data).
    using std::chrono::seconds;

    const std::string channel  = "lab.l4.zmq.e2e.a";
    const std::string prod_uid = "prod.l4zmq.uid12345678";
    const std::string cons_uid = "cons.l4zmq.uid12345678";
    constexpr int kSlots = 5;
    // Producer port: fixed (HEP-CORE-0021 §16.5 ephemeral-binding
    // not yet wired — #94).  Use 19000 + pid % 1000 to keep
    // concurrent test runs from colliding.
    const int prod_port = 19000 + (::getpid() % 1000);

    // ── Hub init + keygen + ZAP install ───────────────────────────────────
    const fs::path hub_dir = tmp("zmqe2e_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ZmqE2EHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "zmqe2e-hub-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }
    ASSERT_TRUE(fs::exists(hub_dir / "hub.pubkey"));

    // ── Producer + consumer keygen + known_roles registration ─────────────
    const fs::path prod_dir = tmp("zmqe2e_prod");
    const fs::path cons_dir = tmp("zmqe2e_cons");
    std::error_code ec;
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(cons_dir / "vault", ec);

    write_zmq_producer_config(prod_dir / "producer.json", hub_dir,
                               prod_uid, channel, prod_port);
    write_zmq_producer_script(prod_dir / "script" / "python", kSlots);
    write_zmq_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, channel);
    write_zmq_consumer_script(cons_dir / "script" / "python", kSlots);

    ::setenv("PYLABHUB_ROLE_PASSWORD", "zmqe2e-role-pw", /*overwrite=*/1);

    const std::string prod_pubkey =
        keygen_role_and_read_pubkey(prod_dir, "producer", prod_uid,
                                     "zmqe2e-role-pw");
    const std::string cons_pubkey =
        keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                     "zmqe2e-role-pw");

    add_known_role(hub_dir, "zmqe2e_prod", prod_uid, "producer", prod_pubkey);
    add_known_role(hub_dir, "zmqe2e_cons", cons_uid, "consumer", cons_pubkey);

    // ── Spawn hub run-mode + grab bound endpoint ──────────────────────────
    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);

    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // ── Producer spawn ────────────────────────────────────────────────────
    WorkerProcess prod(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});

    auto dump_prod = [&](const std::string &where) -> std::string {
        std::string s;
        s += "[fail at: " + where + "]\n";
        s += "── producer log file ──\n" + read_role_log(prod_dir) + "\n";
        s += "── producer stderr ──\n" + prod.get_stderr() + "\n";
        s += "── hub log ──\n" + read_hub_log(hub_dir) + "\n";
        return s;
    };

    // Hub-side REG_REQ accepted for producer (HEP-0036 §5b canonical
    // wire schema applied; CURVE handshake passed at the ZAP gate).
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=RegReqAccepted role='" + prod_uid + "' channel='" + channel + "'",
        seconds(7)))
        << dump_prod("RegReqAccepted (hub side) — REG_REQ chain");

    // Producer observes the REG_ACK; producer-side allowlist seeded.
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=RegAckReceived", seconds(5)))
        << dump_prod("RegAckReceived (producer side)");

    // ── Consumer spawn ────────────────────────────────────────────────────
    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});

    auto dump_full = [&](const std::string &where) -> std::string {
        std::string s;
        s += "[fail at: " + where + "]\n";
        s += "── producer log file ──\n" + read_role_log(prod_dir) + "\n";
        s += "── producer stderr ──\n" + prod.get_stderr() + "\n";
        s += "── consumer log file ──\n" + read_role_log(cons_dir) + "\n";
        s += "── consumer stderr ──\n" + cons.get_stderr() + "\n";
        s += "── hub log ──\n" + read_hub_log(hub_dir) + "\n";
        return s;
    };

    // Consumer REG_REQ accepted by hub (HEP-0036 §5.2 R6 producer-kLive
    // gate satisfied; producer's heartbeat advanced first).
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=ConsumerRegReqAccepted role='" + cons_uid +
        "' channel='" + channel + "'", seconds(7)))
        << dump_full("ConsumerRegReqAccepted — consumer REG_REQ chain");

    // Consumer observes the unified CONSUMER_REG_ACK (HEP-0036 §5b.7);
    // the `producers[]` array carries the producer's resolved
    // ZMQ endpoint + pubkey.
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=ConsumerRegAckReceived", seconds(5)))
        << dump_full("ConsumerRegAckReceived (consumer side)");

    // ── Post-migration observability markers ───────────────────────────
    //
    // Under HEP-CORE-0017 §3.3.0 the one-to-one topology puts the
    // producer on the binding side (PUSH bind) and the consumer on
    // the dialing side (PULL connect).  There is no "pre-attach
    // loop" — the consumer's queue drives Standby → Configured →
    // Active in one step via `apply_master_approval(CONSUMER_REG_ACK)`.
    // The observability markers below verify the topology-model
    // path actually ran; a regression that broke it would fail here
    // instead of silently limping along on a legacy fallback.
    //
    // Producer-side: instance_id captured + APPLIED_REQ round-trip
    // completed (HEP-CORE-0042 §3a — unchanged under topology model).
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=ProducerInstanceIdCaptured channel='" + channel + "'",
        seconds(5)))
        << dump_full("event=ProducerInstanceIdCaptured — producer processed REG_ACK");
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=ChannelAuthApplied channel='" + channel + "'",
        seconds(5)))
        << dump_full("event=ChannelAuthApplied — producer's allowlist "
                     "applied + APPLIED_REQ RTT closed");

    // ── Data flow verification: consumer received all N slots ─────────────
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "cons_test: complete N=" + std::to_string(kSlots), seconds(10)))
        << dump_full("cons_test: complete N=" + std::to_string(kSlots) +
                     " — data flow");

    // ── Shutdown ──────────────────────────────────────────────────────────
    cons.send_signal(SIGTERM);
    EXPECT_EQ(cons.wait_for_exit(10), 0)
        << "consumer did not exit cleanly on SIGTERM.\n"
        << cons.get_stderr();

    prod.send_signal(SIGTERM);
    EXPECT_EQ(prod.wait_for_exit(10), 0)
        << "producer did not exit cleanly on SIGTERM.\n"
        << prod.get_stderr();

    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0)
        << "hub did not exit cleanly on SIGTERM.\n" << hub.get_stderr();

    // ── Class-D gate: no [ERROR ] in any log ──────────────────────────────
    auto contains_error = [](const std::string &s) {
        return s.find("[ERROR ]") != std::string::npos;
    };
    const std::string hub_log = read_hub_log(hub_dir);
    EXPECT_FALSE(contains_error(hub_log))
        << "hub log [ERROR ]:\n" << hub_log;
    EXPECT_FALSE(contains_error(prod.get_stderr()))
        << "producer stderr [ERROR ]:\n" << prod.get_stderr();
    EXPECT_FALSE(contains_error(cons.get_stderr()))
        << "consumer stderr [ERROR ]:\n" << cons.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}

// ─── Mutation pin (denial scenario) ─────────────────────────────────────────
//
// Same setup as Scenario A but the consumer is NOT added to
// known_roles.  Expectation: the consumer's CTRL CURVE handshake
// fails at the broker's ZAP gate (or CONSUMER_REG_REQ is rejected
// at Layer-2 known-role check).  Either way the consumer MUST NOT
// observe `event=ConsumerRegAckReceived` and MUST NOT receive any
// slot.  This is the regression pin for "removing consumer from
// known_roles must make AUTH-7 fail."

TEST_F(PlhHubCliTest, ZmqE2E_UnauthorizedConsumerDeniedByBroker)
{
    using std::chrono::seconds;

    const std::string channel  = "lab.l4.zmq.e2e.deny";
    const std::string prod_uid = "prod.l4zmq.uid87654321";
    const std::string cons_uid = "cons.l4zmq.uid87654321";
    // Distinct port range from scenario A (19000..) to avoid
    // collision when the two tests run back-to-back with overlapping
    // socket TIME_WAIT.
    const int prod_port = 20000 + (::getpid() % 1000);

    const fs::path hub_dir = tmp("zmqe2e_deny_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ZmqDenyHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "zmqe2e-deny-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }

    const fs::path prod_dir = tmp("zmqe2e_deny_prod");
    const fs::path cons_dir = tmp("zmqe2e_deny_cons");
    std::error_code ec;
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(cons_dir / "vault", ec);

    write_zmq_producer_config(prod_dir / "producer.json", hub_dir,
                               prod_uid, channel, prod_port);
    write_zmq_producer_script(prod_dir / "script" / "python", 5);
    write_zmq_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, channel);
    write_zmq_consumer_script(cons_dir / "script" / "python", 5);

    ::setenv("PYLABHUB_ROLE_PASSWORD", "zmqe2e-deny-role-pw", /*overwrite=*/1);

    const std::string prod_pubkey =
        keygen_role_and_read_pubkey(prod_dir, "producer", prod_uid,
                                     "zmqe2e-deny-role-pw");
    // Consumer keygens normally but is NOT added to known_roles —
    // the mutation point.  Producer IS added so its registration
    // chain runs to completion.
    (void)keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                       "zmqe2e-deny-role-pw");

    add_known_role(hub_dir, "zmqe2e_deny_prod", prod_uid, "producer",
                   prod_pubkey);
    // NOTE: consumer NOT added.

    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);

    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    WorkerProcess prod(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=RegAckReceived", seconds(15)))
        << "producer setup failed before consumer scenario could be exercised:\n"
        << prod.get_stderr();

    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});

    // The denial path: CTRL CURVE handshake fails at the ZAP gate
    // (consumer not in known_roles), or CONSUMER_REG_REQ is rejected
    // at Layer-2.  Either way:
    //   - consumer MUST NOT log `ConsumerRegAckReceived` with status=success
    //   - consumer MUST NOT receive any slot (no `cons_test: read slot`)
    // Generous window then assert absence.
    std::this_thread::sleep_for(seconds(5));

    const std::string cons_combined =
        read_role_log(cons_dir) + cons.get_stderr();

    EXPECT_EQ(cons_combined.find(
                  "event=ConsumerRegAckReceived "), std::string::npos)
        << "DENIAL REGRESSION: unauthorized consumer received "
           "CONSUMER_REG_ACK.  consumer combined log:\n" << cons_combined;
    EXPECT_EQ(cons_combined.find("cons_test: read slot"), std::string::npos)
        << "DENIAL REGRESSION: unauthorized consumer received data slot.\n"
        << cons_combined;

    cons.send_signal(SIGTERM);
    cons.wait_for_exit(10);

    prod.send_signal(SIGTERM);
    EXPECT_EQ(prod.wait_for_exit(10), 0) << prod.get_stderr();

    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0) << hub.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}

// ─── Consumer registration REJECTED (schema mismatch) → FATAL H3b teardown ──
//
// Pins #70 (2026-07-21): the consumer is AUTHORIZED (in known_roles, so the ZAP
// gate admits its CURVE handshake) but its slot schema (float64) does not match
// the channel the producer established (float32).  CONSUMER_REG_REQ is therefore
// rejected, `register_consumer` fails, and the consumer's `worker_main_` takes a
// FATAL post-setup early-return — which MUST call `teardown_infrastructure_()`
// (the "H3b" unwind) inline on the worker thread, matching producer/processor.
//
// Validation is DETERMINISTIC via the production `event=TeardownInfrastructure`
// marker: the pre-fix consumer omitted the call on this path, so the marker
// would be ABSENT.  (The marker fires on teardown entry, so it proves the call
// was made — the essence of the fix — independent of any later timing.)  We
// don't depend on the specific reject reason: any register failure routes
// through the same H3b path.
TEST_F(PlhHubCliTest, ZmqE2E_ConsumerSchemaMismatch_AbortsAndTearsDown)
{
    using std::chrono::seconds;

    const std::string channel  = "lab.l4.zmq.e2e.schemamm";
    const std::string prod_uid = "prod.l4zmq.uid11112222";
    const std::string cons_uid = "cons.l4zmq.uid11112222";
    const int prod_port = 21000 + (::getpid() % 1000);

    const fs::path hub_dir = tmp("zmqe2e_schemamm_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ZmqSchemaMMHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "zmqe2e-schemamm-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }

    const fs::path prod_dir = tmp("zmqe2e_schemamm_prod");
    const fs::path cons_dir = tmp("zmqe2e_schemamm_cons");
    std::error_code ec;
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(cons_dir / "vault", ec);

    write_zmq_producer_config(prod_dir / "producer.json", hub_dir,
                               prod_uid, channel, prod_port);
    write_zmq_producer_script(prod_dir / "script" / "python", 5);
    write_zmq_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, channel);
    write_zmq_consumer_script(cons_dir / "script" / "python", 5);

    // MUTATION POINT: producer emits a single scalar `value:float32`.  Give the
    // consumer a deliberately-mismatched, richer slot schema that differs on
    // ALL THREE fingerprint axes at once — member set (3 fields vs 1), type
    // (float64 / uint16 vs float32), and array size (`samples` is a count-8
    // array vs a scalar).  The fingerprint folds field name + type + count +
    // length + packing, so this differs from the channel's on every axis → the
    // broker rejects CONSUMER_REG_REQ.
    {
        nlohmann::json j;
        { std::ifstream f(cons_dir / "consumer.json"); f >> j; }
        j["in_slot_schema"]["packing"] = "aligned";
        j["in_slot_schema"]["fields"]  = nlohmann::json::array({
            nlohmann::json{{"name", "ts"},        {"type", "float64"}},
            nlohmann::json{{"name", "samples"},   {"type", "float32"}, {"count", 8}},
            nlohmann::json{{"name", "sensor_id"}, {"type", "uint16"}}
        });
        std::ofstream f(cons_dir / "consumer.json");
        f << j.dump(2);
    }

    ::setenv("PYLABHUB_ROLE_PASSWORD", "zmqe2e-schemamm-role-pw", /*overwrite=*/1);
    const std::string prod_pubkey =
        keygen_role_and_read_pubkey(prod_dir, "producer", prod_uid,
                                     "zmqe2e-schemamm-role-pw");
    const std::string cons_pubkey =
        keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                     "zmqe2e-schemamm-role-pw");
    // BOTH authorized — the consumer must clear the ZAP gate and REACH
    // register_consumer, so the abort is a schema reject, not a handshake deny.
    add_known_role(hub_dir, "zmqe2e_schemamm_prod", prod_uid, "producer",
                   prod_pubkey);
    add_known_role(hub_dir, "zmqe2e_schemamm_cons", cons_uid, "consumer",
                   cons_pubkey);

    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);
    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // Producer establishes the channel (float32) and goes live first.
    WorkerProcess prod(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=RegAckReceived", seconds(15)))
        << "producer setup failed:\n" << prod.get_stderr();

    // Consumer registers → schema mismatch → register_consumer fails →
    // FATAL H3b teardown on the worker thread.
    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});

    // PRIMARY (deterministic) pin of the #70 fix: the FATAL register-fail path
    // ran teardown_infrastructure_() on the worker thread.
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=TeardownInfrastructure", seconds(15)))
        << "consumer did NOT tear down infrastructure on the register-fail "
           "path (H3b) — worker-thread sockets would be deferred to destructor "
           "cleanup on another thread.  consumer log:\n"
        << read_role_log(cons_dir) << cons.get_stderr();

    // And it must abort on its own — a GRACEFUL exit (code 0), not a crash or a
    // hang.  A register-failure abort is a clean shutdown, so exit 0 is correct;
    // this still catches the failure modes the missing-teardown bug could cause:
    // a foreign-thread ZMQ close SIGABRT/SIGSEGV → 134/139, or a hang that
    // wait_for_exit escalates to SIGKILL → 137.
    const int rc = cons.wait_for_exit(15);
    EXPECT_EQ(rc, 0)
        << "consumer must abort startup cleanly (exit 0) on schema reject — a "
           "non-zero code indicates a crash or a hang killed by the harness.  "
           "log:\n" << read_role_log(cons_dir) << cons.get_stderr();

    prod.send_signal(SIGTERM);
    EXPECT_EQ(prod.wait_for_exit(10), 0) << prod.get_stderr();
    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0) << hub.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}

// ─── Scenario C: fan-in with two authorized producers ─────────────────────
//
// #246 Phase 3a L4 close-out — multi-producer coverage at the
// coordination-protocol layer (HEP-CORE-0042 §7.1).  Extends
// AUTH-7's single-producer Scenario A by pinning the fan-in
// behaviour of the consumer's §7.1 pre-attach loop when the broker
// hands back `CONSUMER_REG_ACK.producers[]` with more than one entry.
//
// What THIS test pins (all shipped-and-in-scope for #246):
//   (1) Broker's `CONSUMER_REG_ACK.producers[]` (HEP-CORE-0036
//       §5b.7) actually returns a length-N array under fan-in, not
//       just length-1.
//   (2) The consumer's §7.1 loop emits `attach:success` for EACH
//       producer, and the `attach:complete` line reports
//       `admitted=2/2` — the load-bearing marker that the loop
//       iterated to completion and neither producer was silently
//       dropped.
//   (3) Both producer-side HEP-CORE-0042 §7.2 markers fire (each
//       producer captures its instance_id and each emits an
//       APPLIED_REQ against its own confirmed_version state).
//   (4) At least ONE producer's data actually flows through — pinned
//       by `cons_test: complete N=<n>` under the shared consumer
//       script.  Combined with (2) this proves the §7.1 loop admits
//       both producers AND drives Standby → Active on the queue.
//
// What THIS test does NOT pin (out of scope for #246; tracked
// under HEP-CORE-0017 §3.3):
//   - Data from ALL admitted producers actually arriving at the
//     consumer.  `ZmqQueue::apply_master_approval` (see
//     `hub_zmq_queue.cpp:991` block comment) explicitly documents:
//     "Stage 1A scope: single-peer; multi-producer fan-in is
//     HEP-CORE-0017 §3.3 future work."  The PULL socket connects
//     only to peer[0]'s endpoint even though `producer_peers_`
//     holds all N.  A future test asserting distinguisher-value
//     coverage across all N producers lands when HEP-0017 §3.3
//     multi-endpoint PULL ships; the surrounding scaffold in this
//     file (`write_zmq_producer_script_with_offset` +
//     `write_zmq_multi_producer_consumer_script`) is preserved for
//     that follow-up.
//
// L3 coverage note: individual denial reasons
// (consumer_not_in_channel_allowlist / producer_not_live /
// timeout / channel_closing) are covered deterministically under
// `test_pattern4_attach_coordination.cpp` via `BrokerWireClient`
// (HEP-CORE-0042 §9 L3 test plan).  This L4 test focuses on the
// end-to-end fan-in loop where all producers succeed — the shape
// exercised by production deployments.
//
// The MixedAdmitDeny partial-success L4 scenario is deferred:
// triggering `producer_not_live` between CONSUMER_REG_REQ and
// ATTACH_REQ dispatch is racy against heartbeat cadence at the
// subprocess boundary, and the L3 coverage above pins the wire-
// level behaviour deterministically.  Tracked in TESTING_TODO.

TEST_F(PlhHubCliTest, ZmqE2E_MultiProducer_TwoAuthorized)
{
    // Fan-in end-to-end (HEP-CORE-0017 §3.3.0):
    //   - Consumer opens the channel via CONSUMER_REG_REQ, binds PULL,
    //     publishes its bound endpoint via ENDPOINT_UPDATE_REQ.
    //   - Producer's REG_ACK.initial_allowlist carries the consumer's
    //     single {endpoint, pubkey_z85}; producer's PUSH dials it.
    //   - Broker fires CHANNEL_AUTH_CHANGED_NOTIFY(phase=admitted,
    //     role_type=producer) to the consumer's captured zmq_identity;
    //     consumer pulls GET_CHANNEL_AUTH_REQ and admits the producer's
    //     pubkey into its ZAP allowlist so PUSH→PULL CURVE succeeds.
    using std::chrono::seconds;

    const std::string channel   = "lab.l4.zmq.e2e.fanin";
    const std::string prod_a_uid = "prod.l4zmq.uid11111111";
    const std::string prod_b_uid = "prod.l4zmq.uid22222222";
    const std::string cons_uid   = "cons.l4zmq.uid11223344";
    constexpr int kSlotsPerProducer = 5;

    // Distinct port range from Scenarios A + B; PID-derived offset
    // for concurrent test runs.
    const int prod_a_port = 21000 + (::getpid() % 1000);
    const int prod_b_port = 22000 + (::getpid() % 1000);

    // Distinct value-offset windows so consumer can distinguish
    // producers: A writes 0..N-1, B writes 100..100+N-1.
    constexpr int kOffsetA = 0;
    constexpr int kOffsetB = 100;

    // ── Hub init + keygen + ZAP install ───────────────────────────────
    const fs::path hub_dir = tmp("zmqe2e_fanin_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ZmqFanInHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "zmqe2e-fanin-hub-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }

    // ── Two producers + consumer keygen + known_roles ─────────────────
    const fs::path prod_a_dir = tmp("zmqe2e_fanin_prod_a");
    const fs::path prod_b_dir = tmp("zmqe2e_fanin_prod_b");
    const fs::path cons_dir   = tmp("zmqe2e_fanin_cons");
    std::error_code ec;
    fs::create_directories(prod_a_dir / "vault", ec);
    fs::create_directories(prod_b_dir / "vault", ec);
    fs::create_directories(cons_dir   / "vault", ec);

    // Both producers share the SAME channel — that's the fan-in shape.
    // Distinct value offsets in their scripts is what lets the consumer
    // distinguish them.
    // 2026-07-08 topology migration — this scenario is the load-bearing
    // proof that fan-in ZMQ admits N producers into a single consumer's
    // PULL socket.  Both producers AND the consumer declare
    // channel_topology="fan-in" so the broker's admission path
    // recognizes the shape and admits producer B (would otherwise fire
    // ONE_TO_ONE_CARDINALITY_VIOLATED under the default).
    write_zmq_producer_config(prod_a_dir / "producer.json", hub_dir,
                               prod_a_uid, channel, prod_a_port,
                               /*channel_topology=*/"fan-in");
    write_zmq_producer_script_with_offset(prod_a_dir / "script" / "python",
                                           kSlotsPerProducer, kOffsetA);
    write_zmq_producer_config(prod_b_dir / "producer.json", hub_dir,
                               prod_b_uid, channel, prod_b_port,
                               /*channel_topology=*/"fan-in");
    write_zmq_producer_script_with_offset(prod_b_dir / "script" / "python",
                                           kSlotsPerProducer, kOffsetB);
    write_zmq_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, channel,
                               /*channel_topology=*/"fan-in");
    // Multi-producer consumer script: requires slots from BOTH offset
    // windows (0 = producer A, 100 = producer B) before emitting
    // `cons_test: complete`.  This is the load-bearing proof that
    // HEP-CORE-0017 §3.3 multi-endpoint PULL actually works — the
    // fan-in loop admitted N producers AND libzmq's PULL socket
    // fair-queues data from all N connected PUSH peers.
    write_zmq_multi_producer_consumer_script(
        cons_dir / "script" / "python",
        /*expected_slots=*/ 2 * kSlotsPerProducer,
        /*required_offsets=*/ "0,100");

    ::setenv("PYLABHUB_ROLE_PASSWORD", "zmqe2e-fanin-role-pw", /*overwrite=*/1);

    const std::string prod_a_pubkey =
        keygen_role_and_read_pubkey(prod_a_dir, "producer", prod_a_uid,
                                     "zmqe2e-fanin-role-pw");
    const std::string prod_b_pubkey =
        keygen_role_and_read_pubkey(prod_b_dir, "producer", prod_b_uid,
                                     "zmqe2e-fanin-role-pw");
    const std::string cons_pubkey =
        keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                     "zmqe2e-fanin-role-pw");

    add_known_role(hub_dir, "zmqe2e_fanin_prod_a", prod_a_uid, "producer",
                   prod_a_pubkey);
    add_known_role(hub_dir, "zmqe2e_fanin_prod_b", prod_b_uid, "producer",
                   prod_b_pubkey);
    add_known_role(hub_dir, "zmqe2e_fanin_cons", cons_uid, "consumer",
                   cons_pubkey);

    // ── Spawn hub run-mode ────────────────────────────────────────────
    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);

    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // Under the fan-in topology model the consumer is the BINDING
    // side: it must come up first, bind its PULL socket, and publish
    // its endpoint before producers can dial.  Broker fills the
    // producer's REG_ACK initial_allowlist with a single
    // {endpoint, pubkey_z85} entry taken from ChannelEntry.consumers
    // + data_endpoint (HEP-CORE-0017 §3.3.0).

    // ── Spawn consumer first (fan-in binding side) ────────────────────
    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});

    auto dump_cons = [&](const std::string &where) -> std::string {
        std::string s;
        s += "[fail at: " + where + "]\n";
        s += "── consumer log ──\n" + read_role_log(cons_dir) + "\n";
        s += "── consumer stderr ──\n" + cons.get_stderr() + "\n";
        s += "── hub log ──\n" + read_hub_log(hub_dir) + "\n";
        return s;
    };

    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=ConsumerRegReqAccepted role='" + cons_uid +
        "' channel='" + channel + "'", seconds(10)))
        << dump_cons("ConsumerRegReqAccepted");
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=ConsumerRegAckReceived", seconds(5)))
        << dump_cons("ConsumerRegAckReceived");

    // ── Spawn both producers (fan-in dialing side) ────────────────────
    WorkerProcess prod_a(plh_role_binary(), "--role",
        {"producer", prod_a_dir.string()});
    WorkerProcess prod_b(plh_role_binary(), "--role",
        {"producer", prod_b_dir.string()});

    auto dump_full = [&](const std::string &where) -> std::string {
        std::string s = dump_cons(where);
        s += "── producer A log ──\n" + read_role_log(prod_a_dir) + "\n";
        s += "── producer A stderr ──\n" + prod_a.get_stderr() + "\n";
        s += "── producer B log ──\n" + read_role_log(prod_b_dir) + "\n";
        s += "── producer B stderr ──\n" + prod_b.get_stderr() + "\n";
        return s;
    };
    auto dump_all = dump_full;  // kept as alias for markers below

    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=RegReqAccepted role='" + prod_a_uid + "' channel='" +
        channel + "'", seconds(7)))
        << dump_full("producer A RegReqAccepted");
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=RegReqAccepted role='" + prod_b_uid + "' channel='" +
        channel + "'", seconds(7)))
        << dump_full("producer B RegReqAccepted");
    ASSERT_TRUE(wait_for_role_marker(prod_a_dir, prod_a,
        "event=RegAckReceived", seconds(5)))
        << dump_full("producer A RegAckReceived");
    ASSERT_TRUE(wait_for_role_marker(prod_b_dir, prod_b,
        "event=RegAckReceived", seconds(5)))
        << dump_full("producer B RegAckReceived");

    // ── Post-migration fan-in binding markers ─────────────────────────
    //
    // Under HEP-CORE-0017 §3.3.0 fan-in the consumer opens the
    // channel, binds PULL, and publishes its bound endpoint.
    // Producers arrive later, receive the consumer's endpoint on
    // REG_ACK's initial_allowlist, dial PUSH, and CURVE-handshake
    // against the consumer's ZAP.  There is no "attach loop" — the
    // producer's queue drives Standby → Configured → Active in one
    // step via `apply_master_approval(REG_ACK)`.
    //
    // Consumer-side binding: the endpoint publish marker fires only
    // when the topology-model binding path ran end-to-end (queue
    // reached Active, actual_endpoint() returned non-empty,
    // send_endpoint_update succeeded).  A regression that leaves
    // data_endpoint unset would fail here — producers would then
    // receive an empty initial_allowlist and their queues would
    // stay in Standby.
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=BindingEndpointPublished channel='" + channel + "'",
        seconds(5)))
        << dump_full("event=BindingEndpointPublished — fan-in consumer "
                     "published its bound endpoint to the broker");

    // Producer-side: both must process REG_ACK's fan-in
    // initial_allowlist entry and complete the APPLIED_REQ round-trip
    // for their per-producer allowlist state on the consumer's ZAP.
    ASSERT_TRUE(wait_for_role_marker(prod_a_dir, prod_a,
        "event=ProducerInstanceIdCaptured channel='" + channel + "'",
        seconds(5)))
        << dump_full("producer A ProducerInstanceIdCaptured");
    ASSERT_TRUE(wait_for_role_marker(prod_b_dir, prod_b,
        "event=ProducerInstanceIdCaptured channel='" + channel + "'",
        seconds(5)))
        << dump_full("producer B ProducerInstanceIdCaptured");
    ASSERT_TRUE(wait_for_role_marker(prod_a_dir, prod_a,
        "event=ChannelAuthApplied channel='" + channel + "'",
        seconds(5)))
        << dump_full("producer A ChannelAuthApplied");
    ASSERT_TRUE(wait_for_role_marker(prod_b_dir, prod_b,
        "event=ChannelAuthApplied channel='" + channel + "'",
        seconds(5)))
        << dump_full("producer B ChannelAuthApplied");

    // ── Data flow verification (BOTH producers flow) ──────────────────
    // The multi-producer consumer script only emits `cons_test: complete`
    // when BOTH the total-slot-count AND both-offsets-seen conditions
    // hold.  Reaching this marker proves the PULL socket actually
    // received slots from BOTH producer A (values 0..N-1, offset=0)
    // AND producer B (values 100..100+N-1, offset=100).  A regression
    // that admits N producers but only wires one into the queue's
    // connect loop (the pre-#246 §3.3 Stage 1A behaviour) fails here
    // even if the total slot count reaches 2N (which it can't, since
    // only one producer would be dialed).
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "cons_test: complete N=", seconds(15)))
        << dump_full("cons_test: complete (both offsets seen)");
    // Extra pin: the completion line MUST mention both offsets.
    {
        const std::string cons_log = read_role_log(cons_dir);
        EXPECT_NE(cons_log.find("offsets_seen=0,100"), std::string::npos)
            << "cons_test: complete did not include both offsets — "
               "one producer's data never reached the consumer.  Full log:\n"
            << cons_log;
    }

    // ── Shutdown ──────────────────────────────────────────────────────
    cons.send_signal(SIGTERM);
    EXPECT_EQ(cons.wait_for_exit(10), 0) << cons.get_stderr();
    prod_a.send_signal(SIGTERM);
    EXPECT_EQ(prod_a.wait_for_exit(10), 0) << prod_a.get_stderr();
    prod_b.send_signal(SIGTERM);
    EXPECT_EQ(prod_b.wait_for_exit(10), 0) << prod_b.get_stderr();
    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0) << hub.get_stderr();

    // Class-D gate: no [ERROR ] in any log.
    auto contains_error = [](const std::string &s) {
        return s.find("[ERROR ]") != std::string::npos;
    };
    EXPECT_FALSE(contains_error(read_hub_log(hub_dir)))
        << "hub log [ERROR ]:\n" << read_hub_log(hub_dir);
    EXPECT_FALSE(contains_error(prod_a.get_stderr()))
        << "producer A stderr [ERROR ]:\n" << prod_a.get_stderr();
    EXPECT_FALSE(contains_error(prod_b.get_stderr()))
        << "producer B stderr [ERROR ]:\n" << prod_b.get_stderr();
    EXPECT_FALSE(contains_error(cons.get_stderr()))
        << "consumer stderr [ERROR ]:\n" << cons.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}

// ─── Scenario D: three-role pipeline through processor ──────────────────────
//
// Producer → Processor → Consumer over ZMQ, three roles in three
// subprocesses.  Topology: BOTH channels declared fan-in.  Load-
// bearing regression guard for the queue-owned topology / layer
// cleanup arc (HEP-CORE-0036 §I9.1) applied to the processor role.
//
//   ch1 (upstream_prod → processor.in):
//     fan-in.  Processor.in BINDS PULL (fan-in binding consumer);
//     upstream_prod DIALS PUSH (fan-in dialing producer).
//   ch2 (processor.out → downstream_cons):
//     fan-in.  Downstream_cons BINDS PULL (fan-in binding consumer);
//     processor.out DIALS PUSH (fan-in dialing producer, defers
//     connect via `apply_master_approval` → `dial_pending=true`).
//
// **What this pins**: the two-channel resolution fix landed
// 2026-07-12 (see `docs/tech_draft/DRAFT_queue_owned_topology_and_layer_cleanup_2026-07-11.md`
// § "Post-review correctness fix").  Pre-fix, the primary-channel
// branch fell back rx→tx: `finalize_channel_connect(in_channel)`
// wrongly returned the OUT queue (dial_pending=true), the queue
// polled the broker with `channel=in_channel` + OUT queue's
// pubkey, and the broker's `handle_check_peer_ready_req` caller
// check rejected with `NOT_A_ROLE_OF_CHANNEL` (processor is
// CONSUMER of IN under fan-in, not the dialing-side producer
// §6.6.3 expects to poll).  Result: `PollResult::PermanentError`
// → processor teardown → this test fails at proc RegAckReceived
// or cons_test complete.  Post-fix: IN call is a legitimate no-op;
// OUT call polls its own channel; test passes.

TEST_F(PlhHubCliTest, ZmqE2E_Processor_FanInBothChannels_ThreeRoles)
{
    using std::chrono::seconds;

    const std::string ch1        = "lab.l4.zmq.e2e.proc.ch1";
    const std::string ch2        = "lab.l4.zmq.e2e.proc.ch2";
    const std::string prod_uid   = "prod.l4zmqproc.uid11111111";
    const std::string proc_uid   = "proc.l4zmqproc.uid22222222";
    const std::string cons_uid   = "cons.l4zmqproc.uid33333333";
    constexpr int     kSlots     = 5;
    constexpr int     kProdOffset = 0;
    constexpr float   kProcMark   = 1000.0f;

    const int prod_port     = 23000 + (::getpid() % 1000);
    const int proc_in_port  = 24000 + (::getpid() % 1000);
    const int proc_out_port = 25000 + (::getpid() % 1000);
    (void)prod_port;      // dialing side, endpoint from REG_ACK.
    (void)proc_out_port;  // dialing side, endpoint from REG_ACK.

    // ── Hub init + keygen ──────────────────────────────────────────────
    const fs::path hub_dir = tmp("zmqe2e_proc_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ZmqProcHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "zmqe2e-proc-hub-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }

    // ── Three role dirs + configs + scripts ────────────────────────────
    const fs::path prod_dir = tmp("zmqe2e_proc_prod");
    const fs::path proc_dir = tmp("zmqe2e_proc_proc");
    const fs::path cons_dir = tmp("zmqe2e_proc_cons");
    std::error_code ec;
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(proc_dir / "vault", ec);
    fs::create_directories(cons_dir / "vault", ec);

    write_zmq_producer_config(prod_dir / "producer.json", hub_dir,
                               prod_uid, ch1, prod_port,
                               /*channel_topology=*/"fan-in");
    write_zmq_producer_script_with_offset(prod_dir / "script" / "python",
                                           kSlots, kProdOffset);

    write_zmq_processor_config(proc_dir / "processor.json", hub_dir,
                                proc_uid,
                                /*in_channel=*/  ch1,
                                /*out_channel=*/ ch2,
                                /*in_topology=*/ "fan-in",
                                /*out_topology=*/"fan-in",
                                proc_in_port, proc_out_port);
    write_zmq_processor_script(proc_dir / "script" / "python", kProcMark);

    write_zmq_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, ch2,
                               /*channel_topology=*/"fan-in");
    write_zmq_consumer_script(cons_dir / "script" / "python", kSlots);

    ::setenv("PYLABHUB_ROLE_PASSWORD", "zmqe2e-proc-role-pw", /*overwrite=*/1);

    const std::string prod_pubkey =
        keygen_role_and_read_pubkey(prod_dir, "producer", prod_uid,
                                     "zmqe2e-proc-role-pw");
    const std::string proc_pubkey =
        keygen_role_and_read_pubkey(proc_dir, "processor", proc_uid,
                                     "zmqe2e-proc-role-pw");
    const std::string cons_pubkey =
        keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                     "zmqe2e-proc-role-pw");

    add_known_role(hub_dir, "zmqe2e_proc_prod", prod_uid, "producer",
                   prod_pubkey);
    add_known_role(hub_dir, "zmqe2e_proc_proc", proc_uid, "processor",
                   proc_pubkey);
    add_known_role(hub_dir, "zmqe2e_proc_cons", cons_uid, "consumer",
                   cons_pubkey);

    // ── Spawn hub run-mode ─────────────────────────────────────────────
    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);

    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // Startup ordering: both binding sides come up first so their
    // endpoints are published before the dialing side needs them.

    // ── Downstream consumer (binding side of ch2) ─────────────────────
    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});
    auto dump_partial = [&](const std::string &where) -> std::string {
        std::string s;
        s += "[fail at: " + where + "]\n";
        s += "── consumer log ──\n" + read_role_log(cons_dir) + "\n";
        s += "── consumer stderr ──\n" + cons.get_stderr() + "\n";
        s += "── hub log ──\n" + read_hub_log(hub_dir) + "\n";
        return s;
    };
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=ConsumerRegReqAccepted role='" + cons_uid +
        "' channel='" + ch2 + "'", seconds(10)))
        << dump_partial("cons ConsumerRegReqAccepted");
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=ConsumerRegAckReceived", seconds(5)))
        << dump_partial("cons ConsumerRegAckReceived");

    // ── Processor (binds ch1 IN, dials ch2 OUT) ───────────────────────
    WorkerProcess proc(plh_role_binary(), "--role",
        {"processor", proc_dir.string()});
    auto dump_partial2 = [&](const std::string &where) -> std::string {
        std::string s = dump_partial(where);
        s += "── processor log ──\n" + read_role_log(proc_dir) + "\n";
        s += "── processor stderr ──\n" + proc.get_stderr() + "\n";
        return s;
    };

    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=RegReqAccepted role='" + proc_uid + "' channel='" +
        ch2 + "'", seconds(7)))
        << dump_partial2("proc RegReqAccepted (ch2/producer-side)");
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=ConsumerRegReqAccepted role='" + proc_uid +
        "' channel='" + ch1 + "'", seconds(7)))
        << dump_partial2("proc ConsumerRegReqAccepted (ch1/consumer-side)");

    // Processor RegAckReceived — the marker the pre-fix
    // two-channel resolution bug would have blocked.
    ASSERT_TRUE(wait_for_role_marker(proc_dir, proc,
        "event=RegAckReceived", seconds(5)))
        << dump_partial2("proc RegAckReceived");
    ASSERT_TRUE(wait_for_role_marker(proc_dir, proc,
        "event=ConsumerRegAckReceived", seconds(5)))
        << dump_partial2("proc ConsumerRegAckReceived");

    // ── Upstream producer (dialing side of ch1) ───────────────────────
    WorkerProcess prod(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});
    auto dump_full = [&](const std::string &where) -> std::string {
        std::string s = dump_partial2(where);
        s += "── producer log ──\n" + read_role_log(prod_dir) + "\n";
        s += "── producer stderr ──\n" + prod.get_stderr() + "\n";
        return s;
    };
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=RegReqAccepted role='" + prod_uid + "' channel='" +
        ch1 + "'", seconds(7)))
        << dump_full("prod RegReqAccepted");
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=RegAckReceived", seconds(5)))
        << dump_full("prod RegAckReceived");

    // ── Data flow verification ────────────────────────────────────────
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "cons_test: complete N=", seconds(20)))
        << dump_full("cons_test complete");

    // ── Shutdown (reverse-startup order) ──────────────────────────────
    prod.send_signal(SIGTERM);
    EXPECT_EQ(prod.wait_for_exit(10), 0) << prod.get_stderr();
    proc.send_signal(SIGTERM);
    EXPECT_EQ(proc.wait_for_exit(10), 0) << proc.get_stderr();
    cons.send_signal(SIGTERM);
    EXPECT_EQ(cons.wait_for_exit(10), 0) << cons.get_stderr();
    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0) << hub.get_stderr();

    auto contains_error = [](const std::string &s) {
        return s.find("[ERROR ]") != std::string::npos;
    };
    EXPECT_FALSE(contains_error(read_hub_log(hub_dir)))
        << "hub log [ERROR ]:\n" << read_hub_log(hub_dir);
    EXPECT_FALSE(contains_error(prod.get_stderr()))
        << "producer stderr [ERROR ]:\n" << prod.get_stderr();
    EXPECT_FALSE(contains_error(proc.get_stderr()))
        << "processor stderr [ERROR ]:\n" << proc.get_stderr();
    EXPECT_FALSE(contains_error(cons.get_stderr()))
        << "consumer stderr [ERROR ]:\n" << cons.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}

// ─── Inbox delivery (role->role inbox over the current plaintext InboxQueue) ──
//
// First L4 inbox test (none existed).  End-to-end pin: one role SENDS to
// another role's inbox and the receiver's on_inbox handler fires with the
// correct sender + payload.  This is the harness the two-tier-key CURVE pilot
// (HEP-CORE-0027 §3.5, task #191) will harden + add a deny path to.

// Receiver = producer on `channel` WITH an inbox (OS-assigned endpoint +
// int32 schema); its on_inbox logs the received message.
void write_inbox_receiver_config(const fs::path    &cfg_path,
                                  const fs::path    &hub_dir,
                                  const std::string &uid,
                                  const std::string &channel,
                                  int                prod_port)
{
    nlohmann::json j;
    j["producer"]["uid"]             = uid;
    j["producer"]["name"]            = "L4InboxReceiver";
    j["producer"]["log_level"]       = "info";
    j["producer"]["auth"]["keyfile"] = "vault/" + uid + ".vault";
    j["out_hub_dir"]      = hub_dir.string();
    j["out_channel"]      = channel;
    j["loop_timing"]      = "fixed_rate";
    j["target_period_ms"] = 50;
    j["out_transport"]           = "zmq";
    j["out_zmq_endpoint"]        = "tcp://127.0.0.1:" + std::to_string(prod_port);
    j["out_zmq_buffer_depth"]    = 256;
    j["out_zmq_overflow_policy"] = "drop";
    j["out_slot_schema"]["packing"] = "aligned";
    j["out_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}});
    j["out_flexzone_schema"]  = nullptr;
    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = true;
    j["stop_on_script_error"] = false;
    j["script"]["type"]       = "python";
    j["script"]["path"]       = ".";
    // Inbox: loopback OS-assigned endpoint + typed schema (one int32 field).
    j["inbox_endpoint"]          = "tcp://127.0.0.1:0";
    j["inbox_schema"]["packing"] = "aligned";
    j["inbox_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "int32"}}});

    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    std::ofstream f(cfg_path);
    f << j.dump(2);
}

void write_inbox_receiver_script(const fs::path &script_dir)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_iter = [0]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'inbox_recv: init')\n"
         "\n"
         "def on_produce(tx, msgs, api):\n"
         "    if tx.slot is None:\n"
         "        return False\n"
         "    tx.slot.value = float(_iter[0])\n"
         "    _iter[0] += 1\n"
         "    return True\n"
         "\n"
         "def on_inbox(msg, api):\n"
         "    api.log('info', 'inbox_recv: GOT sender=' + str(msg.sender_uid) +\n"
         "            ' value=' + str(msg.data.value))\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'inbox_recv: stop')\n";
}

// Sender = producer on its own channel (no inbox); each tick it retries
// api.open_inbox(receiver_uid) until discoverable, then sends one message.
void write_inbox_sender_script(const fs::path    &script_dir,
                               const std::string &receiver_uid)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_iter = [0]\n"
         "_sent = [False]\n"
         "_RECV = '" << receiver_uid << "'\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'inbox_send: init')\n"
         "\n"
         "def on_produce(tx, msgs, api):\n"
         "    if not _sent[0]:\n"
         "        h = api.open_inbox(_RECV)\n"
         "        if h is not None:\n"
         "            slot = h.acquire()\n"
         "            if slot is not None:\n"
         "                slot.value = 42\n"
         "                rc = h.send()\n"
         "                api.log('info', 'inbox_send: SENT rc=' + str(rc))\n"
         "                _sent[0] = True\n"
         "    if tx.slot is None:\n"
         "        return False\n"
         "    tx.slot.value = float(_iter[0])\n"
         "    _iter[0] += 1\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'inbox_send: stop')\n";
}

TEST_F(PlhHubCliTest, ZmqE2E_InboxDelivery)
{
    using std::chrono::seconds;

    const std::string recv_channel = "lab.l4.inbox.recv";
    const std::string send_channel = "lab.l4.inbox.send";
    const std::string recv_uid     = "prod.l4inbox.recv12345678";
    const std::string send_uid     = "prod.l4inbox.send12345678";
    const int         recv_port    = 21000 + (::getpid() % 1000);
    const int         send_port    = 22000 + (::getpid() % 1000);

    const fs::path hub_dir = tmp("inbox_e2e_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4InboxHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "inbox-e2e-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }

    const fs::path recv_dir = tmp("inbox_e2e_recv");
    const fs::path send_dir = tmp("inbox_e2e_send");
    std::error_code ec;
    fs::create_directories(recv_dir / "vault", ec);
    fs::create_directories(send_dir / "vault", ec);

    write_inbox_receiver_config(recv_dir / "producer.json", hub_dir,
                                 recv_uid, recv_channel, recv_port);
    write_inbox_receiver_script(recv_dir / "script" / "python");
    write_zmq_producer_config(send_dir / "producer.json", hub_dir,
                               send_uid, send_channel, send_port);
    write_inbox_sender_script(send_dir / "script" / "python", recv_uid);

    ::setenv("PYLABHUB_ROLE_PASSWORD", "inbox-e2e-role-pw", /*overwrite=*/1);
    const std::string recv_pubkey =
        keygen_role_and_read_pubkey(recv_dir, "producer", recv_uid,
                                     "inbox-e2e-role-pw");
    const std::string send_pubkey =
        keygen_role_and_read_pubkey(send_dir, "producer", send_uid,
                                     "inbox-e2e-role-pw");
    add_known_role(hub_dir, "inbox_recv", recv_uid, "producer", recv_pubkey);
    add_known_role(hub_dir, "inbox_send", send_uid, "producer", send_pubkey);

    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);
    const std::string bound_ep = extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    WorkerProcess recv(plh_role_binary(), "--role",
        {"producer", recv_dir.string()});
    ASSERT_TRUE(wait_for_role_marker(recv_dir, recv, "event=RegAckReceived",
                                     seconds(15)))
        << "receiver never registered:\n" << recv.get_stderr();

    WorkerProcess send(plh_role_binary(), "--role",
        {"producer", send_dir.string()});
    ASSERT_TRUE(wait_for_role_marker(send_dir, send, "event=RegAckReceived",
                                     seconds(15)))
        << "sender never registered:\n" << send.get_stderr();

    // Slice 1 (HEP-CORE-0027 §3.5): each role captures the hub-wide inbox
    // roster from REG_ACK.known_roles.  Both roles are in the hub's
    // known_roles, so the merge fires with a non-zero total — this is the
    // seed the inbox ROUTER ZAP arm (slice 2) consumes.
    ASSERT_TRUE(wait_for_role_marker(recv_dir, recv,
        "event=InboxKnownRolesMerged", seconds(10)))
        << "receiver never captured the known_roles roster:\n"
        << read_role_log(recv_dir);
    ASSERT_TRUE(wait_for_role_marker(send_dir, send,
        "event=InboxKnownRolesMerged", seconds(10)))
        << "sender never captured the known_roles roster:\n"
        << read_role_log(send_dir);

    // Slice 2 (HEP-CORE-0027 §3.5): the receiver seeds its inbox ROUTER's
    // CURVE/ZAP allowlist from the roster, lifting it off the S1 deny-all
    // default.  This is the gate that admits the authorized sender below.
    ASSERT_TRUE(wait_for_role_marker(recv_dir, recv,
        "event=InboxAllowlistSeeded", seconds(10)))
        << "receiver never seeded its inbox ROUTER ZAP allowlist:\n"
        << read_role_log(recv_dir);

    // Delivery: sender discovers the receiver's inbox and sends value=42; the
    // receiver's on_inbox fires with the correct sender uid.
    ASSERT_TRUE(wait_for_role_marker(send_dir, send, "inbox_send: SENT rc=0",
                                     seconds(15)))
        << "sender never sent to the inbox:\n"
        << read_role_log(send_dir) << send.get_stderr();
    ASSERT_TRUE(wait_for_role_marker(recv_dir, recv,
        "inbox_recv: GOT sender=" + send_uid + " value=42", seconds(15)))
        << "receiver's on_inbox never fired for the sent message:\n"
        << read_role_log(recv_dir) << recv.get_stderr();

    send.send_signal(SIGTERM);
    send.wait_for_exit(10);
    recv.send_signal(SIGTERM);
    recv.wait_for_exit(10);
    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0) << hub.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}
