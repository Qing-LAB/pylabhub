# Schema & Metrics Query Integration — completing the read paths for a schema-driven ecosystem

| | |
|---|---|
| **Status** | DESIGN DRAFT — 2026-07-26.  Scenario-driven review of integrating the two orphaned Class-C queries (`SCHEMA_REQ`, `METRICS_REQ`) into the role-facing API.  Resolves task #95 (KEEP-RESERVED/DELETE → **KEEP + INTEGRATE**).  Decisions marked ⚖ need user ruling before code. |
| **Origin** | 2026-07-26 wire-inventory audit: both messages have complete, well-formed broker handlers and dispatch rows, but no client half anywhere — no `BrokerRequestComm` method, no role-API accessor, no engine binding.  User direction: these are built mechanisms (schema communication, metrics reporting); think from application scenarios whether the design gives a clean, logical, COMPLETE framework, and detect conflicts + gaps. |
| **Authorities this composes with** | HEP-0034 (schema registry: write/citation paths, owner-lifetime), HEP-0017 §4.7.0.1/§4.7.0.3 (owner-first + channel-lifecycle machine), HEP-0019 (metrics via heartbeat), HEP-0007 §12 (wire catalog), HEP-0028 (script API, 3-engine parity), HEP-0033 §18.2 (Class-C read-only queries), HEP-0046 (typed-body tiers). |

---

## 1. What exists today (verified in code 2026-07-26)

**The schema registry has three of its four paths.**  Write: producers
self-register schemas at REG (Path B) or adopt (Path A); hub-globals load
at startup.  Cite: consumers assert `expected_*` at CONSUMER_REG (named /
anonymous / absent modes).  Evict: owner-lifetime cascade (HEP-0034
§7.2).  **Read over the wire: `handle_schema_req` serves it — and nothing
calls it.**  Two request forms, one handler:

- `(owner, schema_id)` → the full `SchemaRecord`: both zones' BLDS +
  packing + 128-hex fingerprint.  The registry key form.
- `channel_name` → the channel's schema invariants (id, owner, blds,
  flexzone_blds, hash).  The only wire path that returns **full BLDS by
  channel** — `DISC_ACK` deliberately carries only the summary
  (id/hash), so this form is complementary to discovery, not redundant.

**The metrics plane has push but no role-facing pull.**  Roles push
metrics on `HEARTBEAT_NOTIFY` (HEP-0019 §2.3); the broker stores them on
per-presence rows.  Readers today: the operator (admin plane, separate
auth) and the hub script (`query_metrics`, in-process).  Roles — the
parties that produce the data — cannot read any of it back.
`handle_metrics_req` serves per-channel or all-channels snapshots plus
live SHM block info — and nothing calls it.

**The asymmetry is an unfinished pattern, not a design.**  Of the
HEP-0033 §18.2 Class-C read-only queries, `CHANNEL_LIST_REQ`,
`ROLE_PRESENCE_REQ`, `ROLE_INFO_REQ`, `SHM_BLOCK_QUERY_REQ` all have BRC
client methods; these two never got theirs.

---

## 2. Scenario walk-throughs (the design test)

Each scenario is walked against the CURRENT framework; ✅ = works with
only the missing client plumbing; ⚠ = works with a caveat to document;
❌ = structural gap.

### S-A  Generic archiver / logger / bridge (schema-driven consumer)

One role binary + one script that attaches to ANY channel named in its
config and processes every slot by interpreting the channel's BLDS at
runtime.  No per-channel schema in its config.

Startup walk-through against the owner-first machine:

1. Config declares channel name, transport, topology (topology stays a
   config fact — a generic archiver on fan-in *is* the owner and must
   declare; on fan-out/1:1 it is a dialer and may omit).  ✅
2. Register as consumer with **absent citation** (legal today —
   citation modes are named / anonymous / absent).  Early arrival is
   handled by the `AWAITING_OWNER` retry; no schema knowledge needed
   yet.  ✅ — and note the pleasing consequence: **registration-first
   ordering makes the owner-first machine do the waiting**, so a
   schema pull placed *after* REG_ACK can never race a missing channel.
3. Pull the schema: `api`-level `get_channel_schema(channel)` →
   `SCHEMA_REQ(channel_name)` → full BLDS.  Channel exists (we are
   registered on it), so the query answers from Open state.  ✅ (needs
   the client plumbing — the point of this draft.)
4. **Build the slot interpreter from the pulled BLDS.**  ❌ **G1 + G4 —
   the two real structural gaps, below.**  The rx queue is built at
   role-host step S1 *from config* `SchemaSpec`, BEFORE the BRC exists;
   and the script engines' slot proxies are compiled from the config
   spec, not from a runtime BLDS.

### S-B  Pre-flight citation (config-light but verifying consumer)

A consumer that wants named-citation safety without duplicating the
schema in its config: register uncited → pull schema → *verify* against
an expected fingerprint pin (a single hash string in config, not the
whole BLDS) → proceed or abort.  ✅ — works with pull-after-REG; the
64-byte fingerprint (HEP-0034 §6.3) is exactly the right thing to pin
in config.  The full-citation-at-REG flow stays available for
config-heavy roles; this adds a middle rung.  (A "cite after REG"
protocol change is NOT needed and NOT proposed — post-hoc verification
against the fingerprint achieves the same integrity without a new wire
state.)

### S-C  Processor deriving output schema from input

At startup: pull input channel's schema, compute output schema, register
producer side with it.  ⚠ — same G1 sequencing dependency as S-A (the
OUT queue and REG payload need the derived schema), plus processor
startup builds both queues before either REG today.  Covered by the same
G1 resolution.

### S-D  Schema lifetime under the channel-lifecycle machine

Does a pulled schema go stale?  Walk the machine:

- **Fan-out / 1:1**: schema-carrying owner (producer) dies → owner
  death is channel death (§4.7.0.3) → every member gets
  `CHANNEL_CLOSING_NOTIFY` → re-establish → re-pull.  **Pull-at-
  establishment is automatically fresh; no schema-changed notify is
  needed.**  ✅ — the owner-first machine makes schema staleness
  structurally impossible on producer-owned topologies.
- **Fan-in**: channel schema is filed by the first producer (Path A/B)
  but the channel is CONSUMER-owned.  Two distinct lifetimes exist and
  must be documented, not "fixed": the **registry record** lives with
  its owner ROLE (HEP-0034 §7.2 eviction), while the **channel's copy**
  of the invariants lives with the CHANNEL.  A generic consumer that
  pulled by `channel_name` holds invariants that remain valid for the
  channel's whole life (schema is immutable on a live channel —
  SCHEMA_MISMATCH guards it).  A tool that pulled by `(owner, id)` may
  see the record evicted while the channel lives.  ⚠ **Rule to write
  into HEP-0034: schema-driven CONSUMERS use the channel form; the
  `(owner, id)` form is registry tooling.**

### S-E  Adaptive producer (metrics-driven throttling)

Producer script polls `api.get_channel_metrics(my_channel)` every N
cycles; reads its consumer's `iteration_count`; throttles.  ✅ — data
already flows in via consumer heartbeats; freshness = heartbeat cadence
(document); the pull is a cheap ctrl-plane round trip paced by the
script.  Mechanism-not-policy: we expose numbers, the script decides.
Complements #74 cleanly: `CHANNEL_COUNT_NOTIFY` pushes cheap liveness
cardinality to everyone; metrics are the expensive payloads pulled on
demand.

### S-F  Standalone monitoring / exporter role

A role whose only job is reading hub-wide metrics and exporting them
(Prometheus bridge, live dashboard) — on the ROLE plane (CURVE
known-role), not the operator plane.  ❌ **G2: the framework has no
control-plane-only role kind.**  Every role host (producer / consumer /
processor) fatals unless its data-channel establishment succeeds; a
"bare" role with a BRC and no channel doesn't exist.  This is
role-binary-unification territory (#292), not this draft's scope — but
this draft should not pretend S-F works.  v1 scope: metrics pull serves
**participant roles** (S-E); hub script + admin plane keep serving
global monitoring; the observer role kind is named as a #292
requirement.

---

## 3. Gap register

| # | Gap / conflict | Severity | Resolution options | Recommendation |
|---|---|---|---|---|
| **G1** | **Startup sequencing: queues are built (S1) from config `SchemaSpec` BEFORE the BRC exists; a registry-driven schema is only pullable after control-plane connect.**  Affects S-A/S-C. | Structural | (a) Reorder role-host startup for registry-driven roles: BRC first → REG (AWAITING_OWNER retry does the waiting) → pull schema → build queues → activate.  (b) Late-bind: build queue in schema-pending Standby at S1, supply schema at S3 activation (`apply_master_approval` already applies ACK-derived state there).  (c) Carry full BLDS on `CONSUMER_REG_ACK` (wire change; makes REG_ACK heavy for every consumer to serve the generic few). | **(b)** — it extends the queue's existing Standby→Configured→Active machine (schema becomes part of "Configured", symmetric with endpoints/allowlist which are ALREADY late-bound from the ACK), needs no wire change and no reordering.  (a) is the fallback if queue internals resist late schema binding.  Reject (c): pays on every registration for a niche need, and REG_ACK is already the heaviest reply. ⚖ |
| **G4** | **Script engines compile slot proxies from config `SchemaSpec`; no engine can build slot accessors from a runtime-pulled BLDS.**  Without this, "schema-driven" stops at the C++/native tier. | Structural (largest work item) | (a) Engine support: build the slot proxy from a runtime `schema::SchemaInfo` (the C++ BLDS interpreter exists; the binding layer needs to accept it post-config).  (b) v1 punt: generic roles are native-engine only; Lua/Python get raw-bytes + a BLDS-describe API. | **(a) as its own slice**, after the plumbing slice; 3-engine parity is the project rule, and (b) would create a two-tier script ecosystem.  Sequence it last — everything else is useful without it (S-B, S-E work today with plumbing only). ⚖ |
| **G2** | No control-plane-only (observer) role kind for S-F. | Deferred | Fold into #292 role-binary unification as a named requirement ("a role kind with BRC + heartbeat + no data channel"). | Defer to #292; record there. |
| **G3** | **Neither handler checks the caller.**  `SCHEMA_REQ`/`METRICS_REQ` answer any CURVE-authenticated known role about any channel.  Compare: `GET_CHANNEL_AUTH` is binding-side-gated, `GET_CHANNEL_PRODUCERS` was consumer-gated — the project's blast-radius discipline gates reads. | Design decision | (a) Member-gate both channel-form queries (caller must hold a presence on the channel — `is_role_registered_on_channel` exists); all-channels METRICS form becomes hub-script/admin-only (wire form requires `channel_name`).  (b) Leave open to all known roles (metrics/schemas are observability/structure, not secrets — hostnames/pids/SHM names are the only mild recon surface). | **(a)** — matches least-privilege precedent, and every v1 scenario (S-A…S-E) pulls only channels the caller is registered on.  The (owner,id) SCHEMA form stays known-role-open (the registry is shared infrastructure, and hub-globals have no channel to be member of).  Loosening later for an observer role is a deliberate #292-era grant, not a default. ⚖ |
| **G5** | Freshness/lifetime semantics are implicit. | Doc | Write into the integration: metrics freshness = heartbeat cadence; schema validity = channel lifetime (S-D); pull-at-establishment pattern; fan-in dual-lifetime rule (channel form for consumers, owner/id form for tooling). | Fold into HEP-0034 §10.3 + HEP-0019 when the slice lands. |
| **G6** | No typed bodies for either message (JSON handlers). | Tracked | Already on the HEP-0046 EnvelopeOnly follow-on list; add `SchemaReqBody`/`MetricsReqBody` when giving them clients (the natural moment). | Do with slice 1. |

**Conflicts detected: none against the lifecycle machine.**  Two
near-conflicts resolve cleanly and should be documented as patterns:
(1) *pre-flight pull vs owner-first timing* — resolved by
register-first-then-pull (the AWAITING_OWNER retry absorbs all waiting;
queries never retry — they answer from state, `CHANNEL_NOT_FOUND` on a
query stays terminal per §4.7.0.3 rule 4); (2) *SCHEMA_REQ(channel) vs
DISC_REQ overlap* — complementary by design (summary vs full structure);
state this in HEP-0007 §12.3 so they don't drift toward duplication.

---

## 4. Proposed integration, sliced

**Slice 1 — client plumbing + gating + pins (small, self-contained).**
BRC: `get_schema(owner, id)` / `get_channel_schema(channel)` /
`get_channel_metrics(channel)` beside `list_channels`.  RoleAPIBase
pass-throughs (Class-C routing).  G3(a) member-gating in both handlers +
`MetricsReqBody`/`SchemaReqBody` typed bodies + dispatch-tier rows for
the two messages (they have none today — closes their legacy-path
bypass).  HEP-0007 §12.2.1 note flips from "no production caller" to the
contract; L2 handler pins (both forms, gating, error paths) + one L3
wire round-trip.  Unlocks S-B and S-E immediately.

**Slice 2 — engine bindings (3-engine parity).**  `api.get_schema` /
`api.get_channel_schema` / `api.get_channel_metrics` in Lua + Python +
Native; HEP-0028 + README_topology_channels docs.  Unlocks S-E for
scripts on all engines.

**Slice 3 — schema-pending queue activation (G1, option b).**  Queue
accepts schema at Configured; role host resolves "schema = registry"
config value via pull between REG_ACK and activation.  Unlocks S-A/S-C
for the native tier.

**Slice 4 — runtime BLDS slot proxies (G4).**  Engine-side; the last
mile to fully generic script roles.

(#292 carries G2's observer role kind.)

---

## 5. Decisions requested (⚖)

1. **G1 resolution** — late-bind schema into the queue's Configured
   stage (recommended) vs. reorder startup?
2. **G3 gating** — member-gated channel queries + known-role-open
   `(owner,id)` registry reads (recommended) vs. all-open?
3. **Slice order** — 1→2→3→4 as above, or pull slice 3 earlier if
   generic native roles are wanted sooner?
