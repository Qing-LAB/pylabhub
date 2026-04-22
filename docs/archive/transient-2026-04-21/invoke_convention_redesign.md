# Invoke Convention Redesign: Directional Grouping + Consistent Returns — CLOSED

**Status**: ✅ CLOSED 2026-04-21 — all proposals implemented and shipped.
The implementation evolved past the draft on two points (both improvements
over the original proposal); see "Implementation divergence" below. Draft
preserved as historical record.
**Scope**: Script callback signatures, invoke interface
**Authoritative documentation**: HEP-CORE-0011 §callbacks (callback
signatures + Frame type names), HEP-CORE-0024 §15.3 (typed invoke
rationale), HEP-CORE-0027 (inbox API).
**Final baseline**: 1456/1456 tests (2026-04-21).

### Implementation status (verified against code 2026-04-21)

| Draft proposal | Status | Notes |
|---|---|---|
| 3-arg callback signatures (`on_produce(tx, msgs, api)`, `on_consume(rx, msgs, api)`, `on_process(rx, tx, msgs, api)`, `on_inbox(msg, api)`) | ✅ DONE | Verified in `python_engine.cpp:963` and lua/native engines |
| `InvokeTx` / `InvokeRx` / `InvokeInbox` structs in `script_engine.hpp` | ✅ DONE | Verified at `script_engine.hpp:99-120` |
| All callbacks return `InvokeResult`; True/False from script | ✅ DONE | `parse_return_value_("on_produce")` in python_engine; old None-as-commit removed |
| Frame type names: `InSlotFrame` / `OutSlotFrame` / `InFlexFrame` / `OutFlexFrame` / `InboxFrame` | ✅ DONE | Verified at `script_engine.hpp:273-277` |
| Directional flexzone in `RoleHostCore`: `set_in_fz_spec` / `set_out_fz_spec` / `in_fz_spec()` / `out_fz_spec()` | ✅ DONE | Verified at `role_host_core.hpp:104-115, 372-373` |
| Processor input flexzone (`in_flexzone_schema_json` in `ProcessorFields`) | ✅ DONE | Per draft Step 8.6 marked done |
| HEP callback documentation updated | ✅ DONE | HEP-0011 lines 201-203, 554-566; HEP-0015 + HEP-0018 superseded (binaries retired 2026-04-21); HEP-0024 §15.3 documents typed-invoke rationale |

### Implementation divergence (improvements over draft)

**Divergence 1 — Flexzone removed from invoke structs entirely.**
- Draft proposed `InvokeRx { slot, slot_size, fz, fz_size }` and
  `InvokeTx { slot, slot_size, fz, fz_size }`.
- Code shipped `InvokeRx { slot, slot_size }` and
  `InvokeTx { slot, slot_size }` — **no fz field**. Header comment in
  `script_engine.hpp:95-97`: *"Flexzone is NOT carried here — scripts
  access it via api.flexzone(ChannelSide::Rx), which returns a cached
  typed view built once at build_api() time."*
- Why this is better:
  - Slot lifecycle (acquired/released per cycle) and flexzone (long-lived
    shared region) had different lifetimes anyway; packing them into the
    same struct was a false symmetry.
  - Cached typed view at `build_api()` time gives scripts a stable
    Python/Lua object across cycles — no per-cycle reconstruction.
  - No `const_cast` gymnastics — the runtime API holds the right type
    up front.

**Divergence 2 — `Rx`/`Tx` for runtime predicates; `in`/`out` for config.**
- Draft proposed `has_in_fz()` / `has_out_fz()` for runtime predicates.
- Code shipped `has_rx_fz()` / `has_tx_fz()` (`role_host_core.hpp:118-119`)
  for runtime predicates while keeping `set_in_fz_spec()` / `in_fz_spec()`
  for setters/getters and `in_flexzone_schema` for the JSON config key.
- Why this is better: formalises the runtime-channel-side concept
  (`ChannelSide::Rx|Tx`, used in `api.flexzone(ChannelSide::Rx)`) as
  distinct from the config-time directional concept (`in_`/`out_`). One
  prefix forced everywhere would have conflated them.

---

## 1. Problem

Current callback signatures are inconsistent:

```python
on_produce(out_slot, fz, msgs, api) -> bool       # 4 positional, fz is output
on_consume(in_slot, fz, msgs, api) -> None         # 4 positional, fz is input, void return
on_process(in_slot, out_slot, fz, msgs, api) -> bool  # 5 positional, fz is OUTPUT ONLY
on_inbox(slot, sender_uid, api) -> None            # 3 positional, void return
```

Issues:
- Processor has no input flexzone (config gap + data loop gap)
- Flexzone not paired with its slot — confusing which direction it belongs to
- Adding per-direction fields (seq, timestamp) grows the parameter list
- Inconsistent return types (bool vs None)
- "SlotFrame" naming inconsistent — processor uses InSlotFrame/OutSlotFrame, others use SlotFrame
- Inbox `seq` not exposed to scripts (cannot detect message loss)

---

## 2. Design: Direction Objects + Inbox Message

Group data-loop parameters by direction. Inbox gets its own message struct
(inbox is peer-to-peer messaging, not a pipeline direction).

### 2.1 Script-level signatures (Python / Lua)

```python
on_produce(tx, msgs, api) -> bool         # tx.slot, tx.fz
on_consume(rx, msgs, api) -> bool         # rx.slot, rx.fz
on_process(rx, tx, msgs, api) -> bool     # rx.slot, rx.fz, tx.slot, tx.fz
on_inbox(msg, api) -> bool                # msg.data, msg.sender_uid, msg.seq
on_init(api) -> None                      # unchanged
on_stop(api) -> None                      # unchanged
```

- `rx` = receive direction (slot is read-only; flexzone is mutable per HEP-0002)
- `tx` = transmit direction (slot and flexzone both writable)
- `msg` = inbox message (typed payload + sender identity + sequence)
- All data callbacks return bool. Consumer/inbox loops ignore the return value
  for now but the plumbing is ready for future flow control.

### 2.2 Flexzone mutability (HEP-0002 design)

Flexzone is bidirectional by design (HEP-0002 TABLE 1). Both producer and
consumer get mutable access. User-managed coordination via SharedSpinLock
and/or atomics in the flexzone struct. Therefore:

- `InvokeRx.fz` is `void*` (NOT `const void*`)
- `InvokeTx.fz` is `void*`

Only the **slot** is read-only on the input side (`InvokeRx.slot` is `const void*`).

### 2.3 C++ invoke interface

Use structs at the invoke boundary. Zero runtime cost (stack-allocated, compiler
inlines field access). Improves readability and extensibility — adding a field
changes the struct, not every virtual method signature.

```cpp
/// Input direction — data received from upstream.
/// Slot is const (system-managed via SlotRWState).
/// Flexzone is mutable (user-managed coordination per HEP-0002).
struct InvokeRx
{
    const void *slot{nullptr};
    size_t      slot_size{0};
    void       *fz{nullptr};       // mutable — bidirectional by design
    size_t      fz_size{0};
};

/// Output direction — data going downstream.
struct InvokeTx
{
    void  *slot{nullptr};
    size_t slot_size{0};
    void  *fz{nullptr};
    size_t fz_size{0};
};

/// Inbox message — one-shot peer-to-peer delivery.
/// No flexzone, no ring buffer. Any role can send to any inbox.
struct InvokeInbox
{
    const void *data{nullptr};     // typed payload (valid until next recv_one())
    size_t      data_size{0};
    std::string sender_uid;        // sender's role UID
    uint64_t    seq{0};            // sender's monotonic sequence number
};
```

Virtual method signatures:

```cpp
virtual InvokeResult invoke_produce(InvokeTx tx, std::vector<IncomingMessage> &msgs) = 0;
virtual InvokeResult invoke_consume(InvokeRx rx, std::vector<IncomingMessage> &msgs) = 0;
virtual InvokeResult invoke_process(InvokeRx rx, InvokeTx tx, std::vector<IncomingMessage> &msgs) = 0;
virtual InvokeResult invoke_on_inbox(InvokeInbox msg) = 0;
```

### 2.4 rx/tx/msg construction in data loops

```cpp
// Producer
InvokeTx tx{out_buf, out_sz, fz_ptr, fz_sz};
auto result = engine_->invoke_produce(tx, msgs);

// Consumer
InvokeRx rx{data, item_sz, fz_ptr, fz_sz};
auto result = engine_->invoke_consume(rx, msgs);

// Processor
InvokeRx rx{held_input, in_sz, in_fz_ptr, in_fz_sz};
InvokeTx tx{out_buf, out_sz, out_fz_ptr, out_fz_sz};
auto result = engine_->invoke_process(rx, tx, msgs);

// Inbox (in drain_inbox_sync)
InvokeInbox msg{item->data, inbox_queue->item_size(), item->sender_id, item->seq};
engine->invoke_on_inbox(msg);
```

### 2.5 Script-level objects

Each engine builds the script-visible objects from the C++ structs:

**Python**: lightweight Python object with named attributes (`tx.slot`, `tx.fz`,
`msg.data`, `msg.sender_uid`, `msg.seq`). Built per-invoke using cached schema
types. No overhead beyond what make_slot_view already does.

**Lua**: table with named fields (FFI cdata for slot/fz). Same pattern.

**Native C**: struct passed by pointer through the C native engine API.

---

## 3. Directional Naming — C++ / Engine Level

### 3.1 Slot type names

All roles use directional names at the C++ engine level:

| Role | Input | Output |
|------|-------|--------|
| Producer | — | OutSlotFrame |
| Consumer | InSlotFrame | — |
| Processor | InSlotFrame | OutSlotFrame |

### 3.2 Flexzone type names

| Role | Input | Output |
|------|-------|--------|
| Producer | — | OutFlexFrame |
| Consumer | InFlexFrame | — |
| Processor | InFlexFrame | OutFlexFrame |

### 3.3 Script-level aliases

For producer/consumer convenience, the engine creates non-directional aliases
in `build_api_()` (after the role is known from `ctx.role_tag`):

- Producer: `SlotFrame` = `OutSlotFrame`, `FlexFrame` = `OutFlexFrame`
- Consumer: `SlotFrame` = `InSlotFrame`, `FlexFrame` = `InFlexFrame`
- Processor: no aliases — both directions are explicit

Implementation: in `build_api_()`, after all types are registered and the role
is known. Python builds a second ctypes class with the alias name. Lua registers
an FFI typedef (e.g., `typedef OutSlotFrame SlotFrame;`). The alias is created
only for single-direction roles. Aliases are NOT created in `register_slot_type()`
because the engine doesn't know the role at registration time.

---

## 4. Directional Field Layout — All Layers

Every layer uses the same `in_`/`out_` prefix convention. No undirected `fz_spec`,
`has_fz`, `slot_spec`, `packing` — only directional names exist.

### 4.1 RoleHostCore (base class)

Replace the single `fz_spec_` / `has_fz_` / `schema_fz_size_` with directional pairs:

```cpp
class RoleHostCore
{
    // ── Schema (set once during init) ───
    SchemaSpec in_fz_spec_;
    SchemaSpec out_fz_spec_;
    size_t     in_schema_fz_size_{0};
    size_t     out_schema_fz_size_{0};

    // Accessors
    void set_in_fz_spec(SchemaSpec spec, size_t fz_size) noexcept;
    void set_out_fz_spec(SchemaSpec spec, size_t fz_size) noexcept;
    bool has_in_fz()  const noexcept { return in_fz_spec_.has_schema; }
    bool has_out_fz() const noexcept { return out_fz_spec_.has_schema; }
    const SchemaSpec &in_fz_spec()  const noexcept;
    const SchemaSpec &out_fz_spec() const noexcept;
    size_t in_schema_fz_size()  const noexcept;
    size_t out_schema_fz_size() const noexcept;
};
```

Old `set_fz_spec()`, `has_fz()`, `fz_spec()`, `schema_fz_size()` are **removed**.
All 28 call sites across producer/consumer/processor migrate to directional names.

### 4.2 Role host members (per-role)

Each role host stores directional schema specs as members (same as slots):

| Field | Producer | Consumer | Processor |
|-------|----------|----------|-----------|
| `in_slot_spec_` | empty | filled | filled |
| `out_slot_spec_` | filled | empty | filled |
| `in_fz_spec_` | empty | filled | filled |
| `out_fz_spec_` | filled | empty | filled |

Producer already has `slot_spec_` → rename to `out_slot_spec_`.
Consumer already has `slot_spec_` → rename to `in_slot_spec_`.
Processor already has `in_slot_spec_` / `out_slot_spec_` → add `in_fz_spec_` / `out_fz_spec_`.

### 4.3 Config fields (per-role)

| Config key | Producer | Consumer | Processor |
|------------|----------|----------|-----------|
| `in_slot_schema` | — | optional | required |
| `out_slot_schema` | required | — | required |
| `in_flexzone_schema` | — | optional | optional (NEW) |
| `out_flexzone_schema` | optional | — | optional |

`ProcessorFields` adds `in_flexzone_schema_json` (already done in code).

### 4.4 Data loop flexzone wiring

All roles construct `InvokeRx` / `InvokeTx` with directional fz pointers:

```cpp
// Producer
void *out_fz = core_.has_out_fz() ? out_producer_->write_flexzone() : nullptr;
InvokeTx tx{out_buf, out_sz, out_fz, out_fz_sz};

// Consumer
void *in_fz = core_.has_in_fz()
    ? const_cast<void*>(in_consumer_->read_flexzone()) : nullptr;
InvokeRx rx{data, item_sz, in_fz, in_fz_sz};

// Processor
void *in_fz = core_.has_in_fz()
    ? const_cast<void*>(in_consumer_->read_flexzone()) : nullptr;
void *out_fz = core_.has_out_fz()
    ? out_producer_->write_flexzone() : nullptr;
InvokeRx rx{held_input, in_sz, in_fz, in_fz_sz};
InvokeTx tx{out_buf, out_sz, out_fz, out_fz_sz};
```

Flexzone is mutable on both sides (HEP-0002 TABLE 1). The `const_cast` on the
read side is required because `Consumer::read_flexzone()` returns `const void*`
but the flexzone is user-managed shared memory with bidirectional access.

### 4.5 Engine type registration

Startup callback registers (no role-specific branching):
- `InSlotFrame` if `in_slot_spec.has_schema`
- `OutSlotFrame` if `out_slot_spec.has_schema`
- `InFlexFrame` if `in_fz_spec.has_schema`
- `OutFlexFrame` if `out_fz_spec.has_schema`
- `InboxFrame` if `inbox_spec.has_schema`

---

## 5. EngineModuleParams Update

```cpp
struct EngineModuleParams
{
    ScriptEngine *engine{nullptr};
    RoleHostCore *core{nullptr};

    std::string tag;
    std::filesystem::path script_dir;
    std::string entry_point;
    std::string required_callback;

    // Directional schemas — consistent across all roles.
    SchemaSpec  in_slot_spec;       // Consumer, Processor
    SchemaSpec  out_slot_spec;      // Producer, Processor
    SchemaSpec  in_fz_spec;         // Consumer, Processor  (NEW)
    SchemaSpec  out_fz_spec;        // Producer, Processor  (was fz_spec)
    SchemaSpec  inbox_spec;
    std::string in_packing{"aligned"};
    std::string out_packing{"aligned"};

    RoleContext role_ctx;
    std::string module_name;
};
```

Startup callback registers (no role-specific branching):
- `InSlotFrame` if `in_slot_spec.has_schema`
- `OutSlotFrame` if `out_slot_spec.has_schema`
- `InFlexFrame` if `in_fz_spec.has_schema`
- `OutFlexFrame` if `out_fz_spec.has_schema`
- `InboxFrame` if `inbox_spec.has_schema`

---

## 6. Return Value Contract

All data callbacks return `InvokeResult`:

| Callback | Return | Script contract | Loop behavior |
|----------|--------|----------------|---------------|
| on_produce | Commit/Discard/Error | True→Commit, False→Discard | publish or discard slot |
| on_consume | Commit/Discard/Error | True→Commit, False→Discard | **ignored** (reserved) |
| on_process | Commit/Discard/Error | True→Commit, False→Discard | publish or discard output |
| on_inbox | Commit/Discard/Error | True→Commit, False→Discard | **ignored** (reserved) |

**All callbacks must return True or False explicitly.** Returning `None` (Python)
or `nil` (Lua) is treated as a script error. This is a breaking change from the
old consumer/inbox convention where `None` was the expected return.

---

## 7. Migration

This is a **breaking change** to all script callback signatures.

```python
# Producer — before / after
def on_produce(out_slot, fz, msgs, api):       # old
def on_produce(tx, msgs, api):                 # new (tx.slot, tx.fz)

# Consumer — before / after
def on_consume(in_slot, fz, msgs, api):        # old, returned None
def on_consume(rx, msgs, api):                 # new, must return True/False

# Processor — before / after
def on_process(in_slot, out_slot, fz, msgs, api):   # old, no input fz
def on_process(rx, tx, msgs, api):                   # new (rx.fz available)

# Inbox — before / after
def on_inbox(slot, sender_uid, api):           # old, returned None
def on_inbox(msg, api):                        # new (msg.data, msg.sender_uid, msg.seq)
```

---

## 8. Implementation Order

1. Define `InvokeRx` / `InvokeTx` / `InvokeInbox` structs in `script_engine.hpp` (DONE)
2. Update `ScriptEngine` virtual methods (DONE)
3. Static code review — check all docs and code for inconsistencies (DONE)
4. Update `RoleHostCore`: replace `fz_spec_`/`has_fz_`/`schema_fz_size_` with
   directional `in_fz_spec_`/`out_fz_spec_` + accessors. Remove old API.
5. Update role host members:
   - Producer: `slot_spec_` → `out_slot_spec_`, add `out_fz_spec_` tracking
   - Consumer: `slot_spec_` → `in_slot_spec_`, add `in_fz_spec_` tracking
   - Processor: add `in_fz_spec_`/`out_fz_spec_` members
6. Add `in_flexzone_schema_json` to `ProcessorFields` (DONE)
7. Update `EngineModuleParams` with directional fz fields (DONE)
8. Update engine implementations (Python, Lua, Native):
   - Update invoke method signatures to use InvokeRx/InvokeTx/InvokeInbox (DONE)
   - New type registration cases: InFlexFrame, OutFlexFrame
   - Remove "SlotFrame" / "FlexFrame" registration paths → InSlotFrame/OutSlotFrame
   - Add alias logic for producer/consumer
   - Build rx/tx/msg script objects in invoke methods
   - Expose msg.seq for inbox
9. Update role host data loops:
   - Construct InvokeRx/InvokeTx with directional fz pointers (partially done)
   - Replace all `core_.has_fz()` → `core_.has_in_fz()` / `core_.has_out_fz()`
   - Replace all `core_.fz_spec()` → `core_.in_fz_spec()` / `core_.out_fz_spec()`
   - Replace all `core_.schema_fz_size()` → directional
   - Wire processor input flexzone in data loop
10. Update all tests
11. Update HEP-0011, HEP-0015, HEP-0018 callback documentation
