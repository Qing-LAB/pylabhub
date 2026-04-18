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

} // namespace zmq_context
} // namespace pylabhub::tests::worker
