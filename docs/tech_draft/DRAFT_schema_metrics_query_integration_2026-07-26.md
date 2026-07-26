# Schema & Metrics Query Integration — completing the read paths for a schema-driven ecosystem

| | |
|---|---|
| **Status** | DESIGN DRAFT — 2026-07-26.  Scenario-driven review of integrating the two orphaned Class-C queries (`SCHEMA_REQ`, `METRICS_REQ`) into the role-facing API.  Resolves task #95 (KEEP-RESERVED/DELETE → **KEEP + INTEGRATE**).  Decisions marked ⚖ need user ruling before code. |
| **Origin** | 2026-07-26 wire-inventory audit: both messages have complete, well-formed broker handlers and dispatch rows, but no client half anywhere — no `BrokerRequestComm` method, no role-API accessor, no engine binding.  User direction: these are built mechanisms (schema communication, metrics reporting); think from application scenarios whether the design gives a clean, logical, COMPLETE framework, and detect conflicts + gaps. |
| **Authorities this composes with** | HEP-0034 (schema registry: write/citation paths, owner-lifetime), HEP-0017 §4.7.0.1/§4.7.0.3 (owner-first + channel-lifecycle machine), HEP-0019 (metrics via heartbeat), HEP-0007 §12 (wire catalog), HEP-0028 (script API, 3-engine parity), HEP-0033 §18.2 (Class-C read-only queries), HEP-0046 (typed-body tiers). |

---

## 0. Invariants and required checks (normative — the code-checkable contract)

Everything this design asserts, in one place.  Each invariant is a
falsifiable statement with its enforcement site; the scenarios and gap
register below are INSTANCES of these rules, never additional rules.
Status: ✅ = enforced today, verified in code 2026-07-26; 🔨 = to build
(slice noted); each 🔨 row names the gap it closes.

| ID | Invariant | Enforcement site (code) | Status |
|---|---|---|---|
| **SI-1** | A channel's format is established EXACTLY ONCE, by its OWNER at channel-open (fan-out/1:1 → the producer; fan-in → the consumer — user ruling, Option B), and is immutable for the channel's lifetime. | Immutability: front-door channel-match (`handle_reg_req` early gate; consumer citation step) + admission invariant compare.  Owner-declares, broker half: fan-in open without schema material → `SCHEMA_REQUIRED` (`handle_consumer_reg_req`, L3-pinned).  Config half (`from-channel` rejected on owning sides) lands with the sentinel in slice 3. | Immutability ✅.  Broker owner-must-declare ✅ 2026-07-26 (pin: `FanInOwnerOpen_NoSchema_Rejected`).  Config half 🔨 slice 3. |
| **SI-2** | **The open row validates the contract it installs**: any registration that OPENS a channel and carries schema material must be self-consistent — structure present ⇒ hash present AND equal to the recomputed fingerprint (`verify_request_fingerprint`) — checked BEFORE the book opens.  Structure without hash is rejected.  (Hash WITHOUT structure stays legal on BOTH sides — the consumer's named-citation mode and the producer's legacy fingerprint-only registration; there is nothing to recompute, and the join gate still compares it exactly.) | Named producer path ✅ (§10.1 block).  Anonymous producer structure ✅ 2026-07-26 (G8b closed — pins: `AnonymousReg_StructureWithoutHash_Rejected`, `AnonymousReg_InconsistentFingerprint_Rejected`).  Fan-in owner citation ✅ 2026-07-26 (G8 closed — Job A now runs for openers; opener hash required; pins: `FanInOwnerOpen_InconsistentFingerprint_Rejected`, `FanInOwnerOpen_ValidCitation_ProducerJoinsByContract`). | ✅ enforced (full ctest 2676/2676 green 2026-07-26). |
| **SI-3** | Every JOIN against an existing channel is checked against the stored contract by EXACT equality on every declared axis: fingerprint always; name exactly (empty matches only empty); owner where claimed.  No adopt, no partial match, no direction exempt. | `_validate_schema_citation` steps (a)/(b)/(c); producer front-door early gate; consumer citation block. | ✅ verified (both directions, incl. blank/cited mixed cases rejecting). |
| **SI-4** | **Writers must match; readers may opt out.**  A producer joining a format-carrying channel MUST present the matching format (an empty-schema producer is rejected by SI-3's fingerprint axis).  A consumer may join with NO schema material at all (absent mode: "all expected_* empty → no validation") — it reads under the channel's existing integrity machinery regardless. | Producer: SI-3 sites.  Consumer: explicit third mode in the citation block. | ✅ verified both halves. |
| **SI-5** | Schema/metrics content leaves the broker ONLY to authenticated, validated, admitted parties: the BLDS rides the success ACK (§2b) or an identity-checked pull; the metrics snapshot answers only channel members.  No structure or telemetry to unauthenticated, unknown, or rejected callers. | ACK: `CONSUMER_REG_ACK` built only on admission success.  Pull gating: `handle_schema_req`/`handle_metrics_req` — currently identity-BLIND (signatures take body only). | ACK-side ✅ by construction; pull gating 🔨 slice 1 (G3 + signature migration). |
| **SI-6** | **The fingerprint chain must close before data flows on a runtime-resolved format**: config pin (when present) == delivered BLDS's recomputed fingerprint == (SHM) the segment header's stamped hashes.  Any link mismatch — and equally a channel that turns out to carry NO established format — is a clean startup abort naming the cause; a `from-channel` role never proceeds on an empty format. | Role-side activation (slice 3): pin check + SHM header cross-check before mapping; empty-format abort. | 🔨 slice 3 (config-schema deployments already close an equivalent chain today via citation ✅). |
| **SI-7** | `from-channel` (runtime-resolved format) is legal ONLY on DIALING sides.  An owning side declaring `from-channel` is a CONFIG ERROR caught at role startup — the owner cannot ask the channel for what only the owner can establish. | Role-host config validation at startup; broker backstop = SI-1/SI-2 rejections. | 🔨 slice 1 (falls out of Option B). |
| **SI-8** | Queries answer from machine state and never wait: `SCHEMA_REQ`/`METRICS_REQ` against Absent → terminal `CHANNEL_NOT_FOUND` (never `AWAITING_OWNER`, never a pend).  All establishment waiting lives in the registration retry (HEP-0017 §4.7.0.3 rule 4). | Both handlers return CHANNEL_NOT_FOUND on missing channel. | ✅ (by the lifecycle machine; keep pinned when handlers gain gating). |
| **MI-1** | Metrics are push-in (heartbeat), pull-out (member-gated `METRICS_REQ`); freshness = heartbeat cadence; the wire form REQUIRES `channel_name` (hub-wide aggregation stays hub-script/admin-plane, until an observer role kind exists — #292). | `handle_metrics_req` (drop the all-channels wire branch; require channel + membership). | 🔨 slice 1. |

Cross-check protocol for this table: every ✅ row cites the exact
mechanism a reviewer can grep; every 🔨 row must flip to ✅ with a code
site + an L2/L3 pin before its slice is called done.  A future reviewer
finding ANY schema/metrics decision in code that does not trace to one
of these rows has found either a new invariant to add here or a defect.

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
   config fact).  Scope note per Option B/SI-7: a generic archiver
   attaches only where it is the DIALING side — fan-out / one-to-one.
   Under fan-in the sole consumer IS the owner and owners always
   declare, so a fan-in attachment point cannot be generic.  ✅
2. Register as consumer with **absent citation** (VERIFIED legal in
   code, fresh-eyes pass 2026-07-26: the citation block's explicit
   third mode — "Empty: all expected_* empty → no validation (consumer
   signals 'I don't care about schema')", broker_service.cpp
   `handle_consumer_reg_req` citation step).  Early arrival is handled
   by the `AWAITING_OWNER` retry; no schema knowledge needed yet.  ✅ —
   and note the pleasing consequence: **registration-first ordering
   makes the owner-first machine do the waiting**, so a schema pull
   placed *after* REG_ACK can never race a missing channel.
3. Pull the schema: `api`-level `get_channel_schema(channel)` →
   `SCHEMA_REQ(channel_name)` → full BLDS.  Channel exists (we are
   registered on it), so the query answers from Open state.  ✅ (needs
   the client plumbing — the point of this draft.)
4. **Build the slot interpreter from the pulled BLDS.**  ❌ **G1 + G4 —
   the two real structural gaps, below.**  The rx queue is built at
   role-host step S1 *from config* `SchemaSpec`, BEFORE the BRC exists
   (both transports: the ZMQ factories reject an empty schema at
   build, and SHM attach needs the layout); and the script engines'
   slot proxies are compiled from the config spec, not from a runtime
   BLDS.

   SHM refinement (fresh-eyes 2026-07-26; storage locations verified
   in code and stated precisely, because "where does the schema live"
   was ambiguous in the first draft):

   The full BLDS structure is ALWAYS stored — the question is WHERE.
   It lives in three places today: (a) the **broker's channel record**
   (`ChannelEntry` schema invariants, filed by the producer's
   registration), (b) the **broker's schema registry**
   (`SchemaRecord.blds`), and (c) each role's **own config**.  What
   the first draft meant — now verified: the **shared-memory segment
   itself** is NOT one of those places.  `SharedMemoryHeader` stores
   exactly two 32-byte fingerprints (`datablock_schema_hash`,
   `flexzone_schema_hash`, `data_block.hpp`); `data_block.cpp` never
   writes BLDS text into the mapped segment; producer and consumer
   each pass a `SchemaInfo` into create/attach as a runtime ARGUMENT,
   built today from source (c), their config.

   Consequences for the generic consumer: it must obtain the structure
   from source (a) — the broker pull this draft integrates — because
   the segment alone cannot describe itself.  And the segment's
   fingerprints give a free end-to-end integrity check: recompute the
   fingerprint from the pulled BLDS and require it to equal the header
   values before mapping.  The broker's answer and the physical memory
   then cross-verify each other.  (Fold into G5's doc rules.)

### S-A2  Generic consumer under FAN-IN (the consumer OWNS the channel)

Asked directly in review 2026-07-26: does the generic pattern extend to
fan-in, where the consumer is the binding owner and OPENS the channel?
**Yes — verified allowed in code — but the timing is fundamentally
different and the flow must be stated:**

- *The uncited owner CAN open its channel.*  Verified: on the
  consumer-opens path the handler builds the channel's schema
  invariants from the `expected_*` fields UNCONDITIONALLY — with
  absent citation they are empty strings, so the channel opens with
  EMPTY schema invariants (the same state as a legacy channel).  No
  deadlock, no special case.
- *The format does not exist yet at the owner's startup.*  Under
  owner-first the consumer comes up FIRST; the schema arrives only
  when the first producer registers and files it.  So the fan-in
  generic owner cannot pull at its own REG time — it late-binds **at
  first-producer-join** (the `on_producer_joined` /
  `phase=live` signal): pull → verify pin → build interpreter →
  start reading.  This is semantically sound by construction: no
  producer ⇒ no data ⇒ no format needed yet.  (Fan-out/1:1 dialing
  consumers keep the simpler pull-right-after-own-REG flow of S-A.)
- *CORRECTED after code verification, then RESOLVED by user ruling
  (both 2026-07-26).*  The first draft overstated this flow: the
  schema validator enforces EXACT equality on every axis and **empty
  matches only empty**, so a blank-opened fan-in channel rejects every
  schema-carrying producer — no adopt-onto-blank path exists, and that
  strictness is deliberate.  **Ruling: Option B (G7) — the fan-in
  owner MUST declare its schema; S-A2 is cut.**  Net rule for fan-in:
  the owner declares (its citation validated per SI-2/G8 fix);
  config-schema producers join and are matched exactly (SI-3); generic
  (`from-channel`) roles do not exist under fan-in in v1 — the
  consumer side is the owner (SI-7 forbids from-channel there) and
  generic dialing producers are deferred (the pull they would need
  conflicts with SI-5 member-gating; recorded, not built).

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

## 2a. How a schema-less (generic) role is allowed, configured, and deployed

Written out end-to-end because "a role without a schema" sounds like a
rule violation — it isn't, and the deployment story is simple.

**Why the broker permits it.**  Two independent facts, both already in
the design:

1. *Registration does not require a schema opinion.*  The consumer
   citation rule has three modes — named, anonymous, and **empty**
   ("all expected_* empty → no validation; the consumer signals 'I
   don't care about schema'", `handle_consumer_reg_req`).  An
   uncited consumer is admitted like any other; citation is a
   verification service for consumers that already know the format,
   never an entry requirement.
2. *The channel always knows its format anyway.*  The full BLDS is
   filed on the broker's channel record the moment the producer
   registers (or, fan-in, when the first producer joins the owner's
   channel).  Schema integrity on the DATA path is enforced by the
   channel invariants + per-slot checksums regardless of whether any
   consumer cited anything.  So an uncited consumer weakens nothing —
   it only skips a pre-flight check on its OWN copy, and a generic
   role has no own copy to check.

**Config setup.**  Today a consumer config supplies the format in
`in_slot_schema_json` — either inline BLDS JSON or a schema-file
reference resolved from `schema_dirs` (HEP-0018 authority).  This
design adds a third, explicit form:

```jsonc
// generic archiver attachment — no format knowledge in the config
"in_channel":           "lab.raw",
"in_channel_topology":  "fan-out",      // deployment fact, stays declared
"in_transport":         "zmq",
"in_slot_schema":       "from-channel", // ⇐ NEW explicit sentinel:
                                        //   resolve at runtime from the
                                        //   broker's channel record
"in_schema_fingerprint": "ab34…"        // OPTIONAL 128-hex pin: verify the
                                        //   pulled format against this and
                                        //   abort on mismatch.  Integrity
                                        //   without structure duplication.
```

An explicit sentinel, NOT empty-string magic — an absent/empty schema
field stays an error exactly as today (no silent fallback; a config
that says nothing is a broken config, a config that says
`from-channel` is a decision).

**Deployment.**  Nothing new: the generic role is provisioned like any
role — keypair, `known_roles` entry, one small config per attachment
point.  Ten channels to archive = ten tiny configs that differ only in
channel name (today it would be ten configs each carrying a full copy
of the format, drifting independently).  The role binary and script
are identical across all of them.

**Startup walk (ties the pieces):** build queue schema-pending →
register uncited, `AWAITING_OWNER` retry absorbs any wait → **REG_ACK
arrives carrying the channel's schema (§2b)** → if a fingerprint pin
is configured, verify and abort on mismatch → (SHM: additionally
cross-check against the segment-header fingerprints, §2 S-A) →
activation applies schema + endpoints + allowlist together
(`apply_master_approval`, the existing S3 step) → script's `on_init`
runs with the format available (G4 slice 4 for script-side field
access).  Under fan-in there is no generic-role variant in v1: the
consumer IS the owner and must declare (SI-1, Option B); generic
dialing producers are deferred (S-A2).

---

## 2b. Delivery at establishment — the schema rides the REG/ACK (user direction 2026-07-26)

Review correction: the first draft framed schema delivery as pull-only
(`SCHEMA_REQ` after registration).  User direction: the BLDS should be
**established during REG/ACK** — and this is the better design, for
reasons the contract itself states:

- **C1 says the ACK is "your view of the book."**  A dialing consumer's
  `CONSUMER_REG_ACK` already delivers the establishment payload — the
  owner's endpoint, its key, and (SHM) `shm_capability_endpoint`.  The
  channel's schema is part of that view; delivering it in the same
  reply is the contract-aligned shape, not an optimization.
- **Exact precedent already in the design:** the allowlist is seeded in
  the ACK (`initial_allowlist`) AND has a pull message
  (`GET_CHANNEL_AUTH_REQ`) for later refresh.  Schema gets the same
  two-part shape: **seeded in the ACK at establishment; `SCHEMA_REQ`
  remains the pull** for the cases the ACK cannot serve (below).
- **SHM becomes fully natural — and fully COVERED:** the same ACK that
  says WHERE to attach says WHAT the memory contains; the consumer
  verifies the delivered BLDS's fingerprint against the segment-header
  hashes before mapping (§2 S-A cross-check).  No extra round trip, no
  timing question.  And because fan-in × SHM is refused by the §3.3.0
  matrix, **every SHM channel is producer-owned: its format exists
  from the moment the channel exists** (the owning producer's config
  supplies it at registration).  The blank-channel problem (S-A2/G7)
  is therefore a ZMQ-fan-in-only question — SHM generic consumers are
  completely served by ACK delivery with no open gap.
- **G1 largely dissolves for dialing consumers:** the queue already
  late-binds ACK-derived state (endpoints, allowlist) at activation via
  `apply_consumer_reg_ack` → `apply_master_approval`.  Schema arriving
  in the ACK rides that EXISTING path — the "schema-pending build"
  relaxation remains, but no new delivery plumbing and no startup
  reordering.

Wire shape: additive optional fields on the typed `ConsumerRegAckBody`
(schema_id, schema_owner, blds, flexzone_blds, packing, schema_hash —
the same field set the channel record stores).  BLDS is a compact
canonical string; the ACK remains a control-plane reply, not a bulk
payload.  Producer REG_ACK is unchanged (producers supply schemas;
they don't need them back).

**What the pull (`SCHEMA_REQ`) still exists for** — with S-A2 cut
(Option B), establishment is fully served by the ACK; the pull remains
for registry tooling reads by `(owner, schema_id)` and on-demand
re-verification by members.  Same division of labor as allowlist-seed
vs. auth-pull refresh.

**Who knows what, when (trust sequence — ratified in review
2026-07-26).**  The BLDS is delivered ONLY inside a success ACK, which
means only after the role has passed every gate: CURVE transport
authentication, the known-role identity binding, and admission
validation (topology, cardinality, citation checks where cited).  A
party that is unauthenticated, unknown, or rejected never receives
structure.  The consumer's prior knowledge can be as small as the
FINGERPRINT (its config pin) or nothing at all; the verification chain
on receipt is: config pin (if present) → delivered BLDS must hash to
it → (SHM) the same fingerprint must equal the segment header's
stamped hashes before mapping.  Broker's claim, consumer's
expectation, and physical memory must all agree; any disagreement is a
startup abort that names the mismatched pair.

---

## 3. Gap register

| # | Gap / conflict | Severity | Resolution options | Recommendation |
|---|---|---|---|---|
| **G1** | **Startup sequencing: queues are built (S1) from config `SchemaSpec` BEFORE the BRC exists; a runtime-resolved schema is only available after control-plane connect.**  Affects S-A/S-C. | Structural — **largely RESOLVED by §2b** (user direction: the schema rides the REG/ACK) | The ACK-delivery design (§2b) collapses the earlier options: for dialing consumers the schema arrives on `CONSUMER_REG_ACK` and rides the EXISTING S3 activation (`apply_master_approval`), exactly like endpoints and allowlist — no reordering, no extra round trip.  Residual work: the queue's schema-pending Standby state (build without schema, accept it at Configured — the ZMQ factories' empty-schema hard-reject relaxes into a staged check).  (The fan-in-owner residual was eliminated by Option B: owners always declare, so no open-time schema gap exists anywhere.) | Adopted per user direction 2026-07-26.  The first draft's rejection of "schema in the ACK" is WITHDRAWN — the "heavy ACK" concern was wrong (BLDS is a compact canonical string) and contract C1 makes the ACK the aligned carrier ("your view of the book"). |
| **G4** | **Script engines compile slot proxies from config `SchemaSpec`; no engine can build slot accessors from a runtime-pulled BLDS.**  Without this, "schema-driven" stops at the C++/native tier. | Structural (largest work item) | (a) Engine support: build the slot proxy from a runtime `schema::SchemaInfo` (the C++ BLDS interpreter exists; the binding layer needs to accept it post-config).  (b) v1 punt: generic roles are native-engine only; Lua/Python get raw-bytes + a BLDS-describe API. | **(a) as its own slice**, after the plumbing slice; 3-engine parity is the project rule, and (b) would create a two-tier script ecosystem.  Sequence it last — everything else is useful without it (S-B, S-E work today with plumbing only).  Adopted — slice 4, decision 3 proceeding. |
| **G2** | No control-plane-only (observer) role kind for S-F. | Deferred | Fold into #292 role-binary unification as a named requirement ("a role kind with BRC + heartbeat + no data channel"). | Defer to #292; record there. |
| **G3** | **Neither handler checks the caller.**  `SCHEMA_REQ`/`METRICS_REQ` answer any CURVE-authenticated known role about any channel.  Compare: `GET_CHANNEL_AUTH` is binding-side-gated, `GET_CHANNEL_PRODUCERS` was consumer-gated — the project's blast-radius discipline gates reads. | Design decision | (a) Member-gate both channel-form queries (caller must hold a presence on the channel — `is_role_registered_on_channel` exists); all-channels METRICS form becomes hub-script/admin-only (wire form requires `channel_name`).  (b) Leave open to all known roles (metrics/schemas are observability/structure, not secrets — hostnames/pids/SHM names are the only mild recon surface). | **(a)** — matches least-privilege precedent, and every v1 scenario (S-A…S-E) pulls only channels the caller is registered on.  The (owner,id) SCHEMA form stays known-role-open (the registry is shared infrastructure, and hub-globals have no channel to be member of).  Loosening later for an observer role is a deliberate #292-era grant, not a default. Adopted — decision 2, proceeding. |
| **G5** | Freshness/lifetime semantics are implicit. | Doc | Write into the integration: metrics freshness = heartbeat cadence; schema validity = channel lifetime (S-D); pull-at-establishment pattern; fan-in dual-lifetime rule (channel form for consumers, owner/id form for tooling); SHM cross-verification rule (pulled BLDS fingerprint MUST match the DataBlock header hashes before mapping — S-A refinement). | Fold into HEP-0034 §10.3 + HEP-0019 when the slice lands. |
| **G6** | No typed bodies for either message (JSON handlers). | Tracked | Already on the HEP-0046 EnvelopeOnly follow-on list; add `SchemaReqBody`/`MetricsReqBody` when giving them clients (the natural moment). | Do with slice 1. |
| **G7** | **✅ RESOLVED — user ruling 2026-07-26: Option B.**  The fan-in owner MUST declare its schema; adopt-onto-blank is not built.  S-A2 is cut from scope.  This yields the unifying config rule of the whole design: **`from-channel` is legal only on DIALING sides — every OWNER declares the format when it opens the book** (fan-out / 1:1 producer; fan-in consumer).  Matches the machine exactly: the book the owner establishes (C1/C2) includes the format; dialers receive their view of it (REG/ACK, §2b).  Enforcement is two-layer: config-time (the role host rejects `from-channel` on an owning side as a config error) and broker-side (the fan-in open row requires schema material — see G8 unified rule). | Resolved | — | Scope note stands: this only ever concerned ZMQ fan-in; SHM and all producer-owned channels were never affected. |
| **G8 (+G8b)** | **The OPEN row installs contracts it never validates — two holes, same shape (both verified in code 2026-07-26).**  Answering "is the fingerprint always required and checked at REQ?": on every JOIN against an existing channel — yes, unconditionally (the front-door compares fingerprints exactly, all modes, both sides).  On NAMED registrations — yes (hash required, `verify_request_fingerprint` self-check).  But on the two ESTABLISH paths the self-check is skipped: **(G8)** the fan-in owner's citation block is guarded by `!consumer_will_open_channel` — a stale/typo'd config hash seeds an internally inconsistent contract and every honest producer is then rejected `SCHEMA_MISMATCH`, the failure landing on the wrong party; **(G8b)** an ANONYMOUS producer (no schema_id) skips the whole §10.1 validation block, yet `schema_inv.{hash,blds}` are filled from the wire unconditionally — a fresh channel can open with an inconsistent hash/structure pair, or with structure and NO hash at all (an unciteable channel: every honest citer computes a real fingerprint and is rejected against the empty one). | Real defects (pre-existing) | **One unified rule closes both, stated in §4.7.0.3 terms: the OPEN row validates the contract it installs, at least as strictly as the JOIN row checks those who match it.**  At any channel-open carrying schema material: structure present → hash REQUIRED and must equal the recomputed fingerprint (`MISSING_HASH` / `FINGERPRINT_INCONSISTENT` before the book opens).  Under Option B (G7) a fan-in consumer-open with NO schema material is itself rejected. | Fix + L2 pins in slice 1 (the helper exists; both sites are one branch away from it). |

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
pass-throughs (Class-C routing).  G3(a) member-gating — which has a
prerequisite the first draft of this document missed (fresh-eyes
2026-07-26): **both handlers today take only `(const nlohmann::json &)`
and cannot see the caller**, so gating requires migrating them to the
identity-aware handler shape first (as `handle_check_peer_ready_req`
already is).  Correction to the first draft: both messages ALREADY have
admission-tier rows (`Tier::EnvelopeOnly` in the wire_dispatch table) —
there is no tier bypass to close; the upgrade is
`SchemaReqBody`/`MetricsReqBody` typed bodies (moving them up from
EnvelopeOnly) + the identity-aware signatures.  HEP-0007 §12.2.1 note
flips from "no production caller" to the contract; L2 handler pins
(both forms, gating, error paths) + one L3 wire round-trip.  Unlocks
S-B and S-E immediately.

**Slice 2 — engine bindings (3-engine parity).**  `api.get_schema` /
`api.get_channel_schema` / `api.get_channel_metrics` in Lua + Python +
Native; HEP-0028 + README_topology_channels docs.  Unlocks S-E for
scripts on all engines.

**Slice 3 — schema-at-establishment (§2b) + schema-pending queues.**
`ConsumerRegAckBody` gains the optional schema fields; the broker fills
them from the channel record; the queue accepts schema at Configured
(relaxing the build-time empty-schema reject); `apply_master_approval`
applies it alongside endpoints/allowlist; the SI-6 chain (pin →
delivered → SHM header) closes at activation, with a clean abort on an
empty format.  Unlocks S-A/S-C for the native tier.

**Slice 4 — runtime BLDS slot proxies (G4).**  Engine-side; the last
mile to fully generic script roles.

(#292 carries G2's observer role kind.)

---

## 5. Decisions requested (⚖)

1. **G1 resolution** — RESOLVED by user direction 2026-07-26: the
   schema is established during REG/ACK (§2b); the queue's
   schema-pending Configured stage carries it in with the rest of the
   ACK state.  No startup reordering.
2. **G3 gating** — PROCEEDING on the recommended option (member-gated
   channel queries; known-role-open `(owner,id)` registry reads),
   baked into SI-5/MI-1 per the "continue" direction 2026-07-26.
   Flag any objection before slice 1 lands.
3. **Slice order** — PROCEEDING 1→2→3→4 per the "continue" direction
   2026-07-26.
4. **RESOLVED 2026-07-26 (clarified by user).**  The question this
   decision originally answered — "store the BLDS text INSIDE the
   shared-memory segment?" — was the reviewer's own framing, not the
   user's proposal.  The user's actual direction: **establish the BLDS
   during REG/ACK** — adopted as the primary delivery path, §2b.  The
   in-segment sub-question is settled **no, fingerprints-only**, for
   the record:
   - Every sanctioned SHM attach is broker-mediated by design
     (HEP-0041 capability-fd handshake: the consumer receives the
     memory descriptor FROM the broker flow) — a reader that can
     reach the segment can always reach the fetch path, so a second
     in-segment copy serves no sanctioned reader.
   - A second copy of the structure is a drift surface; the
     fingerprints already bind segment ↔ broker record
     cryptographically without duplication (one source of truth).
   - `SharedMemoryHeader` is frozen ABI under the Core Structure
     Change Protocol, and BLDS text is variable-length — it cannot go
     in the fixed header; it would need a new versioned region and an
     offset remap (full mandatory-review ceremony) for the marginal
     value above.
   - The one scenario that would justify self-description —
     broker-less post-mortem forensics — does not exist today:
     verified, the recovery tooling (`data_block_recovery.cpp`)
     operates purely structurally and never interprets payload
     fields.  If field-level forensics ever becomes a requirement,
     the right shape is a dedicated versioned self-description
     region appended at creation — recorded here as the future
     option, deliberately not built now.
