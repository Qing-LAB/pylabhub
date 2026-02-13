# DataHub — Topic summary

**Purpose:** Short index and developer entry point for the Data Exchange Hub (DataBlock). For execution and design details, use the canonical docs below.

---

## Quick links

| Need | Document |
|------|----------|
| **What to do next / checklist** | **`docs/DATAHUB_TODO.md`** (single execution plan) |
| **How to implement / patterns** | **`docs/IMPLEMENTATION_GUIDANCE.md`** |
| **Design spec (authoritative)** | **`docs/hep/HEP-CORE-0002-DataHub-FINAL.md`** (implementation status there syncs with DATAHUB_TODO) |
| **What’s in the HEP / document guide** | This file (§ Document guide below) |
| **Test plan and Phase A–D** | **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`** (priorities in DATAHUB_TODO) |
| **Design rationale / critical review** | `docs/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md`, `docs/DATAHUB_DESIGN_DISCUSSION.md` |

See **`docs/DOC_STRUCTURE.md`** for the full documentation layout.

---

## API and examples (for developers)

**Headers:** Include **`plh_datahub.hpp`** (umbrella; pulls in MessageHub, DataBlock, JsonConfig). Lifecycle (Logger, CryptoUtils) is required for DataBlock.

**Creating a producer and writing a slot:**

```cpp
#include "plh_datahub.hpp"

pylabhub::utils::LifecycleGuard app(
    pylabhub::utils::MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::CryptoUtils::GetLifecycleModule()
    )
);

pylabhub::hub::MessageHub hub;
pylabhub::hub::DataBlockConfig config{};
config.name = "my_channel";
config.shared_secret = 0;
config.physical_page_size = pylabhub::hub::DataBlockPageSize::Size4K;
config.ring_buffer_capacity = 4;
config.policy = pylabhub::hub::DataBlockPolicy::RingBuffer;
config.consumer_sync_policy = pylabhub::hub::ConsumerSyncPolicy::Single_reader;
config.enable_checksum = true;
config.checksum_policy = pylabhub::hub::ChecksumPolicy::Enforced;

auto producer = pylabhub::hub::create_datablock_producer(hub, config.name, config.policy, config);
if (auto slot = producer->acquire_write_slot(5000)) {
    std::span<uint8_t> buf = slot->buffer_span();
    // ... fill buf ...
    slot->commit(used_bytes);
    slot->release_write_slot();
}
```

**Finding a consumer and reading:**

```cpp
uint64_t secret = producer->shared_secret();
auto consumer = pylabhub::hub::find_datablock_consumer(hub, "my_channel", secret, std::nullopt);
if (consumer) {
    auto it = consumer->slot_iterator();
    if (auto h = it.try_next(1000)) {
        std::span<const uint8_t> buf = h->buffer_span();
        // ... read buf ...
        h->release_consume_slot();
    }
}
```

**Config:** Layout- and mode-critical parameters must be set explicitly (producer creation fails otherwise): `policy`, `consumer_sync_policy`, `physical_page_size`, `ring_buffer_capacity` (≥ 1). Config is validated **before** any memory creation in a single place: the DataBlock creator constructor. See **IMPLEMENTATION_GUIDANCE.md** § "Config validation and memory block setup" and **RAII_LAYER_USAGE_EXAMPLE.md**.

**Policies:** `DataBlockPolicy` (Single, DoubleBuffer, RingBuffer); `ConsumerSyncPolicy` (Latest_only, Single_reader, Sync_reader); `ChecksumPolicy` (Manual, Enforced). See **HEP-CORE-0002** for layout, protocol, and recovery.

**Tests:** Run **`test_layer3_datahub`** (e.g. `./test_layer3_datahub --gtest_filter=SlotProtocolTest.*`). See **README_testing.md** for the test layout.

---

## Document guide (what's in the DataHub spec)

The **single authoritative spec** is **`docs/hep/HEP-CORE-0002-DataHub-FINAL.md`**. Summary of its structure:

| Section | Content |
|---------|---------|
| **1** Executive Summary | Design philosophy, key decisions (Dual-Chain, SlotRWCoordinator, Minimal Broker), implementation status |
| **2** System Architecture | Diagrams, dual-chain (flexible zones vs fixed buffers), two-tier sync |
| **3** Memory Layout | 4KB header, SharedMemoryHeader, SlotRWState (48B), ConsumerSyncPolicy, checksum layout |
| **4** Synchronization | SlotRWState coordination, TOCTTOU mitigation, memory ordering, SharedSpinLock |
| **5** API Specification | C API, C++ wrappers, Transaction API, examples |
| **6** Control Plane | Broker protocol (REG/DISC/DEREG), discovery, heartbeat |
| **7** Usage Patterns | Sensor streaming, video frames, data queue, multi-camera sync |
| **8** Error Handling & Recovery | CLI (diagnose, force-reset, auto-recover), PID liveness, procedures |
| **9–15** | Performance, security, schema (P9), implementation guidelines, testing, deployment, appendices |

**Archived design docs** (historical only): `docs/archive/data-hub/` — CRITICAL_REVIEW, P7/P8/P9 design docs, old HEP drafts. Use **HEP-CORE-0002** and **DATAHUB_TODO.md** for current plan and status.

**Audience:** Implementers → §1, §4, §5, §12. Users → §1.2, §7, §5.4. Operators → §8, §14. Execution order and checklist → **`docs/DATAHUB_TODO.md`**.
