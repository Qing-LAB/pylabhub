# DRAFT: HEP-CORE-0041 — Pattern 4 Reform Coverage Matrix (v1.1)

**Status:** Transient design artifact, in-progress.
**Created:** 2026-06-24.  v1.1 amendment: §2.4 + §2.5 + §4.1 updated to
reflect option (B) decision — narrow accessor APIs on `DataBlockProducer`
instead of extending the wide-surface `DataBlockDiagnosticHandle` to
fd-source.  Shipped in S2c-6 (commit forthcoming).
**Authors / drivers:** post-#275-S2c discussion on Pattern 4 applicability to deferred tests.
**Sibling docs:**
- `DRAFT_HEP-0041-test-completeness_2026-06.md` v3.1 — drives the #275-S2 migration plan; this doc is its dual.
- `docs/README/README_testing.md` § "Pattern 4 — Multi-process wire protocol test" — pattern definition.
- `docs/HEP/HEP-CORE-0036.md` §7.4 — single-pumper invariant motivating Pattern 4.
- `docs/HEP/HEP-CORE-0041.md` — capability transport that makes Pattern 4 the natural shape for SHM-cross-process tests.

---

## 1. Why this matrix exists

`#275-S2c` finished mechanical in-process migration of L3 tests off the
secret-gated path.  Along the way, several tests were **retired** or
**deferred** because their structure (multi-process by `/dev/shm` name,
or name-based diagnostic-helper introspection) cannot map cleanly onto
the fd-source single-process helper pattern.

`#275-S3/S4/S5` will delete the legacy `create_datablock_producer<F,D>` /
`find_datablock_consumer<F,D>` name-based factories, the C-API secret
param, and rename `SharedMemoryHeader::shared_secret[64]` →
`reserved_capability_token[64]`.  When those land, every deferred test
that still calls a name-based factory will fail to compile or fail at
attach.

Without an up-front coverage decision per deferred test, S3/S4/S5 will
hit each failure in isolation and force retire-vs-reform decisions under
time pressure.  This matrix is the design artifact that closes that gap.

For each deferred/retired test it records:
- **Contract pinned** — the production behavior the test asserts on
- **Pre-S2 access path** — what production surface the test used
- **HEP-0041 status of that surface** — exists / removed / changed shape
- **Verdict** — one of:
  - **(a) Pattern 4 reform** — contract still exists in production; test must move to subprocess-per-role
  - **(b) Already correctly retired** — feature dropped or coverage moved to L2 already
  - **(c) In-process rewrite** — single-process is structurally sufficient; needs a small infrastructure assist (e.g., fd-source diagnostic helper)
  - **(d) Wholesale retire** — test was pinning an internal implementation detail the new model doesn't expose

---

## 2. Matrix

### 2.1 Retired in S2a (commit `2b10b5cb`)

| Test | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `DatahubProducerConsumerTest.FindConsumerWrongSecretReturnsNull` | C-API `memcmp(stored_secret, supplied_secret, 64) != 0` gate in `find_datablock_consumer_impl` | name-based factory + secret param | gate REMOVED — fd possession IS auth | (b) — L2 covers via `test_attach_protocol.cpp::RejectsConsumerWithWrongSeckey` (MAC fail = stronger cryptographic gate at correct layer) |
| `DatahubShmQueueTest.CreateReaderWrongSecret` | ShmQueue legacy `create_reader(name, secret, ...)` secret check | name-based ShmQueue factory | factory RETIRES in #275-S3 | (b) — L2 covers via `test_hub_shm_queue_capability.cpp` Tests 3-6 (state-machine refusals on capability path) |

### 2.2 Retired in S2b (commit `3c8538c9`)

| Test | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `DatahubWriteAttachTest.*` (4 tests, file deleted) | secondary-writer attach pattern | `attach_datablock_as_writer_impl` C-API | feature DROPPED — zero production users | (b) — feature gone, no coverage debt |

### 2.3 Retired in S2c-2 (commit `1b10ba05`)

| Test | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `DatahubE2ETest.LatestOnlyEndToEndDeliversLastSlot` | end-to-end producer→consumer Latest_only delivery | producer subproc + consumer subproc + named SHM | SHM transport now via fd + SCM_RIGHTS; broker mediates attach | **(a) Pattern 4 reform** → `Pattern4DataPipelineTest.LatestOnlyEndToEnd` |
| `DatahubE2ETest.SequentialEndToEndDeliversAllSlots` | Sequential policy delivers all slots in order | same | same | **(a)** → `Pattern4DataPipelineTest.SequentialOrdered` |
| `DatahubE2ETest.ConsumerSeesProducerExitInIsRunning` | consumer observes producer-process death via `is_running()` | header `producer_running` flag + named SHM | flag still exists in header; access via fd-source consumer | **(a)** → `Pattern4DataPipelineTest.ProducerExitVisibleToConsumer` |
| `DatahubE2ETest.ConsumerSeesFlexZoneWriteInRoundTrip` | flexzone bidirectional writes visible across processes | mmap'd region in named SHM | mmap region preserved over fd-source; visible across SCM_RIGHTS-shared fd | **(a)** → `Pattern4DataPipelineTest.FlexZoneRoundTrip` |
| `DatahubE2ETest.ConsumerHandlesLateStart` | consumer attaches AFTER producer-started | name lookup retry on `/dev/shm` | broker mediates attach via `CONSUMER_ATTACH_REQ` — broker holds producer's transport; late consumers ask broker | **(a)** → `Pattern4DataPipelineTest.LateConsumerAttachViaBroker` |
| `DatahubE2ETest.WriteAcquireBackpressureUnderSequential` | producer blocks when ring full + Sequential consumer slow | producer + consumer subprocs racing on named SHM ring | ring still in SHM; access via fd-source pair | **(a)** → `Pattern4DataPipelineTest.SequentialBackpressure` |
| (any other e2e tests masked or skipped) | TODO: re-confirm against the deleted file's pre-deletion contents | — | — | **(a)** |

### 2.4 Deferred in S2c-4 — RESOLVED in S2c-6 via option (B)

| Test | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `DatahubHeaderStructureTest.SchemaHashesPopulatedWithTemplateApi` | `SharedMemoryHeader::flexzone_schema_hash` + `datablock_schema_hash` populated when typed template is used | name-based `open_datablock_for_diagnostic(channel)` → reads header bytes | header fields PRESERVED (S5 only renames `shared_secret`, not schema fields) | **(c) ✅ Resolved S2c-6** — migrated to `producer->flexzone_schema_hash()` / `producer->datablock_schema_hash()` accessors (return `std::array<uint8_t, 32>` by value — no borrowed mmap view) |
| `DatahubHeaderStructureTest.SchemaHashesZeroWithoutSchema` | same fields all-zero when `_impl` called with `nullptr` schemas | same | same | **(c) ✅ Resolved S2c-6** — same accessors; producer minted via inline transport + `create_datablock_producer_from_fd_impl` (since helpers always pass non-null schemas) |
| `DatahubHeaderStructureTest.DifferentTypesProduceDifferentHashes` | different F/D pairs → different hash bytes | same (two channels in same process) | same | **(c) ✅ Resolved S2c-6** — same accessors; uses `std::array operator!=` for hash comparison |

### 2.5 Deferred in S2c-5 — RESOLVED in S2c-6 via option (B)

All 5 sites previously consumed `open_datablock_for_diagnostic(ch)` to
inspect heartbeat fields in the SHM header.  S2c-6 migrated each to the
matching `DataBlockProducer` accessor.

| Site (worker function) | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `consumer_auto_registers_heartbeat_on_construction` | `active_consumer_count` increments on consumer attach | name-based diag helper | field PRESERVED; in-process consumer attach observable from same process | **(c) ✅ Resolved S2c-6** — `producer->active_consumer_count()`; consumer attached inline via `find_datablock_consumer_from_fd<F,D>` over `::dup(p.transport->borrow_fd())` to preserve 0→1 transition observability |
| `consumer_auto_unregisters_heartbeat_on_destroy` | counter returns to 0 on consumer scope exit | same | same | **(c) ✅ Resolved S2c-6** — same accessor; consumer in scope block |
| `all_policy_consumers_have_heartbeat` | independent counters across multiple consumer attaches | same | same | **(c) ✅ Resolved S2c-6** — same accessor; 2 consumers via separate fd dups |
| `producer_operator_increment_updates_heartbeat` | `last_heartbeat_ns` advances on heartbeat tick | same | same | **(c) ✅ Resolved S2c-6** — `producer->last_heartbeat_ns()` |
| `consumer_operator_increment_updates_heartbeat` | `consumer_heartbeats[i].last_heartbeat_ns` indexed per consumer | same | same | **(c) ✅ Resolved S2c-6** — `producer->consumer_heartbeat_ns(slot_index)` with iteration over `MAX_CONSUMER_HEARTBEATS` to find the occupied slot (matches pre-S2c-6 scan logic) |

### 2.6 Deferred in S2c-5 (commit `5664d388`, datahub_stress_raii)

| Test | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `DatahubStressRaiiTest.MultiProcessFullCapacityStress` | ring-wrap under stress with multi-consumer fan-out + checksum enforcement | producer subproc + N consumer subprocs + named SHM | SHM ring preserved; production geometry IS one role per process | **(a) Pattern 4 reform** → `Pattern4DataStressTest.MultiConsumerRingWrap` |
| `DatahubStressRaiiTest.SingleReaderBackpressure` | Sequential policy blocks producer when single reader is slow | same (1 producer subproc + 1 consumer subproc) | same | **(a)** → `Pattern4DataStressTest.SequentialBackpressure` (subset of `Pattern4DataPipelineTest.SequentialBackpressure` — possibly fold) |

### 2.7 Sibling deferrals from pre-#275 tasks (#182 / #183 / #184)

These were deferred to "deeper rework" BEFORE #275 started.  They have the
same structural problem (multi-process subprocess pattern + named-SHM
attach) and are natural members of a single Pattern 4 reform task.

| Worker file | Task | Verdict |
|---|---|---|
| `datahub_broker_health_workers.cpp` | #182 | **(a) Pattern 4 reform** — broker + role subprocesses with health-check wire frame; pin BRC heartbeat + hub-dead detection over the production wire |
| `datahub_broker_workers.cpp` | #184 | **(a)** — broker subproc + role subprocess; CHANNEL_NOTIFY_REQ + REG_REQ wire-protocol pinning |
| `datahub_role_state_workers.cpp` | #184 | **(a)** — same |
| (`datahub_e2e_workers.cpp` from #183) | already retired in S2c-2, see §2.3 | — |

---

## 3. Verdict tally

- **(a) Pattern 4 reform**: 6 e2e tests (§2.3) + 2 stress tests (§2.6) + 3 sibling files (§2.7) = ~11 tests + 3 file-fanouts
- **(b) Already correctly retired**: 2 secret-gate tests (§2.1) + 4 WriteAttach tests (§2.2) = 6 tests, NO debt
- **(c) In-process rewrite**: 3 header_structure tests (§2.4) + 5 policy heartbeat sites (§2.5) = 8 sites, needs one shared helper (`open_datablock_for_diagnostic_from_fd`)
- **(d) Wholesale retire**: 0

---

## 4. Recommended execution order

### 4.1 ✅ S2c-6 — landed

**Verdict (c) — In-process rewrite** shipped via option (B): narrow
accessor APIs on `DataBlockProducer` instead of extending the
diagnostic helper to fd-source.  Design rationale captured in the
S2c-6 commit message + the per-test verdict rows in §2.4/§2.5.

Accessors added to `DataBlockProducer` (all `const noexcept`, return by
value to avoid mmap-borrowed views):

- `std::array<uint8_t, 32> flexzone_schema_hash()` — immutable post-init
- `std::array<uint8_t, 32> datablock_schema_hash()` — immutable post-init
- `uint32_t active_consumer_count()` — atomic load, acquire
- `uint64_t last_heartbeat_ns()` — atomic load, acquire (producer's own)
- `uint64_t consumer_heartbeat_ns(uint32_t slot_index)` — atomic load,
  acquire (consumer slot, indexed)

8 test sites migrated (3 in header_structure + 5 in policy_enforcement).
All `cfg.shared_secret = N` lines dropped — forward-compatible with S5.
`open_datablock_for_diagnostic` (the wide-pointer helper) is no longer
called from any test file in this batch; recovery-tool consumers in
`src/utils/shm/data_block_recovery.cpp` remain.

Why (B) over the originally-proposed (c) helper extension: the helper's
`SharedMemoryHeader*` return exposes the full struct (including the
`reserved_capability_token[64]` field reserved for future capability
material under S5).  Tests reaching for the helper enable accidental
contract violations on reserved fields.  Narrow accessors keep test
coupling at the class interface level, matching existing precedents
like `ZapRouter::registered_domain_count_for_test()` and HEP-CORE-0019
§5.4.2 "Direct accessor methods."  Audit follow-up tracked as task #284.

### 4.2 Before any of S3/S4/S5 lands (mandatory)

**Verdict (a) — Pattern 4 reform** must EITHER land BEFORE S3 OR have
the affected tests masked/retired in S3 itself.  Recommended: pre-stage
the Pattern 4 reforms as a separate task so coverage doesn't regress.

Proposed sibling task **#PPP** (number assigned at task creation): "L3
Pattern 4 reform of HEP-0041-incompatible tests."  Subtasks mirror the
existing test ladder (#221-#229):
1. `Pattern4DataPipelineTest` — covers §2.3 (e2e) + §2.6 (stress) +
   §2.7 (broker_health, broker, role_state)
2. Ladder rungs in dependency order — start with the simplest
   (LatestOnlyEndToEnd) and add complexity rung-by-rung, same shape as
   the existing `Pattern4RegistrationTest` family.

Scope: ~12+ tests over multiple commits.  Same wall-clock as the
existing #221-#229 ladder work.

### 4.3 As part of #275-S5 (mandatory)

The S5 Core-Structure-Change-Protocol commit renames `shared_secret[64]`
→ `reserved_capability_token[64]`.  At that point:
- All tests in §2.1 + §2.2 are already retired (no work)
- All tests in §2.4 + §2.5 are migrated via §4.1 (no work)
- All tests in §2.3 + §2.6 + §2.7 need to be either (i) retired with a
  banner pointing at task #PPP, OR (ii) already reformed via #PPP

If #PPP hasn't shipped, S5 retires the affected tests with banners and
opens the work as #PPP's first checkpoint.  If #PPP HAS shipped, S5 is
trivial — just the field rename.

---

## 5. Open questions for designer (you)

1. **Approve verdict assignments** in §2 — particularly the 8 (c) sites.
   Is `open_datablock_for_diagnostic_from_fd` the right abstraction, or
   should we instead expose schema-hash / heartbeat fields via
   `DataBlockProducer`/`DataBlockConsumer` accessors (avoiding diagnostic
   helper entirely)?

2. **Pattern 4 reform task scope** — should the §2.7 sibling files
   (broker_health, broker, role_state) fold into #PPP, or stay as their
   own deferred tasks (#182/#184)?  Folding centralises the Pattern 4
   infrastructure work; keeping them separate keeps #PPP scoped to
   HEP-0041-driven reforms only.

3. **S5 sequencing** — if #PPP is going to be long-running (12+ tests),
   should S5 ship WITH masking of unreformed Pattern 4 tests, or should
   S5 block on #PPP completion?

---

## 6. References

- HEP-CORE-0041 (`docs/HEP/HEP-CORE-0041.md`) — capability transport spec
- HEP-CORE-0036 §7.4 — single-pumper invariant
- `docs/README/README_testing.md` § "Pattern 4" — pattern definition + test ladder pattern
- `docs/tech_draft/DRAFT_HEP-0041-test-completeness_2026-06.md` v3.1 — driver doc for #275-S2 migration
- Sibling deferred tasks: #182 / #183 / #184
- Existing Pattern 4 test ladder: #221-#229 (rungs 2-12 of broker+role wire protocol)
