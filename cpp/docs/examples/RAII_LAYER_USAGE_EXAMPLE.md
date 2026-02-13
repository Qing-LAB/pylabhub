# C++ RAII Layer Usage Example

This document shows how to use the DataHub C++ RAII layer with concrete code. It also clarifies **what is implemented today** vs **what would require future work** regarding schema/fingerprint and type verification.

---

## 1. How DataBlock is Initialized

```cpp
#include "plh_datahub.hpp"

using namespace pylabhub::hub;

// --- Producer (creator) ---
MessageHub& hub = MessageHub::get_instance();
hub.connect("tcp://127.0.0.1:5555");  // or leave unconnected for in-process

DataBlockConfig config{};
config.name = "my_datablock";
config.shared_secret = 0x12345678;
config.ring_buffer_capacity = 4;
config.unit_block_size = DataBlockUnitSize::Size4K;
config.policy = DataBlockPolicy::RingBuffer;
config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
config.enable_checksum = true;
config.checksum_policy = ChecksumPolicy::Enforced;

// Flexible zone: define layout by (name, size) per zone.
// Size must match your struct; you are responsible for consistency.
config.flexible_zone_configs.push_back({"metadata", sizeof(MyMetadataStruct), -1});
config.flexible_zone_configs.push_back({"aux", sizeof(MyAuxStruct), 0});  // spinlock 0

auto producer = create_datablock_producer(hub, config.name, config.policy, config);

// --- Consumer (attacher) ---
// expected_config MUST match producer's config (flexible_zone_size, ring_buffer_capacity,
// unit_block_size, enable_checksum). Layout checksum in header is validated on attach.
DataBlockConfig expected_config = config;  // same layout
auto consumer = find_datablock_consumer(hub, config.name, config.shared_secret, expected_config);
```

**Config defaults:** `DataBlockConfig config{}` has sensible defaults (`shared_secret=0`, `ring_buffer_capacity=1`, `policy=RingBuffer`, `enable_checksum=false`, etc.). Set only the fields you need; avoid partial initialization surprises. Same for `FlexibleZoneConfig` and `SchemaInfo`.

**What is stored/verified at init:**

- **Layout checksum** (BLAKE2b of layout-defining header fields: ring_buffer_capacity, unit_block_size, logical_unit_size, flexible_zone_size, enable_checksum, policy, consumer_sync_policy) is stored in the segment header at creation. On consumer attach and integrity validation, it is recomputed and compared — mismatch means refuse attach or report integrity failure.
- **Schema hash** (optional): When using schema-aware `create_datablock_producer<Schema>(...)` and `find_datablock_consumer<Schema>(...)`, a BLDS schema hash is stored in the header and validated on consumer attach. This applies to the **overall data schema**, not per-zone or per-slot type mapping.

**Physical vs logical slot size:**

- **Invariant: logical ≥ physical always.** Physical is `unit_block_size` (allocation granularity). Logical is the slot stride and `buffer_span().size()`. Set `logical_unit_size = 0` to mean “use physical” (logical = physical). If set, `logical_unit_size` must be a multiple of physical (and thus ≥ physical). There is no case where logical < physical.
- **Invariant: logical ≤ total structured buffer.** Total buffer = slot_count × effective_logical; slot_count is at least 1, so there is always at least one logical unit (logical ≤ total buffer).

**Access and validation (single control surface):**

- **Access** to layout information (slot stride, region offsets) is internal: all slot and region pointers/sizes are derived from **DataBlockLayout** (one source). Application code uses handles and transaction context (`ctx.slot()`, `ctx.flexible_zone(i)`); it does not touch layout math.
- **Validation** entry points are grouped in one place in the API: `validate_header_layout_hash` (ABI), `store_layout_checksum` / `validate_layout_checksum` (segment layout values). Attach runs header hash then layout checksum + config match; recovery/integrity use the same layout checksum. See DATAHUB_CPP_ABSTRACTION_DESIGN.md §4.10.

---

## 2. How Flexible Zone is Formatted/Mapped to a User Structure

The flexible zone is configured by **size per zone** (via `FlexibleZoneConfig`). There is **no built-in type mapping** — you declare a struct, set `size = sizeof(YourStruct)` in config, and manually map bytes to your struct inside the transaction.

```cpp
// User-defined struct for zone 0 (must be trivially copyable for shared memory)
struct MyMetadataStruct {
    uint64_t sequence;
    double   timestamp;
    char     source[32];
};
static_assert(std::is_trivially_copyable_v<MyMetadataStruct>);

// Config: zone 0 size = sizeof(MyMetadataStruct)
config.flexible_zone_configs.push_back({"metadata", sizeof(MyMetadataStruct), -1});

// Inside with_write_transaction: access zone 0 as MyMetadataStruct
with_write_transaction(*producer, 5000, [](WriteTransactionContext& ctx) {
    std::span<std::byte> zone0 = ctx.flexible_zone(0);  // index = 0 for first zone
    if (zone0.size() < sizeof(MyMetadataStruct))
        throw std::runtime_error("Zone 0 too small");

    MyMetadataStruct& meta = *reinterpret_cast<MyMetadataStruct*>(zone0.data());
    meta.sequence = 42;
    meta.timestamp = 1234567.89;
    std::strncpy(meta.source, "sensor_A", sizeof(meta.source) - 1);

    ctx.slot().write(...);  // slot buffer separate from flexible zone
    ctx.slot().commit(...);
});

// Inside with_read_transaction: read zone 0
with_read_transaction(*consumer, slot_id, 5000, [](ReadTransactionContext& ctx) {
    std::span<const std::byte> zone0 = ctx.flexible_zone(0);
    if (zone0.size() < sizeof(MyMetadataStruct))
        return;

    const MyMetadataStruct& meta = *reinterpret_cast<const MyMetadataStruct*>(zone0.data());
    std::cout << "sequence=" << meta.sequence << ", source=" << meta.source << std::endl;
});
```

**Current limitation:** The layout (zone count, sizes) is enforced and checksum-protected. The **interpretation** (which struct type you use for each zone) is **not** stored or verified. You must ensure `sizeof(MyMetadataStruct)` matches the config and that producer and consumer agree on the struct layout. A future `with_typed_flexible_zone<T>(ctx, zone_index, func)` could add type checks and optionally schema verification.

---

## 3. How Ring-Buffer Unit (Slot) is Formatted/Mapped to a User Structure

Use `with_typed_write<T>` and `with_typed_read<T>` — they perform a runtime `sizeof(T) <= slot_buffer_size` and alignment check, then pass `T&` / `const T&` to your lambda.

```cpp
struct SlotPayload {
    uint64_t id;
    uint32_t value;
    char     tag[16];
};
static_assert(std::is_trivially_copyable_v<SlotPayload>);

// Write: acquire slot, get T&, commit sizeof(T)
with_typed_write<SlotPayload>(*producer, 5000, [](SlotPayload& data) {
    data.id = 0xCAFEBABE;
    data.value = 42;
    std::strncpy(data.tag, "event", sizeof(data.tag) - 1);
});

// Read: acquire slot by slot_id, get const T&
with_typed_read<SlotPayload>(*consumer, slot_id, 5000, [](const SlotPayload& data) {
    std::cout << "id=" << data.id << ", value=" << data.value << std::endl;
});
```

**Current limitation:** The typed API checks `sizeof(T)` and alignment at **call time**. It does **not** verify `T` against a stored schema/fingerprint in the header. The optional schema hash (from schema-aware create/find) describes the overall data schema (BLDS) but is not wired into `with_typed_write<T>` to validate that `T` matches the registered schema.

---

## 4. How Mapping Information is Stored/Verified

| What                         | Stored                    | Verified on attach / use      |
|-----------------------------|---------------------------|-------------------------------|
| Layout (zone sizes, counts) | Layout checksum in header | Attach, integrity validation  |
| Header ABI                  | Header layout hash        | Attach                        |
| Data schema (optional)      | schema_hash in header     | Consumer find (schema-aware)  |
| Flexible zone struct type   | **Not stored**            | **Not verified**              |
| Slot buffer struct type     | **Not stored**            | Size/alignment at call time   |

So: layout and optional data schema are stored and verified; per-zone and per-slot type mappings are not yet part of the protocol.

---

## 5. How Type Information Flows in with_read/write_transaction

### 5a. `with_write_transaction` / `with_read_transaction` (context-based)

The lambda receives a **context**, not a typed handle. Type information is **not** passed in; you access spans and cast manually.

```cpp
with_write_transaction(*producer, 5000, [](WriteTransactionContext& ctx) {
    // Slot buffer: raw span
    std::span<std::byte> buf = ctx.slot().buffer_span();
    // You cast manually:
    auto* payload = reinterpret_cast<SlotPayload*>(buf.data());
    payload->id = 1;

    // Flexible zone: raw span per zone
    std::span<std::byte> zone0 = ctx.flexible_zone(0);
    auto* meta = reinterpret_cast<MyMetadataStruct*>(zone0.data());
    meta->sequence = 1;

    ctx.slot().commit(sizeof(SlotPayload));
});

with_read_transaction(*consumer, slot_id, 5000, [](ReadTransactionContext& ctx) {
    if (!ctx.validate_read()) { /* slot overwritten */ return; }

    // Same manual cast
    std::span<const std::byte> buf = ctx.slot().buffer_span();
    const auto* payload = reinterpret_cast<const SlotPayload*>(buf.data());

    std::span<const std::byte> zone0 = ctx.flexible_zone(0);
    const auto* meta = reinterpret_cast<const MyMetadataStruct*>(zone0.data());
});
```

### 5b. `with_typed_write<T>` / `with_typed_read<T>` (slot buffer only)

Here, **T** is the type information. The API checks `sizeof(T)` and alignment, then passes `T&` / `const T&` so you access members directly — no manual cast for the slot buffer.

```cpp
with_typed_write<SlotPayload>(*producer, 5000, [](SlotPayload& data) {
    data.id = 1;           // direct member access
    data.value = 42;
    std::strncpy(data.tag, "evt", sizeof(data.tag) - 1);
});

with_typed_read<SlotPayload>(*consumer, slot_id, 5000, [](const SlotPayload& data) {
    std::cout << data.id << ", " << data.value << std::endl;  // direct access
});
```

Flexible zone still uses the context: `ctx.flexible_zone(i)` and manual cast. There is no `with_typed_flexible_zone<T>` yet.

---

## 6. Combined Example: Init, Flexible Zone, Typed Slot, RAII

```cpp
#include "plh_datahub.hpp"
#include <cstring>
#include <iostream>

using namespace pylabhub::hub;

struct SlotPayload { uint64_t id; uint32_t value; };
struct Zone0Meta { uint64_t seq; char name[16]; };

static_assert(std::is_trivially_copyable_v<SlotPayload>);
static_assert(std::is_trivially_copyable_v<Zone0Meta>);

int main() {
    MessageHub& hub = MessageHub::get_instance();
    hub.connect("tcp://127.0.0.1:5555");  // or "" for in-process only

    DataBlockConfig config;
    config.name = "example_block";
    config.shared_secret = 0xDEADBEEF;
    config.ring_buffer_capacity = 4;
    config.unit_block_size = DataBlockUnitSize::Size4K;
    config.flexible_zone_configs.push_back({"meta", sizeof(Zone0Meta), -1});

    auto producer = create_datablock_producer(hub, config.name,
                                              DataBlockPolicy::RingBuffer, config);
    auto consumer = find_datablock_consumer(hub, config.name, config.shared_secret, config);

    // --- Write path: use with_write_transaction when you need both slot and flexible zone ---
    with_write_transaction(*producer, 5000, [](WriteTransactionContext& ctx) {
        // Slot buffer: either manual write or typed access via ctx.slot()
        SlotPayload p = {1, 100};
        ctx.slot().write(&p, sizeof(p));

        // Flexible zone: manual map to your struct (you ensure size matches config)
        std::span<std::byte> z0 = ctx.flexible_zone(0);
        Zone0Meta* meta = reinterpret_cast<Zone0Meta*>(z0.data());
        meta->seq = 1;
        std::strncpy(meta->name, "writer", sizeof(meta->name) - 1);

        ctx.slot().commit(sizeof(p));
    });

    // --- Or use with_typed_write when you only need typed slot access (no flexible zone) ---
    with_typed_write<SlotPayload>(*producer, 5000, [](SlotPayload& p) {
        p.id = 2;
        p.value = 200;
    });

    // --- Read path ---
    uint64_t sid = 0;  // or get from consumer->slot_iterator().last_slot_id() / try_next
    with_read_transaction(*consumer, sid, 5000, [](ReadTransactionContext& ctx) {
        if (!ctx.validate_read()) return;
        const SlotPayload* p = reinterpret_cast<const SlotPayload*>(ctx.slot().buffer_span().data());
        const Zone0Meta* m = reinterpret_cast<const Zone0Meta*>(ctx.flexible_zone(0).data());
        std::cout << "slot: id=" << p->id << " value=" << p->value << "\n";
        std::cout << "zone0: seq=" << m->seq << " name=" << m->name << "\n";
    });

    return 0;
}
```

**Style choice:**
- **`with_write_transaction` / `with_read_transaction`:** Use when you need slot buffer and flexible zone together. You manually cast slot buffer and zones to your structs.
- **`with_typed_write<T>` / `with_typed_read<T>`:** Use when you only need typed slot access; the lambda receives `T&` / `const T&` directly. Flexible zone is not available in that lambda.

---

## 7. Gap vs Your Supposition

You supposed: *"when datablock is initialized with mapping structure info, schema/fingerprint is built, and during use within the with_* block, the type is checked against actual use."*

**Implemented today:**

- Layout (zone sizes, slot layout) is checksum-protected and validated on attach/integrity.
- Optional BLDS schema hash is stored and validated when using schema-aware create/find.
- `with_typed_write<T>` / `with_typed_read<T>` enforce `sizeof(T)` and alignment at call time.

**Not implemented:**

- No stored fingerprint for flexible zone struct type; no verification when accessing `ctx.flexible_zone(i)`.
- No verification that `T` in `with_typed_write<T>` matches a stored schema; the schema hash is not used for that.

A future design could:

1. Store a type/schema fingerprint per flexible zone and per slot buffer at creation.
2. In `with_typed_write<T>`, `with_typed_read<T>`, and a potential `with_typed_flexible_zone<T>`, verify that `T`’s fingerprint matches the stored one before allowing access.

That would give the end-to-end type safety you described.
