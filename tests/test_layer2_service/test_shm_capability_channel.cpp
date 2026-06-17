/**
 * @file test_shm_capability_channel.cpp
 * @brief L2 skeleton pins for the ShmCapabilityChannel abstract
 *        interface (HEP-CORE-0041 §6).
 *
 * Pattern 1 — pure interface tests; no LOGGER_*, no FileLock, no
 * lifecycle module.  The factory functions ship as throw-only stubs
 * until task #249 lands the `memfd_create` backend; this file pins
 * (a) the abstract surface and (b) the documented throw contract.
 *
 * When task #249 supplies a real backend, the throw-message pins move
 * to that backend's test file and this skeleton retires.
 */
#include "utils/security/shm_capability_channel.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

using pylabhub::utils::security::IShmCapabilityConsumer;
using pylabhub::utils::security::IShmCapabilityProducer;
using pylabhub::utils::security::attach_shm_capability_consumer;
using pylabhub::utils::security::create_shm_capability_producer;

// ── Compile-time pins on the abstract surface ────────────────────────────
//
// Removing any virtual method, or changing the interface to a
// concrete (non-abstract) shape, fires these at build time before
// any TEST body runs.  Cheaper than a runtime mutation pin.

static_assert(std::is_abstract_v<IShmCapabilityProducer>,
              "IShmCapabilityProducer must be a pure abstract base — "
              "concrete backends live behind create_shm_capability_producer "
              "per HEP-CORE-0041 §6.");
static_assert(std::is_abstract_v<IShmCapabilityConsumer>,
              "IShmCapabilityConsumer must be a pure abstract base.");

// Both producer and consumer interfaces explicitly delete copy + move
// to keep kernel-resource ownership unambiguous (the fd/HANDLE and
// listener belong to exactly one instance).  These asserts catch any
// future loosening — std::is_abstract is true even with a deleted
// virtual dtor, so the move-construction check is the load-bearing
// half.

static_assert(!std::is_copy_constructible_v<IShmCapabilityProducer>,
              "IShmCapabilityProducer must remain non-copyable.");
static_assert(!std::is_copy_constructible_v<IShmCapabilityConsumer>,
              "IShmCapabilityConsumer must remain non-copyable.");

// ── Runtime pins on the throw-stub contract ──────────────────────────────

TEST(ShmCapabilityChannelSkeletonTest, ProducerFactoryThrowsWithDocumentedMessage)
{
    try
    {
        std::unique_ptr<IShmCapabilityProducer> p =
            create_shm_capability_producer(/*bytes=*/4096);
        FAIL() << "Expected create_shm_capability_producer to throw — no "
                  "backend is registered.";
        (void) p;
    }
    catch (const std::runtime_error &e)
    {
        const std::string what{e.what()};
        EXPECT_NE(what.find("HEP-CORE-0041"), std::string::npos)
            << "Throw message must cite HEP-CORE-0041 so readers find the "
               "design contract: "
            << what;
        EXPECT_NE(what.find("#249"), std::string::npos)
            << "Throw message must name task #249 so readers find the "
               "backend-implementation task: "
            << what;
    }
}

TEST(ShmCapabilityChannelSkeletonTest, ConsumerFactoryThrowsWithDocumentedMessage)
{
    try
    {
        std::unique_ptr<IShmCapabilityConsumer> c =
            attach_shm_capability_consumer(
                "/tmp/plh_skeleton_no_backend.sock",
                std::chrono::milliseconds{100});
        FAIL() << "Expected attach_shm_capability_consumer to throw — no "
                  "backend is registered.";
        (void) c;
    }
    catch (const std::runtime_error &e)
    {
        const std::string what{e.what()};
        EXPECT_NE(what.find("HEP-CORE-0041"), std::string::npos)
            << "Throw message must cite HEP-CORE-0041: " << what;
        EXPECT_NE(what.find("#249"), std::string::npos)
            << "Throw message must name task #249: " << what;
    }
}

// ── Pin on AcceptedPeer's POSIX-credential triple ───────────────────────
//
// L2 (auth orchestration, task #250) reads pid/uid/gid out of this
// struct to run the §9 D4 step 3 sanity check.  Renaming or dropping
// a field here would silently break that consumer; the assignments
// below pin the field names at compile time.

TEST(ShmCapabilityChannelSkeletonTest, AcceptedPeerHasPosixCredentialFields)
{
    IShmCapabilityProducer::AcceptedPeer p{};
    p.peer_socket_fd = -1;
    p.pid            = static_cast<pid_t>(0);
    p.uid            = static_cast<uid_t>(0);
    p.gid            = static_cast<gid_t>(0);
    EXPECT_EQ(p.peer_socket_fd, -1);
    EXPECT_EQ(p.pid, static_cast<pid_t>(0));
    EXPECT_EQ(p.uid, static_cast<uid_t>(0));
    EXPECT_EQ(p.gid, static_cast<gid_t>(0));
}
