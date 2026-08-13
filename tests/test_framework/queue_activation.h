#pragma once
/**
 * @file queue_activation.h
 * @brief Bring a queue up in a test the way production does
 *        (HEP-CORE-0036 §6.7 / §6.6.3).
 *
 * **Use these instead of `q->start()`.**
 *
 * A queue leaves its constructor in `Standby`, and `start()` refuses a
 * queue that has not reached `Configured` (§6.7.1).  That is not an
 * obstacle to route around — it is the admission contract.  Every data
 * socket in this system is CURVE-authenticated, and who a queue may
 * talk to is the broker's decision, delivered as REG_ACK /
 * CONSUMER_REG_ACK and installed by `apply_master_approval`.
 *
 * An L2 test has no broker and does not need one.  What the queue wants
 * is the *shape* of the master's answer, which is a JSON object; an
 * empty one is an ACK naming no peers.  That is a stub of the ANSWER,
 * not a stub of the broker, so it does not breach the no-mocks rule.
 *
 * @code
 * auto q = ZmqQueue::create_reader(ChannelTopology::FanIn, std::move(rx));
 * ASSERT_TRUE(activate(*q));
 * @endcode
 *
 * Pass a populated ACK instead when the test cares about the peers —
 * see the `TopologyFactory_*` tests, which hand it a real
 * `producers` / `initial_allowlist` array.
 *
 * The full narrative contract, including which topology needs the
 * second step and why, is `@ref queue_activation` in
 * `src/include/utils/hub_queue.hpp`.
 */

#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_zmq_queue.hpp"

namespace pylabhub::tests
{

/// Readiness-wait budget for `complete_deferred_dial`.  The oracle
/// answers on the first poll, so this bounds a hang rather than a wait.
inline constexpr std::uint64_t kDeferredDialTimeoutMs = 1000;

/// Drive a freshly built queue up the way production does.
///
/// `apply_master_approval` is the only entry point to
/// Standby → Configured → Active; `start()` is the step it performs
/// internally once the master's answer is in hand.
[[nodiscard]] inline bool activate(::pylabhub::hub::ZmqQueue &q)
{
    return q.apply_master_approval(nlohmann::json::object());
}

/// Same, for tests that deliberately hold the ABSTRACT interface.
/// `apply_master_approval` is declared on both `QueueReader` and
/// `QueueWriter` — it is the polymorphic mutator §6.7 names — so the
/// contract holds through a base pointer too.  `ZmqQueue` derives from
/// both, which is why the concrete overload above exists: without it a
/// `ZmqQueue&` argument would be ambiguous.
[[nodiscard]] inline bool activate(::pylabhub::hub::QueueWriter &q)
{
    return q.apply_master_approval(nlohmann::json::object());
}
[[nodiscard]] inline bool activate(::pylabhub::hub::QueueReader &q)
{
    return q.apply_master_approval(nlohmann::json::object());
}

/// The SECOND half of activation, needed by exactly one topology: the
/// fan-in producer (dialing PUSH).
///
/// `apply_master_approval` deliberately stops that one at `DialDeferred`
/// instead of connecting.  Connecting would begin the CURVE handshake
/// against the consumer's PULL socket immediately, and under fan-in the
/// consumer installs this producer's key in its ZAP allowlist a moment
/// LATER — libzmq treats the resulting ZAP DENY as terminal, with no
/// client-side retry (HEP-CORE-0036 §6.6.3).  So the connect waits for
/// the peer to confirm.
///
/// In production the role host supplies a `PeerReadinessOracle` that
/// asks the broker whether the consumer has confirmed the snapshot.  A
/// test that has already seeded the consumer's allowlist by hand knows
/// the answer is "yes", so the oracle can say so directly.
///
/// Safe to call on ANY queue: every other topology is already `Active`
/// after `activate()`, and `finalize_connect` returns success without
/// touching the oracle for those.  The role host calls it uniformly for
/// the same reason.
[[nodiscard]] inline bool complete_deferred_dial(::pylabhub::hub::ZmqQueue &q,
                                                 std::uint64_t timeout_ms = kDeferredDialTimeoutMs)
{
    struct AlwaysReadyOracle final : public ::pylabhub::hub::PeerReadinessOracle
    {
        PollResult poll() noexcept override { return PollResult::Ready; }
    };
    AlwaysReadyOracle oracle;
    return q.finalize_connect(oracle, timeout_ms, /*is_cancelled=*/{}, "[test]");
}

/// The queue's bound address as a string, for a test that needs to dial
/// it.
///
/// Use this instead of reaching for the configured endpoint.  A test that
/// binds `tcp://127.0.0.1:0` and then connects to the *configured* string
/// connects to port 0 and fails in a way that looks like a transport
/// problem.  `bound_address()` is the only thing that knows the real
/// port, and it answers `std::nullopt` when there is no answer — so this
/// helper fails the test loudly rather than handing back something
/// plausible.
///
/// This replaced a family of `ASSERT_FALSE(q->actual_endpoint().empty())`
/// guards that could not fail: the accessor used to fall back to the
/// configured string, so the assertion held even for a queue that had
/// never bound (HEP-CORE-0036 §6.7.2).
[[nodiscard]] inline std::string bound_endpoint_or_fail(const ::pylabhub::hub::ZmqQueue &q)
{
    auto bound = q.bound_address();
    if (!bound.has_value())
    {
        ADD_FAILURE() << "queue reports no bound address: it is not Active, or it dials "
                         "rather than binds (HEP-CORE-0036 §6.7.2)";
        return {};
    }
    return bound->str();
}

/// Same, for the inbox ROUTER.
[[nodiscard]] inline std::string bound_endpoint_or_fail(const ::pylabhub::hub::InboxQueue &q)
{
    auto bound = q.bound_address();
    if (!bound.has_value())
    {
        ADD_FAILURE() << "inbox reports no bound address — start() has not completed "
                         "(its configured default is port 0, so there is nothing to "
                         "fall back to)";
        return {};
    }
    return bound->str();
}

} // namespace pylabhub::tests
