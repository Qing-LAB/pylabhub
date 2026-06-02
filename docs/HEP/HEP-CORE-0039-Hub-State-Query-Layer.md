# HEP-CORE-0039: Hub State Query Layer

| Property       | Value |
|----------------|-------|
| **HEP**        | `HEP-CORE-0039` |
| **Title**      | Hub State Query Layer — Capture + Query Framework, Named Helpers, Convention Lock-in |
| **Status**     | Draft — design premises ratified 2026-06-02; implementation pending.  Promoted from `docs/tech_draft/hub_state_query_layer_design.md` (2026-05-20) and expanded with internal-broker query helpers + inline-join inventory + Phase D coupling 2026-06-02. |
| **Created**    | 2026-05-20 (tech_draft); 2026-06-02 (HEP) |
| **Area**       | `pylabhub-utils` (`HubState` + queries + script bindings) |
| **Depends on** | HEP-CORE-0033 (Hub Character — owns HubState lifecycle + HubAPI surface), HEP-CORE-0023 (Startup Coordination — channel-observable semantics consumed here), HEP-CORE-0019 (Metrics Plane — query-driven model that this framework complements on the script side) |
| **Amends**     | HEP-CORE-0033 §8 (adds `captured_mono` / `hub_uid` / `snapshot_seq` metadata to the documented `HubStateSnapshot` struct); HEP-CORE-0033 §12.3 (adds `snapshot()` to the HubAPI read surface as the primary primitive); HEP-CORE-0019 §3 (cross-references the script-side snapshot path as a parallel entry alongside the broker-side `MetricsStore` / `METRICS_REQ` path) |
| **Reference**  | This HEP is the authoritative design for the read-side data-access framework on the hub. Mutators (the `HubState::_on_*` family) and wire serializers (`hub_state_json.hpp`) are out of scope. |

---

## 1. Motivation

The hub keeps a federated state model — channels, roles, bands, peers,
SHM blocks, schemas, counters — across several maps inside
`HubState`. Reading that state correctly today requires the caller
to know:

- which map to consult,
- what the lock discipline is,
- when a single-key lookup is enough vs. when a coherent multi-aspect
  view is needed,
- when the existing `compute_channel_observable` / `find_*` helpers
  already cover the case, and
- how to assemble a join across maps without drifting.

The survey conducted 2026-06-02 across `src/utils/ipc/broker_service.cpp`,
`hub_state.cpp`, `hub_state_json.cpp`, `admin_service.cpp`, and
`hub_api.cpp` found ~25 sites where the same join is open-coded
inline despite the existence of canonical helpers — and several
patterns with no canonical helper yet, forcing each caller to invent
one. This drift is the load-bearing reason for the HEP:

> When the right helper exists, callers don't always find it. When the
> right helper does not exist, every caller invents a different shape.

The fix is structural, not cosmetic:

1. **Decouple capture from query.** Capture (`HubState::snapshot()`)
   is the one expensive, locked, deep-copy operation. Query is the
   cheap, pure, lock-free operation over a captured snapshot.
2. **Promote inline joins to named helpers** with codified return
   types and a single convention for parameters.
3. **Document the convention** so future code uses the framework
   instead of recreating the join.
4. **Apply the framework to the densest existing duplication site**
   (the `check_heartbeat_timeouts` 190-line sweep) as the concrete
   migration proof.

This HEP defines the framework. The concrete migration site list
lives in `docs/todo/QUERY_LAYER_TODO.md` (sibling subtopic TODO so
the HEP stays timeless).

## 2. Design premises (ratified by designer 2026-06-02)

1. **The capture / query split is the load-bearing contract.**
   `HubStateSnapshot` is the only object that crosses the
   `shared_lock` boundary on a read path. Once obtained, it is
   passed to query helpers by `const &`; the helpers neither lock
   nor re-snapshot.
2. **Two query surfaces, one framework.** Script-facing queries
   return `nlohmann::json` (the JSON path scripts already expect).
   Internal broker / hub_state queries return **domain types**
   (`std::optional<T>`, `const T *`, `std::vector<const T *>`, or
   visitor-pattern `for_each_*`). Both surfaces obey the same
   capture / lock rules; they differ only in return type.
3. **Use what already exists. Add only what's missing.** Existing
   methods (`HubState::channel/role/band/peer/shm_block/schema`,
   `ChannelEntry::find_producer/find_consumer/first_producer/observe`,
   `RoleEntry::find_presence`, `compute_channel_observable`,
   `observe_channel`) are part of the framework as-is. New helpers
   fill genuine gaps surfaced by the survey.
4. **Conventions are codified.** Parameter-type drift
   (`std::string_view` vs `const std::string&`), return-type drift
   (`std::optional<T>` vs raw pointer), and naming drift
   (`find_*` vs `observe` vs `compute_*`) are settled here. New
   helpers follow the convention; older helpers are tagged for
   migration when touched.
5. **Mutators stay out of scope.** The `HubState::_on_*` family,
   `_dereg_role_on_channel_locked`-shape helpers, and the writer
   lock are governed by HEP-CORE-0033 §"Hub State Mutation" and
   the Core Structure Change Protocol in
   `docs/IMPLEMENTATION_GUIDANCE.md`. The Query Layer reads; it
   does not write.
6. **Wire serializers stay out of scope.** `hub_state_json.hpp`
   already owns the JSON wire shape (`channel_to_json`,
   `role_to_json`, etc.). The Query Layer's script-facing helpers
   delegate to existing serializers where possible; they do not
   replace them.

## 3. Three-layer model

### Layer 1 — Capture

One operation, one `shared_lock`, one deep-copy, one self-describing
result.

```cpp
namespace pylabhub::hub
{
struct HubStateSnapshot
{
    // ── Data containers (existing) ─────────────────────────────────
    std::unordered_map<std::string, ChannelEntry>  channels;
    std::unordered_map<std::string, RoleEntry>     roles;
    std::unordered_map<std::string, BandEntry>     bands;
    std::unordered_map<std::string, PeerEntry>     peers;
    std::unordered_map<std::string, ShmBlockRef>   shm_blocks;
    std::map<SchemaKey, schema::SchemaRecord>      schemas;
    BrokerCounters                                 counters;

    // ── Metadata ───────────────────────────────────────────────────
    std::chrono::system_clock::time_point captured_at;    // EXISTS — wall-clock
    std::chrono::steady_clock::time_point captured_mono;  // NEW — monotonic
    std::string                            hub_uid;        // NEW — origin disambiguation
    std::uint64_t                          snapshot_seq{0};// NEW — monotonic per hub
};

class HubState
{
public:
    HubStateSnapshot snapshot() const;  // shared_lock + deep copy + metadata
};
}
```

**Metadata justification:**

| Field | Why |
|---|---|
| `captured_at` | Wall-clock for human-readable logs / forensic timestamps. Already in place at `hub_state.cpp:143`. |
| `captured_mono` | Steady-clock for age math (`age_seconds()`); immune to NTP jumps. |
| `hub_uid` | Disambiguates dual-hub-processor snapshots — a log line "channels=12" is ambiguous between hubs; "channels=12, hub_uid=A" is not. |
| `snapshot_seq` | Per-hub monotonic counter. Lets callers detect missed snapshots (`if seq != last_seq + 1: log.warn`). |

**Lifetime + ownership.** `HubStateSnapshot` is a value type. Callers
may keep it across event-loop ticks, copy it, or drop it. There is
no aliasing back to live `HubState`. Mutations after capture are
invisible to the snapshot. This is the load-bearing property — it's
why the framework works at all.

**When to call `snapshot()`.** Cold paths only: admin RPCs,
observability dumps, periodic telemetry, audit logs, federation
peer-hello payload assembly, script-facing `list_*` accessors. Hot
paths (broker dispatch per-frame, ZAP handler per-handshake) MUST
NOT call `snapshot()` — they hit `HubState::channel(name)` or
similar single-key accessors directly (Layer 2 §3.2).

### Layer 2 — Query primitives

Layer 2 splits into two complementary surfaces. Both obey the same
"snapshot or live state under shared_lock" boundary; they differ
only in return type.

#### 2a. Internal-broker query primitives (domain types)

Used by broker handlers, sweep loops, `RoleAPIBase` glue, and any
in-process consumer that wants typed access. Return raw types
(`const T *`, `std::optional<T>`, `std::vector<const T *>`,
visitor callbacks).

**Existing primitives that are part of the framework as-is:**

```cpp
// HubState single-key accessors (cold path; copy-out under shared_lock)
std::optional<ChannelEntry>  HubState::channel(const std::string &name) const;
std::optional<RoleEntry>     HubState::role(const std::string &uid) const;
std::optional<BandEntry>     HubState::band(const std::string &name) const;
std::optional<PeerEntry>     HubState::peer(const std::string &hub_uid) const;
std::optional<ShmBlockRef>   HubState::shm_block(const std::string &channel) const;
std::optional<schema::SchemaRecord>
                             HubState::schema(const std::string &owner_uid,
                                              const std::string &schema_id) const;

// ChannelEntry per-channel walks (in-place pointer; caller holds parent)
const ProducerEntry *ChannelEntry::find_producer(const std::string &role_uid) const noexcept;
const ConsumerEntry *ChannelEntry::find_consumer(const std::string &role_uid) const noexcept;
const ProducerEntry *ChannelEntry::first_producer() const noexcept;
std::optional<std::string>
                     ChannelEntry::producer_zmq_pubkey(std::string_view uid) const;
std::optional<std::string>
                     ChannelEntry::producer_zmq_node_endpoint(std::string_view uid) const;
const nlohmann::json *ChannelEntry::producer_metadata(std::string_view uid) const noexcept;
ChannelObservable    ChannelEntry::observe(const RolePresence *p) const noexcept;
bool                 ChannelEntry::is_shm() const noexcept;
std::size_t          ChannelEntry::producer_count() const noexcept;
std::size_t          ChannelEntry::consumer_count() const noexcept;

// RoleEntry per-role queries
const RolePresence  *RoleEntry::find_presence(const std::string &channel,
                                              const std::string &role_type) const noexcept;
bool                 RoleEntry::any_presence_alive() const noexcept;

// Cross-record derived state (templated on RolesMap for snapshot/live duality)
template <typename RolesMap>
ChannelObservable compute_channel_observable(const ChannelEntry &ch,
                                              const RolesMap &roles) noexcept;
ChannelObservable observe_channel(const ChannelEntry &ch,
                                   const HubStateSnapshot &snap) noexcept;
```

**New primitives this HEP adds** (gaps surfaced by the survey;
patterns documented in §6).  Visitor callback signatures are
spelled out per declaration so an implementer can code from this
without guessing.

```cpp
namespace pylabhub::hub
{
// — Per-channel kLive producer enumeration (replaces 7+ inline joins).
template <typename RolesMap>
std::vector<const ProducerEntry *>
enumerate_live_producers(const ChannelEntry &ch, const RolesMap &roles);

// — Per-channel visitor over (producer, its presence).
//   Visitor signature: void(const ProducerEntry &, const RolePresence *)
//   The presence pointer is null when the producer has no matching
//   `find_presence(channel, "producer")` row in `roles`; this is a
//   legitimate state during late presence drop and should NOT be
//   treated as an error.  Visitor is INVOKED FOR EACH producer in
//   declaration order (vector index 0..N-1).
template <typename RolesMap, typename Fn>
void for_each_producer_with_presence(const ChannelEntry &ch,
                                      const RolesMap &roles, Fn fn);

// — Per-channel visitor over (consumer, its presence).  Symmetric
//   to producer side; same visitor signature with ConsumerEntry.
//   Visitor signature: void(const ConsumerEntry &, const RolePresence *)
template <typename RolesMap, typename Fn>
void for_each_consumer_with_presence(const ChannelEntry &ch,
                                      const RolesMap &roles, Fn fn);

// — Visitor over channel members with non-empty zmq_identity.
//   Replaces the 5+ NOTIFY fan-out sites.
//   Visitor signature: void(std::string_view zmq_identity,
//                            std::string_view role_uid)
//   `which == PartyKind::Producer` walks `ch.producers[]`; Consumer
//   walks `ch.consumers[]`.  Members with empty `zmq_identity` are
//   SKIPPED (the canonical fan-out guard).
enum class PartyKind { Producer, Consumer };
template <typename Fn>
void for_each_party_identity(const ChannelEntry &ch,
                              PartyKind which, Fn fn);

// — Find a role's attachments across the hub.  A role may appear
//   as producer on one channel AND consumer on another (multi-
//   presence semantics, HEP-CORE-0023 §2.1 + HEP-0033 §19); the
//   helper returns ALL attachments, not just the first.  Callers
//   that want first-match-wins should index `[0]` after a non-empty
//   check.
struct RoleAttachment
{
    std::string           channel;
    std::string           role_type;     // "producer" | "consumer"
    const ProducerEntry  *producer{nullptr};  // exactly one of these
    const ConsumerEntry  *consumer{nullptr};  // is non-null
};
std::vector<RoleAttachment>
find_role_attachments(const HubStateSnapshot &snap,
                       const std::string &uid);

// — Producer presence predicate (replaces 5+ inline state checks).
//   Returns true iff the producer has a matching presence row whose
//   state is Connected AND first_heartbeat_seen.  False otherwise
//   (missing role, missing presence, or any sub-Live state).
template <typename RolesMap>
bool is_producer_live(const ChannelEntry &ch,
                       const std::string &role_uid,
                       const RolesMap &roles);

// — Sweep visitor for the heartbeat-timeout scan
//   (collapses the 4-loop 190-line block in check_heartbeat_timeouts).
//
//   TWO-PHASE PATTERN — load-bearing:
//     1. Visit (read-only) — `for_each_presence_matching` invokes the
//        visitor with a COPIED `RolePresence` value plus pointers
//        into channel/party rows.  The visitor MAY NOT mutate hub
//        state; it MAY record decisions in a caller-owned container.
//     2. Apply (under writer lock) — caller drains the decision
//        container and invokes the appropriate `HubState::_on_*`
//        mutator under the writer lock.
//   This boundary is the reason the sweep visitor is safely usable
//   over a snapshot (no live-state aliasing) AND over live state
//   (the visit holds shared_lock; mutation requires upgrading).
struct PresenceSweepTarget
{
    PartyKind             party;
    const ProducerEntry  *producer{nullptr};
    const ConsumerEntry  *consumer{nullptr};
    RolePresence          presence;        // copied; read-only contract
    std::string           channel;
};
//   Predicate signature: bool(const RolePresence &)
//   Visitor signature:   void(const PresenceSweepTarget &)
template <typename RolesMap, typename Predicate, typename Fn>
void for_each_presence_matching(const ChannelEntry &ch,
                                 const RolesMap &roles,
                                 Predicate pred, Fn fn);

// — uid-list extractors (replaces 3 inline collectors).
std::vector<std::string> producer_uids(const ChannelEntry &ch);
std::vector<std::string> consumer_uids(const ChannelEntry &ch);
}
```

All new primitives are **header-only** (`hub_state.hpp` or a sibling
`hub_state_queries.hpp`) and **templated on `RolesMap`** so the same
implementation serves the snapshot path and any internal caller that
holds the live writer lock.  The template strategy is already proven
by `compute_channel_observable`.

**`RolesMap` template contract.**  The `RolesMap` type must satisfy:

- It has a `.find(const std::string &)` (or `string_view`-compatible)
  member returning an iterator-like type `it`.
- `it != roles.end()` is a valid expression.
- `it->second` returns something of `RoleEntry`-shape, specifically
  with a `.find_presence(const std::string &channel,
                          const std::string &role_type) const noexcept
                          -> const RolePresence *` method.

This is duck-typed today (no C++20 `concept`).  The two concrete
satisfiers are `HubStateSnapshot::roles` and the live
`HubState::Impl::roles`.  If a future divergent caller appears,
promote the contract to a `concept RolesMapLike` at that time; the
duck-typed shape is the canonical contract until then.

#### 2b. Script-facing query primitives (`nlohmann::json`)

Used by the script API. Each returns a `nlohmann::json` value
suitable for direct exposure to Python / Lua via the
`HubSnapshot` wrapper (Layer 3).

```cpp
namespace pylabhub::hub
{
nlohmann::json list_channels(const HubStateSnapshot &snap);
nlohmann::json get_channel  (const HubStateSnapshot &snap, const std::string &name);
nlohmann::json list_roles   (const HubStateSnapshot &snap);
nlohmann::json get_role     (const HubStateSnapshot &snap, const std::string &uid);
nlohmann::json list_bands   (const HubStateSnapshot &snap);
nlohmann::json get_band     (const HubStateSnapshot &snap, const std::string &name);
nlohmann::json list_peers   (const HubStateSnapshot &snap);
nlohmann::json get_peer     (const HubStateSnapshot &snap, const std::string &hub_uid);
nlohmann::json list_shm_blocks(const HubStateSnapshot &snap);
nlohmann::json get_shm_block  (const HubStateSnapshot &snap, const std::string &channel);

// Cross-aspect aggregates
nlohmann::json channel_state_distribution(const HubStateSnapshot &snap);
nlohmann::json health_summary             (const HubStateSnapshot &snap);

// Metrics filter (folds existing broker logic)
nlohmann::json query_metrics_from_snapshot(const HubStateSnapshot &snap,
                                            const MetricsFilter &filter);

// Channel-list specialized queries
int                       count_channels_in_state(const HubStateSnapshot &,
                                                  const std::string &state);
std::vector<std::string>  channels_in_state(const HubStateSnapshot &,
                                            const std::string &state);
}
```

Implementation delegates to the existing serializers in
`hub_state_json.hpp` where possible, plus the Layer 2a primitives
where a join is required. Script-facing helpers are NOT
re-implementations; they are typed wrappers over what's already
there.

### Layer 3 — Script API

The script gets a snapshot object that holds the C++ snapshot via
`std::shared_ptr<const HubStateSnapshot>` (ratified — see §11 below)
and exposes Layer 2b queries as methods.  Shared ownership is the
load-bearing choice: Python and Lua bindings duplicate handles
freely, and a snapshot may be held across multiple script ticks.

```cpp
namespace pylabhub::hub_host
{
class HubSnapshot
{
public:
    explicit HubSnapshot(std::shared_ptr<const HubStateSnapshot> snap);

    // ── Layer 2b queries (one method per free function in §3.2b) ──
    // Each method delegates to the corresponding Layer 2b free
    // function, passing the wrapped snapshot.  No re-snapshotting;
    // no locks.
    nlohmann::json list_channels()                                const;
    nlohmann::json get_channel(const std::string &name)           const;
    nlohmann::json list_roles()                                   const;
    nlohmann::json get_role(const std::string &uid)               const;
    nlohmann::json list_bands()                                   const;
    nlohmann::json get_band(const std::string &name)              const;
    nlohmann::json list_peers()                                   const;
    nlohmann::json get_peer(const std::string &hub_uid)           const;
    nlohmann::json list_shm_blocks()                              const;
    nlohmann::json get_shm_block(const std::string &channel)      const;
    nlohmann::json channel_state_distribution()                   const;
    nlohmann::json health_summary()                               const;
    nlohmann::json query_metrics(const MetricsFilter &filter)     const;
    int            count_channels_in_state(const std::string &)   const;
    std::vector<std::string>
                   channels_in_state(const std::string &state)    const;

    // ── Metadata accessors (snapshot-intrinsic; not delegated) ────
    double         age_seconds()                          const;  // steady-clock
    std::string    captured_at_iso()                      const;
    double         captured_epoch()                       const;
    const std::string &hub_uid()                          const noexcept;
    std::uint64_t   seq()                                 const noexcept;
};
}
```

Bindings (sketch):

```cpp
// Python (pybind11)
py::class_<HubSnapshot>(m, "HubSnapshot")
    .def("list_channels",          &HubSnapshot::list_channels)
    .def("get_channel",            &HubSnapshot::get_channel,
                                    py::arg("name"))
    .def("count_in_state",         &HubSnapshot::count_channels_in_state,
                                    py::arg("state"))
    // ... etc.
    .def_property_readonly("captured_at",   &HubSnapshot::captured_at_iso)
    .def_property_readonly("captured_epoch",&HubSnapshot::captured_epoch)
    .def_property_readonly("hub_uid",       &HubSnapshot::hub_uid)
    .def_property_readonly("seq",           &HubSnapshot::seq);

// Lua: userdata + metatable with the same method set.
```

Script usage:

```python
def on_periodic_check(api):
    snap = api.snapshot()
    if snap.health_summary()["channels"]["live"] < expected_live:
        api.log("warn",
                f"only {snap.count_in_state('live')} live "
                f"(snapshot age {snap.age_seconds():.1f}s, "
                f"hub_uid={snap.hub_uid})")
```

### Existing HubAPI `list_*` methods

They stay, refactored as one-line wrappers:

```cpp
nlohmann::json HubAPI::list_channels() const
{
    if (!impl_->host) return nlohmann::json::array();
    return pylabhub::hub::list_channels(impl_->host->state().snapshot());
}
```

This preserves backwards compatibility — scripts using the
single-aspect pattern see no change. Internally they still do one
snapshot each, same as today. The **new** primitive is
`HubAPI::snapshot()`.

## 4. Conventions (codified)

The survey found drift in three dimensions. This section settles it.

### 4.1 Return type by query shape

| Query shape | Return type |
|---|---|
| Single-key top-level lookup (e.g. `HubState::channel(name)`) | `std::optional<T>` (value copy, lock-safe across boundary) |
| Per-entity walk inside a held parent (e.g. `ChannelEntry::find_producer(uid)`) | `const T *` (raw pointer; null on miss; caller assumed to hold parent) |
| Multi-key derived value (e.g. `compute_channel_observable(ch, roles)`) | Plain value (`ChannelObservable`, `bool`, etc.) |
| Multi-key derived collection (e.g. `enumerate_live_producers`) | `std::vector<const T *>` — pointers into the snapshot or live state held by the caller |
| Visitor (e.g. `for_each_producer_with_presence`) | `void`; visitor receives pointers / references |
| Script-facing (Layer 2b) | `nlohmann::json` |

### 4.2 String parameter types

All new query helpers use **`std::string_view`** for read-only
string parameters.  Some existing helpers already use `string_view`
(`ChannelEntry::producer_zmq_pubkey`, `producer_zmq_node_endpoint`,
`producer_metadata`) — these are correctly classified as already
following the convention.  Other existing helpers in the same set
take `const std::string &` (`find_producer`, `find_consumer`,
`find_presence`); these are not migrated in this HEP but the
convention for new code is `string_view`.

### 4.3 `noexcept` discipline

- Single-key lookups returning `std::optional<T>`: NOT `noexcept`
  (they take the shared_lock; the lock could theoretically throw).
- Per-entity walks returning raw pointer: `noexcept`. They are
  pure function-over-vector scans with no allocations.
- Derived-state functions over a snapshot: `noexcept` when they
  do not allocate; otherwise unmarked.

### 4.4 `const`-correctness

All read-side primitives provide a `const` overload. Where mutators
share a pattern, a non-`const` overload exists alongside (see
`ChannelEntry::find_producer` — both `const T *` and `T *` are
provided today).

### 4.5 Naming

| Operation | Verb |
|---|---|
| Single-key lookup that may miss | `find_*` (returns `optional` or raw pointer) |
| Single-key lookup that asserts presence (for internal use) | `at_*` (rare; prefer `find_*` + check) |
| Multi-key derived state | `compute_*` (`compute_channel_observable`); `observe_*` (`observe_channel`) — both pre-existing forms are accepted aliases |
| Collection accessor | `enumerate_*` (returns vector) or `for_each_*` (visitor) |
| Predicate | `is_*` (`is_producer_live`, `is_shm`) or `contains_*` / `contains` (`PeerAllowlist::contains` is a pre-existing accepted alias for set-membership predicates) |
| Count | `*_count` (`producer_count`) or `count_*` (`count_channels_in_state`) — both pre-existing forms are accepted |
| Extract a list of identifiers | `*_uids` (`producer_uids`, `consumer_uids`) |

### 4.6 Templating on `RolesMap`

Any helper that joins across the channels-map and the roles-map
templates on the roles map type so it works for both:

- `HubStateSnapshot::roles` (the value-typed snapshot)
- `pylabhub::hub::detail::HubState::Impl::roles` (the live map under
  the writer lock — internal callers only)

`compute_channel_observable` already demonstrates this pattern.
New helpers follow it.

## 5. Inventory: what's already built (do not duplicate)

This section is a quick reference. Full file:line citations live in
the migration TODO.

### 5.1 Snapshot / aggregate types

| Type | Where | Role |
|---|---|---|
| `HubStateSnapshot` | `hub_state.hpp` | THE point-in-time aggregate. Returned by `HubState::snapshot()`. |
| `QueueMetrics`, `InboxMetricsSnapshot`, `LoopMetricsSnapshot` | queue/role-side | Telemetry; unrelated to HubState — out of scope. |
| `ContextMetrics` | per-DataBlock atomic counters | Atomic state, not a snapshot type. Out of scope. |

Marked for retirement:

| Type | Where | Reason |
|---|---|---|
| `ChannelSnapshotEntry` / `ChannelSnapshot` | `broker_service.hpp` | Lossy projection of `ChannelEntry`; only used by L3 worker tests. Layer 2b returning `nlohmann::json` covers the test case. |
| `RoleStateMetrics` | `broker_service.hpp` | Strict subset of `BrokerCounters`; exists only as a test-stable shape. |

### 5.2 Existing query methods (canonical — use these, do not reinvent)

All listed in §3.2a as already part of the framework. The complete
method-by-method inventory with file:line is in
`docs/todo/QUERY_LAYER_TODO.md`.

### 5.3 Existing JSON serializers (out of scope; do not replace)

`hub_state_json.hpp` provides `channel_to_json`, `role_to_json`,
`band_to_json`, `peer_to_json`, `broker_counters_to_json`. Layer 2b
helpers delegate to these where the JSON shape matches.

### 5.4 NOTIFY wire primitive

`BrokerServiceImpl::send_to_identity(socket, zmq_identity, type, body)`
is the canonical broker-to-role wire primitive. Already used by 8+
NOTIFY paths; Phase D's `CHANNEL_AUTH_UPDATE` snapshot push uses the
same primitive. Out of scope for this HEP except as a consumer of
`enumerate_live_producers` / `for_each_party_identity`.

## 6. Inline-join patterns (and the canonical helper for each)

The patterns the survey found, with the canonical helper that
replaces them. Each pattern lists the operation, not the call sites
(sites live in the migration TODO).

| Pattern | Canonical helper |
|---|---|
| Walk `ch.producers[]` → `roles.find(uid)` → `find_presence(channel, "producer")` → reduce | `compute_channel_observable` (for the observable reduction); `for_each_producer_with_presence(ch, roles, fn)` (for arbitrary reductions) |
| Walk `ch.consumers[]` → `roles.find(uid)` → `find_presence(channel, "consumer")` → reduce | `for_each_consumer_with_presence` (new — no canonical today) |
| Find role across the hub by uid → `(channel, role_type)` | `find_role_attachments(snap, uid)` (new) |
| Filter `ch.consumers` by `c.role_uid == uid` | `ChannelEntry::find_consumer(uid)` — already exists; some sites duplicate it |
| Walk parties with non-empty `zmq_identity` → send NOTIFY | `for_each_party_identity(ch, kind, fn)` (new) |
| Collect `role_uid`s from `producers[] / consumers[]` into vector | `producer_uids(ch) / consumer_uids(ch)` (new) |
| "Is producer P kLive on channel X?" inline state check | `is_producer_live(ch, role_uid, roles)` (new) |
| Heartbeat-timeout nested sweep `(channel × party × presence)` filtered by state + age — **two passes** required (see note below) | `for_each_presence_matching(ch, roles, pred, fn)` (new) |
| Mutator-side joins (e.g. `roles.find(uid)` + `on_dereg` + `drop_channel_if_orphaned`; `ChannelStatusChangedHandler` refire preamble) | OUT OF SCOPE (mutators; governed by HEP-0033 and the Core Structure Change Protocol). Catalogued in the migration TODO Pattern P9 for cross-reference only. |

**Two-passes-with-cross-pass-dependency note.**  When a sweep's
Pass-2 predicate depends on state mutated by Pass-1 (the canonical
example is the heartbeat-timeout sweep: Pass-1 stamps a fresh
`state_since` on Connected→Pending; Pass-2 must observe that fresh
stamp to EXCLUDE just-demoted presences from same-sweep
termination), the migration MUST use two snapshots + two invocations
of `for_each_presence_matching`:

1. Take Snapshot-1; invoke `for_each_presence_matching` with the
   Pass-1 predicate; collect Pass-1 decisions.
2. Drain Pass-1 decisions via the Pass-1 mutator.
3. Take Snapshot-2 — fresh, reflecting Pass-1's mutations.
4. Invoke `for_each_presence_matching` with the Pass-2 predicate
   over Snapshot-2; collect Pass-2 decisions.
5. Drain Pass-2 decisions via the Pass-2 mutator + any
   broker-side fan-out (NOTIFY emission, etc.) keyed off a
   `pre_drop` snapshot captured BEFORE the mutator runs.

A single helper call with a combined Pass-1-or-Pass-2 predicate
over one snapshot is **wrong**: it would consider just-demoted
presences for termination in the same tick.  See
`docs/todo/QUERY_LAYER_TODO.md` Pattern P8 for the
heartbeat-timeout migration shape.

## 7. Phase D coupling

The PeerAdmission Phase D broker glue (task #126, blocked) requires
the following from this framework. All needs are either already
satisfied or land as part of the new helpers in §3.2a.

| Phase D need | Status under this HEP |
|---|---|
| Channel allowlist storage (`ChannelAccessEntry`) | OUT OF SCOPE (storage decision M-D1; resolved separately) |
| Enumerate kLive producers of a channel | `enumerate_live_producers(ch, snap.roles)` |
| Reach producer's `tx_queue` from broker | Already exists — `send_to_identity(prod.zmq_identity, ...)` over CTRL |
| Serialize allowlist to wire JSON | OUT OF SCOPE (wire serializer; lives in `hub_state_json.hpp` or a new file under it) |
| Role-side dispatch CHANNEL_AUTH_UPDATE → tx_queue | OUT OF SCOPE (role-side wiring; tracked under task #103) |
| Broker per-producer ACK-wait state machine | OUT OF SCOPE (broker-side request-correlation pattern; net new) |
| Consumer-revoke sweep across channels | `find_role_attachments` + `for_each_party_identity` |
| REG_ACK.initial_allowlist on producer reconnect | OUT OF SCOPE (HEP-0021 schema work; tracked under task #104) |

Phase D explicitly DOES NOT block on this HEP; the framework merely
gives Phase D a clean foundation. If this HEP lands first, Phase D
code stays small. If Phase D lands first, this HEP's migration
retroactively cleans up Phase D code along with the rest.

## 8. Files / modules affected

### 8.1 New files

| Path | Purpose |
|---|---|
| `src/include/utils/hub_state_queries.hpp` | Layer 2a + 2b free-function declarations |
| `src/utils/ipc/hub_state_queries.cpp` | Layer 2a + 2b implementations |
| `src/include/utils/hub_snapshot.hpp` | `HubSnapshot` wrapper class (shared_ptr-based, script-bindable) |
| `src/utils/service/hub_snapshot.cpp` | Wrapper class methods (forwards to Layer 2b) |
| `tests/test_layer2_hub_state_queries/` | L2 unit tests for every Layer 2 helper |
| `tests/test_layer2_hub_snapshot_python/` | Script-binding L2 tests (Python) |
| `tests/test_layer2_hub_snapshot_lua/` | Script-binding L2 tests (Lua) |

### 8.2 Modified files

| Path | Change |
|---|---|
| `src/include/utils/hub_state.hpp` | Add `captured_mono`, `hub_uid`, `snapshot_seq` to `HubStateSnapshot` |
| `src/utils/ipc/hub_state.cpp` | Populate new metadata in `HubState::snapshot()`; add `std::atomic<uint64_t> snapshot_seq_counter_` to Impl |
| `src/include/utils/hub_api.hpp` | Add `[[nodiscard]] std::shared_ptr<HubSnapshot> snapshot() const;` |
| `src/utils/service/hub_api.cpp` | Implement `HubAPI::snapshot()`; refactor `list_*` / `get_*` to one-line Layer 2b wrappers |
| `src/scripting/hub_api_python.cpp` | Bind `HubSnapshot` class |
| `src/scripting/lua_engine.cpp` | Add `HubSnapshot` userdata + metatable |
| `src/utils/ipc/broker_service.cpp` | Migration sites — done per the QUERY_LAYER_TODO list, NOT bundled into this HEP's code phase |
| `src/utils/ipc/hub_state.cpp` | Same — migration sites listed in TODO |

### 8.3 Data structures changed

| Type | Change |
|---|---|
| `HubStateSnapshot` | Add `captured_mono`, `hub_uid`, `snapshot_seq` |
| `HubState::Impl` | Add `std::atomic<std::uint64_t> snapshot_seq_counter_{0}`.  Increment-then-assign in `snapshot()` so the FIRST live snapshot has `seq == 1`.  `seq == 0` is reserved for default-constructed `HubStateSnapshot` (uninitialized; never returned from `HubState::snapshot()`). |
| `HubSnapshot` (new) | Script-binding wrapper holding `std::shared_ptr<const HubStateSnapshot>` |

## 9. Migration plan

The framework lands in phases. Migration of existing inline-join
sites is a separate task per pattern; see
`docs/todo/QUERY_LAYER_TODO.md` for the picking order.

**Phase A — Framework code (no behavior change)**

1. Add `hub_state_queries.hpp` + `.cpp` with all Layer 2a + 2b
   declarations and implementations of NEW primitives.
2. Add the 3 metadata fields to `HubStateSnapshot`.
3. Populate them in `HubState::snapshot()`.
4. New L2 unit tests for every new primitive.
5. No HubAPI changes yet. Existing code unaffected.
6. Validate: full test suite passes.

**Phase B — Refactor HubAPI to delegate**

7. Refactor existing `HubAPI::list_*` / `get_*` to one-line Layer 2b
   wrappers.
8. Validate: every existing test still passes. Behavior identical.

**Phase C — Script-side `snapshot()`**

9. Add `HubSnapshot` wrapper class.
10. Add `HubAPI::snapshot()`.
11. Python binding.
12. Lua binding.
13. New L2 binding tests including the coherence test (§10).
14. Validate.

**Phase D — Apply to the densest existing duplication site**

15. Migrate the `check_heartbeat_timeouts` 4-loop sweep
    (`broker_service.cpp` lines ~2879-3068) to
    `for_each_presence_matching`. This is the proof-of-use.
16. Code review specifically for the migration: verify no behavior
    change.

**Phase E — Catalogued migration sweep**

17. Drive the remaining sites from `docs/todo/QUERY_LAYER_TODO.md`,
    one pattern at a time. Each pattern is its own commit + review.
    Order suggested in the TODO (highest-duplication-first).
18. Mark retired types (`ChannelSnapshotEntry`, `RoleStateMetrics`)
    with `[[deprecated]]` once no in-tree caller remains; remove in
    a follow-on cleanup.

Phases A–D land in one work-stream. Phase E is open-ended cleanup
that can happen incrementally over many sessions.

## 10. Test strategy

### 10.1 Unit tests for every new Layer 2 primitive

Each new helper gets a dedicated L2 test file pinning its semantics:

- Empty inputs (empty channel, empty roles map).
- Single-element inputs.
- Multi-element inputs covering all branches.
- Edge cases (kAbsent, kRegistering, kStalled, kLive).

### 10.2 The coherence test (load-bearing for the whole framework)

Implemented as an L2 test that uses the same broker-worker pattern
as the existing `test_layer2_hub_state_*` family.  A worker drives a
REG_REQ for a fresh role to perform the mutation; the test captures
a snapshot BEFORE issuing the REG_REQ and verifies the snapshot does
not reflect the post-mutation state.

```cpp
TEST(HubSnapshot, CoherentMultiAspectView)
{
    auto snap = hub.state().snapshot();         // capture
    drive_consumer_reg_req(hub, channel, new_role_uid);  // L2 worker
    auto channels = pylabhub::hub::list_channels(snap);
    auto roles    = pylabhub::hub::list_roles(snap);
    EXPECT_FALSE(role_appears_in(roles, new_role_uid));
    EXPECT_FALSE(role_appears_in_any_channel(channels, new_role_uid));
}
```

If this test ever fails, the capture/query split is broken — that
is the framework's core invariant.

### 10.3 Convention guard

A `clang-tidy` check (or simpler grep guard in CI) verifies new code
does not introduce inline `roles.find(uid)` + `find_presence(...)`
patterns. Decision deferred to Phase A implementation — the
mechanism doesn't affect the design.

### 10.4 Migration regression bar

For every migration commit in Phase E, the diff must be behavior-
preserving (same JSON output for serializer paths, same observable
effects for sweep paths). Tests already covering these surfaces
must remain green; a migration commit that breaks an existing test
is suspect.

## 11. Decisions ratified + open implementation choices

### 11.1 Ratified (with the HEP, 2026-06-02)

These are settled and bind Phase A code:

- **`HubSnapshot` ownership model: `std::shared_ptr<const HubStateSnapshot>`.**
  Multiple scripts can hold the same snapshot cheaply; Python/Lua
  bindings duplicate handles freely; lifetime is implicit (last
  holder releases).  Alternatives (move-only `unique_ptr`, value
  type) were considered and rejected — the shared-ownership model
  matches how scripts use the snapshot in practice.
- **`HubAPI::snapshot()` returns a fresh snapshot on every call.**
  No caching, no TTL.  Scripts that want to reuse can hold the
  snapshot object across ticks; the framework does not impose a
  policy.

### 11.2 Open implementation choices (decide during Phase A)

These are tactical and do not affect the framework shape:

1. **`captured_at` script representation.** ISO 8601 string AND
   Unix epoch float (both exposed as separate accessors) recommended.
2. **What does `snapshot()` return for an uninitialized hub?**
   Empty containers + `captured_at = now()` + `hub_uid = ""` — script
   callers test `if not snap.hub_uid` to detect.
3. **`ChannelSnapshotEntry` retirement timing.** Mark deprecated in
   Phase A; remove in a follow-on cleanup after all L3 worker tests
   migrate.  Don't force the removal in the same commit.
4. **Convention-guard mechanism.** clang-tidy custom check vs CI
   grep vs nothing-just-document.  Decision deferred to Phase A;
   does not block any other phase.
5. **Lua userdata GC discipline.** `lua_newuserdata` + metatable
   with `__gc` releasing the `shared_ptr`.  Standard pattern; flagged
   so the binding implementer doesn't skip it.

## 12. Cross-references

| HEP | Relevance |
|---|---|
| HEP-CORE-0001 (Lifecycle) | `HubState` is constructed inside `HubHost`'s `LifecycleGuard`; queries assume `HubState` is alive |
| HEP-CORE-0019 (Metrics Plane) | `query_metrics_from_snapshot` is the snapshot-based entry point; existing query-driven model in §3-4 stays the broker-side path |
| HEP-CORE-0023 (Startup Coordination) | `ChannelObservable` semantics (`kAbsent / kRegistering / kStalled / kLive`) defined here; this HEP consumes them |
| HEP-CORE-0033 (Hub Character) | Owns HubState lifecycle + HubAPI surface; this HEP amends §12.3 to record `HubAPI::snapshot()` as the primary read primitive |
| HEP-CORE-0034 (Schema Registry) | Schemas appear in `HubStateSnapshot::schemas` per the existing structure |
| HEP-CORE-0036 (Authenticated Connection Establishment) | Phase D consumes the framework; coupling listed in §7 |

## 13. Implementation status

**Not yet implemented.** This HEP is design-only. For current plan
and priorities, see `docs/TODO_MASTER.md`. The framework lands in
phases A–D as a single work stream; Phase E (catalogued migration
sweep) is open-ended.

Pre-existing facilities that already implement parts of the
framework (see §5.2) remain in use; this HEP ratifies them as the
canonical primitives going forward.
