#pragma once
/**
 * @file zmq_context_workers.h
 * @brief Worker functions for ZmqContext lifecycle tests (Pattern 3).
 *
 * Each worker spawns in its own subprocess (IsolatedProcessTest::SpawnWorker)
 * with a LifecycleGuard owning Logger + ZMQContext. Tests assert on the
 * shared zmq::context_t exposed by hub::get_zmq_context().
 */

namespace pylabhub::tests::worker
{
namespace zmq_context
{

/** get_zmq_context() returns a valid context (no throw). */
int get_context_returns_valid();

/** Two consecutive get_zmq_context() calls return the same instance. */
int get_context_same_instance();

/** zmq::socket_t can be constructed against the shared context. */
int create_socket_works();

/** N concurrent threads calling get_zmq_context() see the same pointer. */
int multithread_get_context_safe();

/// `apply_socket_policy(sock, TcpConnect)` sets every option in the
/// full policy block (linger=0, sndtimeo=500, heartbeat 5s/30s,
/// reconnect_ivl=-1, reconnect_ivl_max=0).  Mutation: removing any
/// `.set(...)` from the helper → readback fails.  See
/// HEP-CORE-0023 §2.5.3 + utils/zmq_socket_policy.hpp.
int apply_socket_policy_tcp_connect_sets_all();

/// `apply_socket_policy(sock, TcpBind)` sets the heartbeat + sndtimeo
/// + linger subset BUT does NOT set reconnect_ivl (bind sockets
/// don't initiate connections — option would be a no-op).  Verifies
/// the role-branching logic in the helper.
int apply_socket_policy_tcp_bind_subset();

/// `apply_socket_policy(sock, InprocSignal)` sets ONLY linger=0
/// (inproc PAIR sockets used for wake-up don't need ZMTP heartbeat
/// or reconnect policy).  Verifies the inproc branch.
int apply_socket_policy_inproc_signal_minimal();

} // namespace zmq_context
} // namespace pylabhub::tests::worker
