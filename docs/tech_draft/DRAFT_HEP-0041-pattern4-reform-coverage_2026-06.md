# DRAFT: HEP-CORE-0041 — Pattern 4 Reform Coverage Matrix (v1)

**Status:** Transient design artifact, in-progress.
**Created:** 2026-06-24.
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

### 2.4 Deferred in S2c-4 (commit `2a1c864e`)

| Test | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `DatahubHeaderStructureTest.SchemaHashesPopulatedWithTemplateApi` | `SharedMemoryHeader::flexzone_schema_hash` + `datablock_schema_hash` populated when typed template is used | name-based `open_datablock_for_diagnostic(channel)` → reads header bytes | header fields PRESERVED (S5 only renames `shared_secret`, not schema fields); access path is name-based | **(c) In-process rewrite** — extend diagnostic helper with `open_datablock_for_diagnostic_from_fd(int fd)` overload (~30 LOC); test stays in-process |
| `DatahubHeaderStructureTest.SchemaHashesZeroWithoutSchema` | same fields all-zero when `_impl` called with `nullptr` schemas | same | same | **(c)** — same diagnostic-helper extension |
| `DatahubHeaderStructureTest.DifferentTypesProduceDifferentHashes` | different F/D pairs → different hash bytes | same (two channels in same process) | same | **(c)** — same diagnostic-helper extension |

### 2.5 Deferred in S2c-5 (commit `5664d388`, datahub_policy_enforcement_workers.cpp)

All 5 sites consume `open_datablock_for_diagnostic(ch)` to inspect heartbeat fields in the SHM header.

| Site (worker function) | Contract pinned | Pre-S2 access | HEP-0041 status | Verdict |
|---|---|---|---|---|
| `consumer_register_heartbeat_on_construction` (line ~386) | `active_consumer_count` increments on consumer attach | name-based diag helper | field PRESERVED; in-process consumer attach observable from same process | **(c) In-process rewrite** — same diagnostic-helper extension as §2.4 |
| `consumer_deregister_heartbeat_on_destruction` (line ~424) | counter returns to 0 on consumer scope exit | same | same | **(c)** |
| `multiple_consumers_increment_active_count` (line ~464) | independent counters across multiple consumer attaches | same | same | **(c)** |
| `heartbeat_updates_timestamp` (line ~609) | `last_heartbeat_ns` advances on heartbeat tick | same | same | **(c)** |
| `heartbeat_per_consumer_slot` (line ~667) | `consumer_heartbeats[i].last_heartbeat_ns` indexed per consumer | same | same | **(c)** |

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

### 4.1 Before #275-S3 (mandatory)

**Verdict (c) — In-process rewrite** must land BEFORE S3/S4/S5 delete the
name-based factories.  Otherwise the 8 deferred sites compile-fail.

Proposed micro-task:
- Add `open_datablock_for_diagnostic_from_fd(int fd)` to `data_block.hpp`
  (~30 LOC; mirrors existing `open_datablock_for_diagnostic` but takes a
  borrowed fd instead of opening by name).
- Migrate the 3 header_structure tests to the new helper + drop their
  `cfg.shared_secret` assignments.
- Migrate the 5 policy_enforcement heartbeat sites similarly.
- Build + L0-L4 sweep + commit.

Scope: ~1 commit, ~150 LOC of test changes + 30 LOC of helper.  Belongs
as a follow-on commit within #275 itself (not a separate task) — call it
**S2c-6**.

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
