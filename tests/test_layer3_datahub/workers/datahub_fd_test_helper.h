#pragma once
/**
 * @file datahub_fd_test_helper.h
 * @brief In-process fd-source producer/consumer pair helper for L3
 *        DataBlock C-API tests under HEP-CORE-0041 1i-cleanup (#275-S2).
 *
 * Tests that previously wrote the legacy secret-shaped name-based
 * pattern:
 *
 *     cfg.shared_secret = SECRET;
 *     auto producer = create_datablock_producer_impl(channel, policy, cfg, ...);
 *     auto consumer = find_datablock_consumer_impl(channel, cfg.shared_secret, &cfg, ...);
 *
 * have been migrated onto this helper.  Post #275-S3/S4/S5 + #316 the
 * `DataBlockConfig::shared_secret` field is gone entirely; under the
 * capability model, producers own their own anonymous memfd and hand
 * the fd to consumers — name + secret have no role.
 *
 * This helper wraps the in-process equivalent: mint an `IShmCapabilityProducer`
 * sized to the layout, build the producer over its memfd via the fd-source
 * factory, then `::dup` the same memfd for the consumer side (in-process
 * substitute for SCM_RIGHTS) and build the consumer.  The dup is closed
 * immediately after the consumer factory dups internally — only the
 * `transport`'s fd persists for the life of the pair.
 *
 * Returned struct's declaration order: `transport` FIRST → destroyed LAST.
 * Producer + consumer (which hold their own internal dups of the fd) get
 * destroyed first in reverse order, matching the production lifetime
 * contract where `RoleHostFrame::shm_transport_` outlives the queues.
 *
 * Usage:
 *
 *     auto p = make_fd_backed_pair(channel, hub::DataBlockPolicy::RingBuffer, cfg);
 *     ASSERT_NE(p.producer, nullptr);
 *     ASSERT_NE(p.consumer, nullptr);
 *     // ... use p.producer + p.consumer same as before ...
 *
 * Schema-typed factories (`find_datablock_consumer<F, D>`) callers can
 * use the symmetric `find_datablock_consumer_from_fd<F, D>` template
 * directly (declared in `utils/data_block.hpp` next to its non-fd twin);
 * this helper covers the non-template case.
 */

#include "utils/data_block.hpp" // create_datablock_producer_from_fd_impl etc.
#include "utils/data_block_config.hpp"
#include "utils/data_block_policy.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include <memory>
#include <string>
#include <unistd.h> // ::dup, ::close

namespace pylabhub::tests::helper
{

/// Owns the L1 transport + producer for an in-process fd-source
/// producer-only attach.  Producer-only tests (release_write_slot
/// validation, write/commit bounds, double-release idempotence) use
/// this — avoids spinning up a consumer attach the test never reads.
struct FdBackedProducer
{
    std::unique_ptr<pylabhub::utils::security::IShmCapabilityProducer> transport;
    std::unique_ptr<pylabhub::hub::DataBlockProducer> producer;

    FdBackedProducer() = default;
    FdBackedProducer(const FdBackedProducer &) = delete;
    FdBackedProducer &operator=(const FdBackedProducer &) = delete;
    FdBackedProducer(FdBackedProducer &&) = default;
    FdBackedProducer &operator=(FdBackedProducer &&) = default;
};

/// Owns the L1 transport + producer + consumer for an in-process
/// fd-source pair.  Declared so that destruction (reverse order)
/// tears down consumer → producer → transport — transport outlives
/// the queues' mmap regions.
struct FdBackedDataBlock
{
    std::unique_ptr<pylabhub::utils::security::IShmCapabilityProducer> transport;
    std::unique_ptr<pylabhub::hub::DataBlockProducer> producer;
    std::unique_ptr<pylabhub::hub::DataBlockConsumer> consumer;

    FdBackedDataBlock() = default;
    FdBackedDataBlock(const FdBackedDataBlock &) = delete;
    FdBackedDataBlock &operator=(const FdBackedDataBlock &) = delete;
    FdBackedDataBlock(FdBackedDataBlock &&) = default;
    FdBackedDataBlock &operator=(FdBackedDataBlock &&) = default;
};

/// Build a fd-source DataBlock producer ONLY over an in-process
/// anonymous memfd — no consumer attach.  Returns a FdBackedProducer
/// with both handles populated (or either null on failure — callers
/// ASSERT_NE per existing test style).
///
/// Use this instead of `make_fd_backed_pair` when the test only
/// exercises producer-side API (invalid-handle release, write/commit
/// bounds, double-release idempotence) — spinning up a consumer the
/// test never reads is wasted work and risks unrelated attach-failure
/// modes leaking into the assertion.
///
/// @param channel     Logical name passed verbatim to the fd-source
///                    factory (used for log lines + DataBlock identity
///                    label; no kernel-namespace meaning on this path).
/// @param policy      DataBlock policy (Single / DoubleBuffer / RingBuffer).
/// @param cfg         Sizing + policy config.  `cfg.shared_secret` is
///                    IGNORED — the fd-source factory doesn't read it.
/// @param fz_schema   Optional FlexZone schema for hash validation.
/// @param db_schema   Optional DataBlock-slot schema for hash validation.
[[nodiscard]] inline FdBackedProducer
make_fd_backed_producer(const std::string &channel, pylabhub::hub::DataBlockPolicy policy,
                        const pylabhub::hub::DataBlockConfig &cfg,
                        const pylabhub::schema::SchemaInfo *fz_schema = nullptr,
                        const pylabhub::schema::SchemaInfo *db_schema = nullptr)
{
    namespace sec = pylabhub::utils::security;
    FdBackedProducer out;

    const size_t total = pylabhub::hub::datablock_layout_total_size(cfg);
    if (total == 0)
        return out;

    out.transport = sec::create_shm_capability_producer(total);
    if (!out.transport)
        return out;

    out.producer = pylabhub::hub::create_datablock_producer_from_fd_impl(
        channel, out.transport->borrow_fd(), policy, cfg, fz_schema, db_schema);
    return out;
}

/// Build a fd-source DataBlock producer + consumer pair over a single
/// in-process anonymous memfd.  Returns a FdBackedDataBlock with all
/// three handles populated (or any of them null on failure — callers
/// ASSERT_NE per existing test style).
///
/// @param channel       Logical name passed verbatim to the fd-source
///                      factories (used for log lines + DataBlock identity
///                      label; no kernel-namespace meaning on this path).
/// @param policy        DataBlock policy (Single / DoubleBuffer / RingBuffer).
/// @param cfg           Sizing + policy config.  `cfg.shared_secret` is
///                      IGNORED — the fd-source factories don't read it.
/// @param fz_schema     Optional FlexZone schema for hash validation.
/// @param db_schema     Optional DataBlock-slot schema for hash validation.
/// @param consumer_uid  Optional consumer UID (registered in producer's
///                      heartbeat slot).
/// @param consumer_name Optional consumer human-readable name.
[[nodiscard]] inline FdBackedDataBlock
make_fd_backed_pair(const std::string &channel, pylabhub::hub::DataBlockPolicy policy,
                    const pylabhub::hub::DataBlockConfig &cfg,
                    const pylabhub::schema::SchemaInfo *fz_schema = nullptr,
                    const pylabhub::schema::SchemaInfo *db_schema = nullptr,
                    const char *consumer_uid = nullptr, const char *consumer_name = nullptr)
{
    namespace sec = pylabhub::utils::security;
    FdBackedDataBlock out;

    const size_t total = pylabhub::hub::datablock_layout_total_size(cfg);
    if (total == 0)
        return out;

    out.transport = sec::create_shm_capability_producer(total);
    if (!out.transport)
        return out;

    out.producer = pylabhub::hub::create_datablock_producer_from_fd_impl(
        channel, out.transport->borrow_fd(), policy, cfg, fz_schema, db_schema);
    if (!out.producer)
        return out;

    // In-process substitute for the §5.5 SCM_RIGHTS handoff: dup the
    // producer's borrowed fd for the consumer side.  The consumer
    // factory dups internally on its own dup-source contract; we close
    // ours immediately after the factory returns.
    const int rx_fd = ::dup(out.transport->borrow_fd());
    if (rx_fd < 0)
        return out;
    out.consumer = pylabhub::hub::find_datablock_consumer_from_fd_impl(
        channel, rx_fd, &cfg, fz_schema, db_schema, consumer_uid, consumer_name);
    ::close(rx_fd);
    return out;
}

/// Typed counterpart of `make_fd_backed_producer` — same shape but
/// schema is derived at compile time from the F (FlexZone) / D (DataBlock)
/// type tags via `create_datablock_producer_from_fd<F, D>`.  Use this
/// when the test exercises typed APIs (`with_transaction<F, D>`,
/// `TestDataBlock` casts) and would otherwise re-derive the schema
/// inline at the call site.
template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] inline FdBackedProducer
make_fd_backed_producer_typed(const std::string &channel, pylabhub::hub::DataBlockPolicy policy,
                              const pylabhub::hub::DataBlockConfig &cfg)
{
    namespace sec = pylabhub::utils::security;
    FdBackedProducer out;

    const size_t total = pylabhub::hub::datablock_layout_total_size(cfg);
    if (total == 0)
        return out;

    out.transport = sec::create_shm_capability_producer(total);
    if (!out.transport)
        return out;

    out.producer = pylabhub::hub::create_datablock_producer_from_fd<FlexZoneT, DataBlockT>(
        channel, out.transport->borrow_fd(), policy, cfg);
    return out;
}

/// Typed counterpart of `make_fd_backed_pair` — same shape but the
/// producer + consumer factories derive schemas at compile time from
/// the F (FlexZone) / D (DataBlock) type tags via the
/// `_from_fd<F, D>` templates.  Saves ~12 lines of inline transport
/// mint + dup + close at every typed-pair test site.
template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] inline FdBackedDataBlock
make_fd_backed_pair_typed(const std::string &channel, pylabhub::hub::DataBlockPolicy policy,
                          const pylabhub::hub::DataBlockConfig &cfg,
                          const char *consumer_uid = nullptr, const char *consumer_name = nullptr)
{
    namespace sec = pylabhub::utils::security;
    FdBackedDataBlock out;

    const size_t total = pylabhub::hub::datablock_layout_total_size(cfg);
    if (total == 0)
        return out;

    out.transport = sec::create_shm_capability_producer(total);
    if (!out.transport)
        return out;

    out.producer = pylabhub::hub::create_datablock_producer_from_fd<FlexZoneT, DataBlockT>(
        channel, out.transport->borrow_fd(), policy, cfg);
    if (!out.producer)
        return out;

    // In-process substitute for the §5.5 SCM_RIGHTS handoff: dup the
    // producer's fd for the consumer side.  The consumer factory dups
    // internally; we close ours immediately after.
    const int rx_fd = ::dup(out.transport->borrow_fd());
    if (rx_fd < 0)
        return out;
    out.consumer = pylabhub::hub::find_datablock_consumer_from_fd<FlexZoneT, DataBlockT>(
        channel, rx_fd, cfg, consumer_uid, consumer_name);
    ::close(rx_fd);
    return out;
}

} // namespace pylabhub::tests::helper
