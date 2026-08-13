# Questions answered by inspecting data — the queue state and endpoint correction

> **Status:** M1 + M2 SHIPPED.  Design agreed 2026-08-11; **M1 shipped
> 2026-08-12**, **M2 shipped 2026-08-12** (full sweep 2798/2798).  M3
> remains, and is smaller than this document originally claimed — see
> the correction below.  Found while reviewing the peer-row work (#145),
> scope widened twice by the owner.  Tracked as task **#148**.
>
> | Mechanism | State | Where |
> |---|---|---|
> | **M1** — the queue holds its state | ✅ shipped 2026-08-12 | `QueueState` in `hub_queue.hpp`; `state_` + `start()` gate in `hub_zmq_queue.cpp`; HEP-CORE-0036 §6.7.1 |
> | **M1b** — two mutator cells M1 left unenforced | ✅ shipped 2026-08-12 | `apply_master_approval` refuses `Uninitialized`; `stop()` terminal from `>= Configured` |
> | **M2** — bind request vs bound address as types | ✅ shipped 2026-08-12 | `BoundAddress` in `net_address.hpp`; `bound_address()` replaces `actual_endpoint()`; 6 broker sites repointed |
> | **M3** — the binding-side republish | ⬜ open | design already ADOPTED in HEP-CORE-0021 §16.6 — this is an implementation gap, not a design one |
>
> Symptoms 1-7 are all CLOSED except the producer-republish asymmetry,
> which is M3.
>
> **Correction to this document, 2026-08-12.**  Two claims above were
> wrong and are corrected in place below:
>
> 1. *"Symptom 3 is closed by M1."*  It is not.  `is_admission_populated()`
>    still branches three ways on container emptiness
>    (`hub_zmq_queue.cpp`).  What M1 removed was its COUPLING to
>    `is_configured()`, which no longer consults it.  The three-way
>    branch is defensible on its own terms — the question "do I have
>    peers?" is genuinely a question about payload, and the queue is its
>    owner — so nothing further is owed here.  The symptom table
>    overstated the fix.
> 2. *"There is no producer-side republish anywhere in the protocol."*
>    **False.**  HEP-CORE-0021 §16.6 specifies the bind → resolve →
>    publish sequence, ADOPTED 2026-07-08, complete with wire (§16.5),
>    mid-life rules (§16.8), readiness gate (§16.7), log markers (§16.10)
>    and test surface (§16.11) — and §16 carries an amendment
>    instructing that every "producer" in it be read as "the binding
>    side".  M3 is therefore not new design.  It is implementing an
>    adopted contract, which makes it cheaper than this document
>    assumed and removes the sign-off gate it implied.

---

## 1. One defect, seven symptoms

Every finding below is the same mistake:

> **The code answers a question by inspecting a piece of data, instead of
> asking the component that knows.**

An empty string is asked *"has registration been applied?"*.  A configured
endpoint is asked *"am I bound?"*.  A syntax validator is asked *"can a
peer connect to this?"*.  A peer vector is asked *"am I configured?"*.

Each inference is right often enough to survive, and wrong in a case
nobody currently exercises.  That is why the suite is green and the
design is not.

| # | Symptom | The question | What is inspected instead |
|---|---|---|---|
| 1 | `is_configured()` returns true in Standby on a binding queue | has apply run? | `!endpoint.empty()` |
| 2 | §6.7's four states are logged, never stored | what state am I in? | three unrelated bools |
| 3 | `is_admission_populated()` branches three ways | do I have peers? | which of 3 containers is non-empty |
| 4 | `actual_endpoint()` returns `:0` before bind | am I bound? | `actual_endpoint.empty()` |
| 5 | `if (!ep.empty())` at the publish site — a guard that cannot fail | is this value usable? | string emptiness |
| 6 | one validator serves bind-requests and dial-targets | can a peer connect? | endpoint *syntax* |
| 7 | consumer stores the peer pair twice, one copy used as a flag | has apply run? | presence of a duplicate |

**Symptoms 1-3 are one missing thing: a stored state.
Symptoms 4-6 are one missing thing: a type that distinguishes a bind
request from a bound address.  Symptom 7 disappears when 1 is fixed.**

---

## 2. Evidence

Each verified by reading, 2026-08-11.  Numbers are non-comment call sites.

**Symptom 1.**  `is_configured()`'s binding branch is
`return !endpoint.empty()`.  A binding queue takes its endpoint from
config at construction
(`role_api_base.cpp`, `rx_opts.endpoint_hint = is_binding ? opts.zmq_node_endpoint : {}`),
so it reports Configured before `apply_master_approval` has run.
Unobservable today: one production caller (`start()`), itself only called
from apply.

*Why no test caught it:* five tests assert `is_configured()`; four are
`bind=false`.  The fifth (`test_hub_zmq_queue.cpp:1225`) IS a binding
queue, but its schema is empty so `schema_pending_` short-circuits above
the binding branch.  **No test reaches that branch with a valid schema.**

**Symptom 2.**  `event=QueueStateTransition from=Standby to=Configured`
is logged at two sites.  No state field exists.

**Symptom 3.**  Binding → `allowlist_ != nullptr && !peers.empty()`;
dialing read → `!producer_peers_.empty()`; dialing write →
`!server_pubkey_z85_.empty()`.  Note the last tests a strict subset of
what `is_configured()` tests for the same side.

**Symptom 4.**  `return actual_endpoint.empty() ? endpoint : actual_endpoint`
— identical in `ZmqQueue` and `InboxQueue`.  `AdminService::bound_endpoint()`
returns empty until bound, i.e. **one of the three already does it right.**
The inbox DEFAULTS to `tcp://127.0.0.1:0`, so unresolved is its normal
starting state.

**Symptom 5.**  `role_api_base.cpp:2130` publishes the fan-in consumer's
bound endpoint to the broker — the value every producer on that channel
receives as its dial target.  Its guard is `if (!ep.empty())`, and since
`actual_endpoint()` falls back to a non-empty configured string, **it can
never fail.**

**Symptom 6.**  `validate_tcp_endpoint` has eight call sites:

| Sites | Checking | `:0` legal? |
|---|---|---|
| `inbox_config.hpp:97`, `transport_config.hpp:102` | a configured **bind request** | **yes** |
| `broker_service.cpp` `:2472 :3251 :3531 :5789 :5860 :5893` | a **dial target** crossing to another role | **no** |

The validator is correct for the first two and must keep accepting `:0`
there.  The other six get a syntax answer to an address question.  **No
"is this dialable" predicate exists anywhere.**

**Symptom 7.**  A dialing consumer holds the peer pair in
`producer_peers_[0]` (which `start()` connects from) and again in the two
scalars (which nothing dials from).  The duplicate exists so
`is_configured()` has something to inspect.

**The asymmetry these produce.**  `send_endpoint_update` has ONE caller,
guarded to the binding **consumer**.  A binding **producer** registers
before it binds and never republishes:

| Side | Sequence | Ephemeral port works? |
|---|---|---|
| Binding consumer (fan-in) | register → bind → **republish resolved** | yes |
| Binding producer (1:1, fan-out) | register with **configured** value → bind → nothing | **no** |

Not live — shipped examples use explicit ports (5580-5583) — but the
system accepts a configuration it cannot serve, and the failure presents
as consumers that never connect.

---

## 3. The correction — three mechanisms, not seven patches

The rule the amendment establishes:

> **Every question has one owner and a typed answer.  Callers ask.  They
> never re-derive an answer from the shape of returned data.**

### M1 — The queue holds its state

One `std::atomic<State>` over the §6.7 enum, driven by the mutators §6.7
already names.  Predicates become reads of it.

- **Fold in `running_`** — it *is* the Active state.
- **Fold in `dial_pending`** — it *is* a resting point between Configured
  and Active (set instead of calling `start()`, cleared immediately
  before the deferred `start()`).
- **Keep `schema_pending_` separate** — it answers "do I know my data
  format", a different axis, and merely *gates* the Standby→Configured
  edge.  Folding it in would put two axes in one variable, which is this
  whole document's mistake one level up.

*Reachability confirms the split:* `running_ && dial_pending` is
unreachable (cleared before start), and `running_ && schema_pending_` is
unreachable (apply refuses on a pending schema while not running).  Two
mutually-exclusive positions on one line, plus one independent gate.

*Constraint:* `is_running()` is read from outside the queue as a relaxed
atomic load.  The state must stay lock-free readable — `atomic<State>`
preserves that; a state inside the mutex would make every reader lock.

Kills symptoms 1, 2, 3, 7.

### M2 — A bind request and a bound address are different types

This is the part that must not be done as a predicate.  Adding an
`is_dialable()` that callers must remember to call reproduces symptom 5 —
a check someone forgets, or writes their own weaker version of.

The two things are genuinely different and should not share
`std::string`:

- a **bind request** — what config carries, `:0` legal, cannot be
  published to a peer
- a **bound address** — exists only after `zmq_bind()` resolves, `:0`
  impossible by construction, the only thing publishable

Then "can a peer connect to this" stops being a question anyone asks: a
function that publishes to a peer *takes a bound address*, and a config
string will not compile there.  `validate_tcp_endpoint` keeps its current
job — validating bind requests — unchanged and correct.

Kills symptoms 4, 5, 6, and turns the producer's missing republish (below)
from a silent gap into a hole the type system points at.

### M3 — The producer gets the republish it never had

Symmetric with `role_api_base.cpp:2138`.  M2 makes the absence visible;
M3 fills it.  Without M3, M2 only converts a broken channel into a
refused one.

---

## 4. Why this is a protocol correction, not a cleanup

M1 changes what §6.7's normative mutator table means: the queue **holds**
a state rather than presenting one.  M2 changes what may travel on
`REG_REQ.zmq_node_endpoint` and `producers[].endpoint` — a bound address,
never a request.  M3 adds a producer-side message flow that does not
exist today.

All three are contract changes.  None can be done as a local repair, and
doing any one alone leaves the system in a worse state than now:

| Alone | Result |
|---|---|
| M1 only | endpoints still publish placeholders |
| M2 only | configs accepted today start being refused, with no path to make them work |
| M3 only | the republish still publishes an untyped string nobody validates |

---

## 5. Sequence

**A. HEP-CORE-0036 §6.7 amendment — OWNER SIGN-OFF GATE.**
State the four states, the legal transitions, who drives each, and
`schema_pending_` as an explicit precondition on one edge.  Add the
endpoint contract: what may travel on the wire and when it becomes
knowable.  No code before this is signed.

**B. M1 — the state.**  ✅ **DONE 2026-08-12.**  `QueueState` introduced
(deliberately ordered so `>= Configured` is the honest spelling of
`is_configured()`); `running_` and `dial_pending` folded in;
`apply_master_approval`, `start()` and `stop()` set it; `start()` gates
on it and its refusal diagnostic now names the state instead of
re-deriving it from two locked members.

What the fix exposed, which is the part worth remembering: **58 tests
failed the moment the gate became real.**  Every one of them called
`start()` on a Standby queue — the migration §6.7 had mandated and
nobody had performed.  They passed before because the predicate they
depended on was the bug.  Also deleted in the same pass:
`RxQueueOptions::producer_peers`, a production field with zero
production writers whose only consumer was a `build_rx_queue` branch
that called `start()` itself, bypassing approval — production surface
that existed to let one test skip the broker.

**C. M2 — the endpoint types.**  ✅ **DONE 2026-08-12.**  `BoundAddress`
lives in `net_address.hpp`: no default constructor, no mutators, and the
only way to obtain one is `parse`, which rejects port 0 and malformed
input.  `actual_endpoint()` is gone; `bound_address()` returns
`std::optional<BoundAddress>` on both `ZmqQueue` and `InboxQueue` and
never falls back to the configured string.  `send_endpoint_update` takes
a `BoundAddress`, so the publish site cannot be handed a request.  The
six broker port-0 checks became one predicate — and got STRICTER as a
side effect: four of them had the shape `ok() && port == 0`, which
accepted anything that failed to parse.

What M2 exposed, worth remembering:

- **Two tests were pinning the defect.**
  `ActualEndpoint_BeforeStart_ReturnsConfiguredEndpoint` and the J6
  port-0 test both ASSERTED the fallback, so the bug had two green
  guards protecting it.  Both are rewritten to assert the design
  (`WhenNotYetBound_QueueReportsNoBoundAddress`,
  `WhenConfiguredWithPortZero_AddressAppearsOnlyAfterBind`) and both
  fail if any plausible-looking fallback returns.
- **`InboxSetupResult::actual_endpoint` was dead** — written at setup,
  read by nobody, because the wire value is read from the queue at
  registration.  Deleted rather than converted; typing a dead field only
  makes it a better-dressed dead field.  Its three siblings
  (`schema_json`, `packing`, `checksum`) are also unread and are left
  alone, since nothing about them was wrong.
- **The L4 marker assertion pinned an unsanctioned name.**  The code
  logged `event=BindingEndpointPublished`, which appears in no HEP;
  HEP-CORE-0021 §16.10 specifies `event=EndpointUpdatePublished`.  Code
  and test now both follow §16.10.

**D. M3 — the binding-side republish.**  Implement HEP-CORE-0021 §16.6
for the binding PRODUCER (fan-out / one-to-one), which never publishes
today because `send_endpoint_update`'s only caller is on the consumer
path (`role_api_base.cpp:2119`).  Two further §16 gaps ride along: the
publish failure path is `LOGGER_WARN`-and-continue where §16.6 specifies
a fatal exit, and the broker emits none of §16.10's four
`EndpointUpdateReq*` markers.

**E. Tests — AUDIT the existing ones, then add.  Two jobs, not one.**

*E1 — evaluate what is already there.*  Every test touching this surface
gets read against the amended contract, asking: does it pin the CURRENT
design or a superseded one, and is it complex enough to fail if the
behaviour regressed?  One instance is already known and is the template
for the audit: five tests assert `is_configured()`, four are `bind=false`,
and the fifth is a binding queue whose empty schema short-circuits the
assertion above the branch it appears to test.  Five assertions, zero
coverage of the defect.  A test that cannot fail is worse than a missing
one, because it reports as coverage.

Check each for: the trivial-case trap (asserting the empty/zero state,
which passes with the feature deleted), the short-circuit trap (an earlier
guard makes the interesting branch unreachable), and single-topology
coverage where the truth table has three.

*E1 outcome, 2026-08-12.*  Done for this surface.  Four cannot-fail
guards repaired (three `actual_endpoint().empty()` checks that the
fallback made unfailable — two of them the entire stated purpose of
their test — plus one in the fan-in round-trip); `ZmqRxNull` given a
falsifiable assertion to replace three base-class constant folds; two
comments that M1 itself had falsified corrected.

*The coverage question this raised, and its answer.*  Before moving any
of this to a higher layer, check what is already there — the rx-side
"consumer arms with CURVE" coverage **already exists at L4**
(`ZmqE2E_AuthorizedConsumerReceivesAllSlots`, the designated §7.1 pin,
real hub + real role processes; with the empty-CURVE fallback dead,
data arriving proves CURVE armed).  Proposing Pattern 4 for it would
have duplicated that.  The layering is sound: L2 owns the API state
machine, L3 Pattern-4 owns the broker's half of the §7.1 decision, L4
owns the whole chain.

*The gap that was real, now closed.*  No L3 or L4 test **observed**
`event=DialDeferred` / `event=FinalizeConnect`.  The fan-in L4 tests
depended on the deferral without checking it, so a regression to
connect-inside-apply would have passed most runs and failed only when
the producer's handshake beat the consumer's allowlist install — the
#2480 signature.  Marker assertions added to both fan-in L4 tests,
including the processor's tx side, where a single role is writer and
reader and only the writer half defers.

*E2 — new tests, derived from the amended HEP, never from current
behaviour.*  First one: a binding queue with a valid schema is NOT
Configured before apply.  It fails on today's code — that is the point.
A test written against today's `is_configured()` would assert the bug.

---

## 6. Acceptance tests for the design

1. *"What state am I in"* is answerable without reading any peer data.
2. A function that publishes an endpoint to a peer **cannot be passed** an
   unresolved one.
3. No call site tests a returned value's emptiness to decide whether it is
   usable.
4. `grep -c "\.empty()"` in these paths falls, and every survivor is
   asking about emptiness as a fact, not as a proxy for state.

---

## 7. Relation to existing work

- **Band 4 (#80)** — the RAII/lifecycle arc carries the same
  explicit-lifecycle-object idea.  If folded there, fold whole.
- **#145** closed the peer-row half of this surface; the duplicate scalars
  are the residue it could not remove while `is_configured()` read them.
- **#133** same file, unrelated cause.
- **In-repo precedents for the fix, both already correct:**
  `PeerReadinessOracle::poll()` returns a typed tri-state instead of an
  inferred bool; `AdminService::bound_endpoint()` is empty until bound
  instead of falling back to a placeholder.  Neither needs inventing.

---

## 8. Sources

| Topic | Where |
|---|---|
| Four states + normative mutator table | `HEP-CORE-0036` §6.7 (diagram ~4670, table ~4695) |
| The inferring predicate | `hub_zmq_queue.cpp` `is_configured` |
| Binding endpoint at construction | `role_api_base.cpp`, `rx_opts.endpoint_hint` |
| States logged, not stored | `hub_zmq_queue.cpp`, `event=QueueStateTransition` ×2 |
| Endpoint resolution points | `hub_zmq_queue.cpp:2224`, `hub_inbox_queue.cpp:469`, `admin_service.cpp:243`, `broker_service.cpp:1246` |
| The one republish | `role_api_base.cpp:2138` |
| Validator + its eight call sites | `net_address.hpp` `validate_tcp_endpoint` |
| Deferred dial + oracle | `hub_zmq_queue.cpp` `finalize_connect`; §6.6.3 |
