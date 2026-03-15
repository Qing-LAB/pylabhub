# DataHub Design Discussion

**Purpose:** Capture design decisions and open questions for flexible zones and integrity/repair. To be updated as we agree on policy and implementation.

**Cross-platform:** Design decisions and APIs are defined for **all supported platforms** (Windows, Linux, macOS, FreeBSD). Any platform-specific behavior (e.g. shm naming, mutex semantics, error codes) must be documented and kept behind the platform abstraction; review and tests must include cross-platform consistency.

---

## 1. Flexible zone: “undefined” by default, access only after definition/agreement

### 1.1 Intended semantics

- **Initial state:** Flexible zone metadata (and thus access to the zone) is **undefined** until the user provides a **definition** that is **agreed** between producer and consumer.
- **No definition ⇒ no access:** Without a definition and agreement, no API should grant access to the flexible zone memory. Any attempt to read or write the flexible zone in that state is forbidden (e.g. return empty span, or fail explicitly).
- **Definition can be “raw”:** The definition does not have to imply a typed/atomic layout. It can be purely **std::span-based**: a name, offset, and size (and optionally spinlock index). No extra constraints (atomics, alignment) are required for the zone content itself; the only requirement is that producer and consumer agree on the layout (offset/size) so they interpret the same memory consistently.
- **Agreement:** “Agreement” means both sides use the same layout (e.g. from `DataBlockConfig` / `expected_config` at attach time, or from a future schema). Consumer must not access flexible zones unless it was created with a config that defines those zones (e.g. `find_datablock_consumer(..., expected_config)` with matching `flexible_zone_configs`).

### 1.2 Current state vs target

- **Producer:** Creates DataBlock with `DataBlockConfig`; `flexible_zone_configs` populates `m_flexible_zone_info` (name → offset, size, spinlock_index). So producer has a defined layout.
- **Consumer:** If created with `DataBlock(name)` only (no config), `m_flexible_zone_info` is **empty**. The consumer has no zone definitions, but today `flexible_zone_span(index)` and related APIs can still be called; they index into `flexible_zones_info` (the vector on the impl). If that vector is empty, index 0 would be out of range or undefined. So in practice “undefined” is partially there (empty map/vector) but not consistently enforced or documented.
- **Target:** Make “undefined” explicit and enforced:
  - Consumer created **without** a config that defines flexible zones ⇒ `flexible_zone_info()` is empty; `flexible_zone_span(i)` (and typed access) must **not** grant access (e.g. return empty span or fail). No “default” or implicit zone.
  - Consumer created **with** `expected_config` that includes `flexible_zone_configs` ⇒ `flexible_zone_info` is populated from that config (already the case in `find_datablock_consumer_impl`); then span-based access is allowed for those defined zones only.
  - Optionally, add an explicit “zone defined” or “zone undefined” state per zone (e.g. for future use when zones can be added dynamically), but the minimum is: no definition/agreement ⇒ no access.

### 1.3 Concrete steps (to implement)

- Document in the API that flexible zone access is **forbidden** until the zone is defined and agreed (producer config + consumer expected_config).
- Ensure consumer path: if `flexible_zone_info()` is empty, all `flexible_zone_span(i)` / `flexible_zone<T>(i)` return empty span or fail (no silent undefined behavior).
- Keep definition “raw” (offset + size + optional spinlock); no requirement for struct layout or atomics inside the zone; std::span is the accessor. Future schema can still describe the *content* of the zone for typed access, but the gate is “definition + agreement.”

---

## 2. Integrity check and repair: what “lighter repair” means and policy-based thinking

### 2.1 What the current implementation does

**Integrity check** (`datablock_validate_integrity`):

1. Opens the DataBlock via `open_datablock_for_diagnostic` (read-only attach).
2. Validates magic, header version (and optionally header layout hash).
3. If checksums are enabled, it needs to **verify** checksums. To do that it:
   - Builds an `expected_config` from the header (sizes, capacities, etc.).
   - Creates a **full consumer** via `find_datablock_consumer(MessageHub::get_instance(), shm_name, secret, expected_config)`. That implies: discovery via broker, attach, and full consumer lifecycle.
   - Uses that consumer to call `verify_checksum_flexible_zone(i)` and `verify_checksum_slot(i)`.

**Repair** (when `repair == true`):

- For each failed checksum (flexible zone or slot), it creates a **full producer** via `create_datablock_producer(MessageHub::get_instance(), shm_name, policy, expected_config)`.
- Uses that producer to call `update_checksum_flexible_zone(i)` or `update_checksum_slot(i)`.
- So repair creates full producer (and possibly consumer) instances, which typically **register with the broker** and participate in the normal lifecycle. That has side effects: extra REG_REQ, broker state, and process-looking like a normal producer/consumer.

### 2.2 What “lighter repair” would mean

A **lighter** repair path would:

- Use **only** the diagnostic handle (read-only attach: `open_datablock_for_diagnostic` → `SharedMemoryHeader*` + raw buffer pointers).
- For **verify:** Compute BLAKE2b over the relevant ranges (flexible zone or slot) and compare to the checksum stored in the header. No consumer object, no MessageHub, no discovery.
- For **repair:** Compute the correct BLAKE2b and write it into `header->flexible_zone_checksums[i]` or the slot checksum region. No producer object, no broker registration.

So “lighter” = same logical operations (verify checksum, overwrite with correct checksum), but implemented as **direct buffer + header access** behind the diagnostic handle, without creating producer/consumer or touching the broker. That avoids lifecycle and broker side effects and keeps the integrity tool “observer/repair” only.

### 2.3 Policy vs mechanism: how to think about it

The **mechanism** is: how we compute and store checksums (BLAKE2b, header layout, etc.). The **policy** is: what we consider an integrity problem, what we allow as recovery actions, and who is allowed to do them.

**Policy questions worth deciding explicitly:**

1. **What counts as an integrity “problem”?**
   - Magic/version/layout mismatch ⇒ unrecoverable (no repair).
   - Checksum mismatch ⇒ “data may be corrupt or tampered”; do we always allow repair (recompute and overwrite checksum) or only in certain modes (e.g. “repair” flag, or only for specific roles)?

2. **Who is allowed to repair?**
   - Any process with the shared secret (or equivalent) that can open the block?
   - Only a dedicated “admin” or “tool” that doesn’t register as producer/consumer?
   - Should repair be allowed at all when a producer/consumer is attached (concurrent access vs consistency)?

3. **What repair actions are allowed?**
   - **Checksum-only repair:** Recompute and overwrite checksums to match current buffer content. Does not fix corrupted *data*; only makes “verify” pass again. This is what we have today (and what a “lighter” path would still do).
   - **Data repair:** Something that “fixes” data (e.g. zero-fill, or restore from backup). Out of scope for current discussion but worth distinguishing from checksum-only.

4. **Should integrity check/repair go through broker at all?**
   - **Current:** Check uses a full consumer (so discovery/broker); repair uses a full producer (broker registration). That ties integrity to the same lifecycle as normal producers/consumers.
   - **Alternative:** Integrity is a separate “tool” or “admin” path: open via diagnostic handle only, no broker. Then we need a **policy** for when that is allowed (e.g. only if no one is attached, or only for a dedicated admin key, etc.).

So yes: we should treat this as **policy-based**. Concretely:

- Define a small **integrity policy** (or document the current one): what is considered a failure, what repair actions exist, who may call them, and whether they require broker/lifecycle or only diagnostic access.
- Implement **mechanism** to support that policy: e.g. “checksum-only repair via diagnostic handle” (lighter path) if the policy says repair may be done without registering as producer/consumer.

### 2.4 Integrity policy (agreed)

Policy decisions so that implementation and tooling can be consistent:

| Question | Decision |
|----------|----------|
| **What counts as an integrity problem?** | (1) Magic or header version/layout mismatch ⇒ **unrecoverable**; do not attempt repair. (2) Checksum mismatch ⇒ **data may be corrupt or tampered**; checksum-only repair (recompute and overwrite checksum) is allowed when explicitly requested (e.g. `repair == true`). |
| **Who is allowed to repair?** | Any process that can open the DataBlock (e.g. with shared secret or diagnostic attach). No special “admin” role required for checksum-only repair. Optionally, a future policy can restrict repair to when no producer/consumer is attached. |
| **What repair actions are allowed?** | **Checksum-only repair:** Recompute BLAKE2b over the current buffer and overwrite the stored checksum. This does not fix corrupted data; it only makes “verify” pass again. **Data repair** (e.g. zero-fill, restore) is out of scope for this policy. |
| **Should integrity check/repair use the broker?** | **Current (“full”) path:** Check uses a full consumer (discovery via broker); repair uses a full producer (broker registration). **Planned (“lighter”) path:** Check and repair use only the diagnostic handle (open, read header/buffers, compute/compare/update checksums). No broker, no producer/consumer lifecycle. We will add the lighter path and document when each is used (e.g. a flag or a separate `datablock_repair_checksums` that uses diagnostic handle only). |

### 2.5 Suggested next steps

- **Implement lighter path:** Checksum verify and checksum-only repair using only `DataBlockDiagnosticHandle` (no consumer/producer, no MessageHub). Expose via a new API or an option to `datablock_validate_integrity`.
- **Centralize** recovery/error codes and integrity outcomes in `utils/recovery_api.hpp` (and optionally a short recovery/integrity doc).

---

## References

- `DATAHUB_DATABLOCK_CRITICAL_REVIEW.md` – §1.2 (consumer flexible_zone_info), §5.1 (checksum helpers), integrity repair mention.
- `DATAHUB_TODO.md` – Remaining plan, Phase 1.4/1.5, recovery.
- `data_block_recovery.cpp` – `datablock_validate_integrity` (check + repair via consumer/producer).
