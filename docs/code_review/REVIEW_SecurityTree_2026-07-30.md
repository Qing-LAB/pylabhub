# REVIEW — Security tree (shm / zmq / curve), completion pass

**Status:** ✅ **READ COMPLETE** — every file in scope has been read.
S-1..S-10 ✅ FIXED; S-11 ❌ OPEN (LOW, robustness, fix not yet approved).
**Date:** 2026-07-30. **Task:** #92.
**Scope:** the ~14,500 lines under `src/utils/security/`, `src/utils/hub/`,
`src/include/utils/security/`.

> Transient review doc (`docs/DOC_STRUCTURE.md §1.7`). Open items also feed
> `docs/todo/PLATFORM_TODO.md` and `AUTH_TODO.md`. When every item is ✅,
> fold lasting design corrections into the owning HEPs and archive per §2.2.

---

## Why this pass exists

An earlier pass the same day was **signal-driven** — residue markers, compiler
warnings, grep for known-bad shapes — not a read. It found real problems (see
"Already closed" below) but covered roughly 8,500 of ~14,500 lines, and two of
its findings were **wrong** in ways that mattered:

- **F4** reported `validate_curve_factory_params` as a cleanup-commit leftover
  and proposed adding the "missing" checks. Forty lines below the signature
  the function explains that an empty serverkey is a **legal factory-time
  state** (HEP-CORE-0036 §6.7 Standby), enforced later by `is_configured()`.
  The proposed fix would have broken production.
- The `-Wdangling-else` site: the compiler's complaint was repeated as "a
  logic hazard". Reading it showed the brace ambiguity was noise (gtest guards
  it) and the real defect was different — a conditional assertion that let the
  test pass when it had verified nothing.

Both were caught only by opening the file. Hence: **read, do not infer.**

### Method note carried from the earlier pass

Making an invalid state *unconstructable* out-found reading it. The CURVE
`PLH_PANIC` located a plaintext ROUTER in `datahub_broker_workers.cpp` — a
file in an unrelated suite that no amount of reading the inbox sources would
have surfaced. Deleting `PeerAllowlist::unrestricted` named 5 more sites at
compile time. Prefer a structural refusal over an audit item where the shape
allows it; it keeps working after the reviewer stops looking.

### Shapes worth hunting (all confirmed present in this codebase)

1. `if (X.empty()) → skip security`. Produced both backdoors closed today.
   Variants seen: guard on an identity name, a peer pubkey, a roster, an
   allowlist.
2. **Unreachable + documented as supported + pinned by a test asserting it
   works.** That combination is why both backdoors survived every prior
   review. `PeerAllowlist::unrestricted` cited an `--allow-anonymous-data`
   operator flag that never existed — a doc describing an intention that
   outlived its plan.
3. Validators that do not validate what they are named for (#68 family).
4. Tests using a production bypass for convenience.
5. **A rationale that covers less than the check it justifies** (see S-1).

---

## Findings

Severity: **HIGH** = exploitable or key-material exposure · **MED** = weakens a
stated contract · **LOW** = correctness/clarity, no security consequence.

| # | Sev | File | Status |
|---|---|---|---|
| S-1 | HIGH | `key_file_acl.cpp:139-172` | ✅ FIXED 2026-07-31 |
| S-2 | MED | `vault_crypto.cpp:184` | ✅ FIXED 2026-07-31 |
| S-3 | MED | `shm_capability_channel.cpp:432-442` | ✅ FIXED 2026-07-31 |
| S-4 | LOW | `shm_capability_channel.cpp:119` | ✅ FIXED 2026-07-31 |
| S-5 | MED | `attach_protocol.hpp:231` | ✅ FIXED 2026-07-31 |
| S-6 | LOW | `secure_subsystem.hpp:272-289` | ✅ FIXED 2026-07-31 |
| S-7 | LOW | `hub_zmq_queue.cpp:339` + `hub_inbox_queue.cpp` | ✅ FIXED 2026-07-31 |
| S-8 | MED | `hub_zmq_queue.cpp:1813-2093` | ✅ FIXED 2026-07-31 |
| S-9 | LOW | `hub_zmq_queue.cpp:2041-2068` | ✅ FIXED 2026-07-31 |
| S-10 | MED | `key_file_acl.cpp` + `vault_crypto.cpp` + `hub_vault.cpp` | ✅ FIXED 2026-07-31 |
| S-11 | LOW | `hub_zmq_queue.cpp:472-498` | ❌ OPEN |

---

### S-11 ❌ LOW — the send thread's retry loop ignores the shutdown signal it was handed

`src/utils/hub/hub_zmq_queue.cpp:472-498`, `ZmqQueueImpl::run_send_thread_`.

The thread body has two nested loops with **different** exit conditions:

```cpp
while (!ctx.shutdown_requested())          // outer — honours the ThreadManager signal
{
    ...
    while (socket)                          // inner — EAGAIN retry
    {
        auto sr = socket.send(..., dontwait);
        if (sr.has_value()) break;
        if (send_stop_.load(...)) { ++send_drop_count_; break; }   // only this flag
        ++send_retry_count_;
        std::this_thread::sleep_for(send_retry_interval_ms_);
        continue;                           // unbounded
    }
}
```

The inner loop checks `send_stop_` but **not** `ctx.shutdown_requested()`, and has
no retry ceiling. `run_recv_thread_` checks both (`:307`), so this is an
asymmetry between the two thread bodies rather than a house convention.

**Why it does not hang today, and why that is the weak part.** Every teardown
path reaches `ZmqQueue::stop()`, which sets `send_stop_` *before* calling
`thread_mgr_->drain()` — and `~ZmqQueue` calls `stop()`. So the flag the inner
loop does check is always set first. Correctness therefore rests on `stop()`'s
internal ordering rather than on the loop honouring the signal it is passed.
Reorder those two statements in `stop()`, or reach a drain by any route that
does not go through `stop()`, and a producer retrying against a blocked peer
never observes shutdown.

That is precisely the state `stop()`'s own diagnostic anticipates:

> "if this fires, the thread body is stuck in a libzmq op that ignored the stop
> flag."

**Not filed higher than LOW** because no live path reaches it: I could not
construct one where `drain()` runs without `send_stop_` already set. It is a
latent robustness gap, recorded so the next person changing `stop()`'s ordering
knows what depends on it.

**Fix direction (not applied — needs approval).** Have the inner loop test the
same pair the outer one does, and give the retry a ceiling so a permanently
blocked peer surfaces as a drop with a diagnostic rather than as a thread that
looks busy forever.

---

### Observation — `ShmQueue` already has the state model S-8 wanted

Not a finding; recorded because it is the convergence target for the other two
queue classes.

`ShmQueue` has **no `running_` flag at all**:

```cpp
bool ShmQueue::is_running() const noexcept
{
    return pImpl && (pImpl->dbc.get() != nullptr || pImpl->dbp.get() != nullptr);
}
```

State is *derived from the resource* — a DataBlock is attached, or it is not.
There is no separate boolean that can be left stale, so the S-8 failure is not
merely fixed here, it is **unrepresentable**: a half-started `ShmQueue` cannot
claim to be Active, because claiming to be Active requires actually holding the
thing.

`ZmqQueue`, `InboxQueue` and `InboxClient` each carry
`std::atomic<bool> running_` and are guarded by scope guards (S-8). The guards
are correct, but they defend a representation that still spells "Active" and
"died halfway through starting" with the same bit. `ShmQueue` shows the shape
that removes the question.

---

### S-10 ✅ FIXED — MED — HEP-0035 §4.6.1's write recipe was prose, typed out three times, and had drifted

`key_file_acl.cpp` (`atomic_write_owner_only_file`), `vault_crypto.cpp`
(`write_secure_file`), `hub_vault.cpp` (`publish_public_key`).

§4.6.1 states the rule for writing a protected file in a sentence:

> use `open(O_CREAT | O_EXCL, 0600)` followed by an explicit
> `fchmod(fd, 0600)` at write time.  Do NOT rely on the process `umask`.

Three call sites each implemented that sentence by hand.  The POSIX bodies
were near-identical — same flags, same EINTR write loop, same
close-and-unlink rollback, the same error wording down to the quoted HEP
section.  The only *intended* difference was the mode: 0600 for the vault,
0644 for `hub.pubkey`.  `KeyFileRole` already encoded exactly that
difference.

**Two unintended differences had crept in, and both were in the wrong
direction:**

| | mode | overwrite | `fsync` | Windows |
|---|---|---|---|---|
| `atomic_write_owner_only_file` | 0600 | replace (tmp+rename) | **yes** | DACL + `MoveFileExW` |
| `write_secure_file` (**the vault**) | 0600 | refuse | **no** | `ofstream(trunc)` |
| `publish_public_key` | 0644 | replace (unlink+create) | **no** | its own |

1. **The vault was the least durable file in the system.** Only
   `atomic_write_owner_only_file` called `fsync`.  Without it a crash or
   power cut can leave a correctly-named vault whose data pages never
   reached the disk — and a vault holds the only copy of an identity
   keypair.  The file with the most to lose had the weakest guarantee.
2. **`write_secure_file`'s refuse-to-overwrite did not exist on Windows.**
   The POSIX branch enforced it with `O_EXCL` and threw a message citing
   "atomic O_EXCL guard, HEP-CORE-0035 §4.6.1".  The Windows branch was
   `std::ofstream(path, trunc)` — it silently overwrote.  The guarantee the
   error message advertised was platform-specific and the code did not say so.

Neither was a decision.  Both are the ordinary outcome of a rule that has
to be retyped to be obeyed.

**What shipped.** One implementation, `security::write_keyfile(path,
contents, role, policy)`, in the module that already owns `KeyFileRole` and
the canonical modes — because "what mode should this file have" and "how do
I write it" must agree, and keeping them in one file is the only way to
guarantee they do.

- `role` supplies the mode from the §4.6.1 table.  `ConfigFile` /
  `ConfigFileReferencingVault` have no canonical mode (the operator owns
  those) and throw rather than inventing one.
- `policy` is `Refuse` (vault: never silently clobber) or `Replace`
  (`hub.pubkey`, `known_roles.json`).
- All three writers now `fsync` before close.
- `publish_public_key` gains atomic replace: the old unlink-then-create left
  a window with no `hub.pubkey` at all, and a re-keygen that failed mid-write
  left it that way.
- `atomic_write_owner_only_file` stays as a thin forwarder — `known_roles.json`
  is an owner-only *data* file, not a `KeyFileRole`, and borrowing `VaultFile`
  to reach the same 0600 would be a lie at the call site.
- Windows `Refuse` is an existence check, not atomic.  Stated in the API doc
  and in the HEP rather than left implicit; closing it needs
  `CreateFileW(CREATE_NEW)` (#120).  It is still strictly stronger than the
  branch that truncated without checking.

Net: ~90 lines deleted from `hub_vault.cpp`, ~95 from `vault_crypto.cpp`,
and `set_owner_only_permissions` — the path-based `chmod` helper that S-2 was
about — became dead and was removed.

HEP-0035 §4.6.1 updated: the recipe now names its single implementation, and
records why the single-implementation rule exists.

**The first attempt at this fix broke something — recorded deliberately.**

Deleting `publish_public_key`'s ~90 hand-written lines also deleted the
`std::filesystem::remove` that preceded them, and with it the stderr note the
CLI emitted when a pre-existing `hub.pubkey` was about to be overwritten.
The full sweep caught it in both configurations:

```
PlhHubCliTest.KeygenEmitsNote_WhenPreExistingPubkeyRemoved (Failed)
```

That test exists for exactly this. Its own comment:

> This test pins the emission: a pre-planted hub.pubkey + `--keygen` must
> surface the note. **A regression that silently drops the emission would be
> invisible otherwise.**

The lesson is about what a mechanism change is allowed to take with it. The
note answers an operator's question — *"was something already sitting there?"*
— about **federation trust material**. That question does not depend on
whether the replacement happens by unlink-then-create or by `rename(2)`. The
unlink was mechanism; the note was contract. Removing the mechanism silently
removed the contract along with it.

Restored, reworded from "was removed" to "will be atomically replaced" so the
text matches what now happens. The test's assertions were left alone — they
pin the three operator-facing substrings, not the mechanism — but its comments
described the unlink and were corrected, with a note that the signal must
survive future rewordings of the mechanism.

---

### S-8 ✅ FIXED — MED — a key removed after construction crosses a `noexcept` boundary

`src/utils/hub/hub_zmq_queue.cpp:1813-2093`, `ZmqQueue::start()`.

`start()` sets `running_ = true` at line 1813, *before* it does any work, then
enters a `try` block that ends with exactly two handlers:

```
catch (const std::invalid_argument &e)   // line 2071
catch (const zmq::error_t &e)            // line 2085
```

Inside that block it reaches into the KeyStore:

```cpp
pImpl->socket.set(zmq::sockopt::curve_publickey, ks.pubkey(pImpl->identity_key_name_));
```

`KeyStore::pubkey` throws **`std::out_of_range`** when the name is not present
(`key_store.cpp:347`, and again at `:351` when the entry exists but carries no
public half). `std::out_of_range` derives from `std::logic_error` — *not* from
`std::invalid_argument`. Neither handler catches it.

The panic guard added at line 1874 does not cover this. It proves
`identity_key_name_` is **non-empty**; it says nothing about whether that name
is actually **in the store**. Those are different failures, and only the first
one is guarded.

**Failure scenario.** A queue is built naming a key that IS present, and the
key is then removed from the KeyStore before `start()` runs — `KeyStore::remove`
is public API. `start()` then runs:

1. `running_` ← `true`
2. `ks.pubkey("role-x")` throws `std::out_of_range`
3. the exception escapes both handlers and leaves `start()`

The cleanup those handlers perform — `socket.close()`, `mechanism_ ←
Uninitialized`, `running_ ← false` — **never runs**. The queue is left with:

- `running_ == true` — it looks Active to every observer
- `mechanism_ == Uninitialized` — never advanced to `Curve`
- a socket constructed but never bound and never connected
- `zap_handle_` unregistered (that block is below the throw)

The caller, `apply_master_approval`, wraps the call in `catch (const
std::exception &)` and returns `false` — so the *immediate* call is reported as
a failure, correctly. The damage is what happens next.

**The retry is the sharp edge.** `start()` opens with:

```cpp
if (pImpl->running_.load(std::memory_order_acquire))
    return true; // already running — idempotent
```

`running_` is still `true` from the aborted attempt. So the second `start()`
returns **`true` immediately**, having bound nothing, connected nothing, and
negotiated nothing. A caller that retries after fixing the key gets a success
report for a queue that will never carry a byte. The idempotence check cannot
tell "already started" from "died halfway through starting" because both states
are spelled `running_ == true`.

**It is worse than a wedged queue — it aborts the process.**

`ZmqQueue::finalize_connect` is declared **`noexcept override`**
(`hub_zmq_queue.hpp:534-536`) and tail-calls `start()`
(`hub_zmq_queue.cpp:1638`). That is the deferred-connect path for fan-in
DIALING PUSH — the one HEP-CORE-0036 §6.6.3 routes every fan-in producer
through. So on that path the escaping `std::out_of_range` does not merely skip
cleanup: it crosses a `noexcept` boundary, and the runtime calls
`std::terminate`.

**Correction — the severity claim was first written as HIGH on a premise the
regression test then disproved.** The original write-up said a mistyped key
name in config would take the process down. It will not:
`validate_curve_factory_params` calls `ks.has(name)`
(`hub_zmq_queue.cpp:742`), so `push_to` / `pull_from` return **nullptr** for an
absent key and the queue is never built. The first version of the S-8 test
asserted the queue would construct, and failed — which is how the wrong premise
surfaced.

The reachable trigger is therefore narrower than claimed: the key must go
missing *between* construction and `start()`. That window is real (`remove` is
public and the two calls are not atomic with respect to each other) and the
consequence when it opens is still `std::terminate`, so the fix stands — but
the severity is **MED**, not HIGH. Recorded rather than quietly amended: the
claim shipped in this document and in `API_TODO.md` before it was checked.

**What shipped.** Both halves, because the guard alone still lets the exception
cross the `noexcept` boundary:

1. **`pylabhub::basics::make_scope_guard`** (the existing helper in
   `utils/scope_guard.hpp`, not a new one) installed immediately after the
   `running_.exchange(true)`, owning `socket.close()` + `mechanism_ ←
   Uninitialized` + `running_ ← false`. Dismissed on the one path that
   completes a start. The three duplicated cleanup blocks — two `catch`
   handlers and the `!worker_started` branch — collapse into it. A guard cannot
   be defeated by a throw whose type nobody enumerated, which is exactly how
   this bug survived.
2. **A terminal `catch (const std::exception &)` + `catch (...)`**, so `start()`
   is non-throwing *in fact* rather than by convention — which is what its
   `noexcept` caller already assumed.

The two specific handlers stay, because they still carry better diagnostics;
they just no longer carry the cleanup.

Verified: Debug 2725/2725, Release 2722/2722.

---

### S-9 ✅ FIXED — LOW — the CURVE engagement guard checks what was *configured*, not what was *negotiated*

`src/utils/hub/hub_zmq_queue.cpp:2041-2068`.

The comment states the guard's purpose plainly:

> After all CURVE setsockopts and bind/connect have completed, **ask libzmq
> directly what mechanism this socket negotiated.**

That is not what `ZMQ_MECHANISM` reports. In `third_party/libzmq/src/options.cpp:1159`:

```cpp
case ZMQ_MECHANISM:
    if (is_int) {
        *value = mechanism;
        return 0;
    }
```

`options.mechanism` is a **local configuration field**, set by
`set_curve_key` / `ZMQ_CURVE_SERVER` when the socket was configured. It is
never written by the handshake. Reading it back returns the value this process
just wrote.

The timing makes this unambiguous: `connect()` in libzmq is asynchronous and
returns before any TCP connection exists, let alone a completed ZMTP/CURVE
handshake. At line 2050 there is no peer and no negotiation to report on.

The guard is still worth keeping — it does verify that the CURVE setsockopts
took effect rather than being silently ignored, which is a real regression class.
But it cannot detect a failed or downgraded handshake, and the comment claims it
can. In a codebase where comments are treated as contract, that gap invites
someone to lean on a guarantee that was never there.

**What shipped.** The comment now says what the check actually does —
configuration, verified locally, before any peer exists — cites
`options.cpp:1159` for why, notes that `connect()` is asynchronous so there is
nothing negotiated yet, and states plainly that it cannot detect a failed or
downgraded handshake. The guard itself is unchanged and kept: proving the CURVE
setsockopts took effect rather than being silently ignored is a real regression
class. Observing an actually-negotiated mechanism is the socket-monitor work
tracked as **#93** (`ZMQ_EVENT_HANDSHAKE_SUCCEEDED` /
`ZMQ_EVENT_HANDSHAKE_FAILED_*`), not a getsockopt.

---

### S-1 ✅ FIXED — HIGH — a *writable* vault parent directory is only a warning

`src/utils/security/key_file_acl.cpp:139-172`, `append_parent_dir_warning`.

**What the code does.** When verifying a vault FILE, it also stats the parent
and tests `(pmode & 0077) != 0`. For **every** bit in that mask it appends
advisory text and leaves `ok = true`.

**The justification, quoted in the comment** (HEP-CORE-0035 §4.6.2):

> "parent dir leak is recoverable; some operators want group-readable parents
> for shared host setups"

**Why that is insufficient.** The rationale covers the READ bits. The mask
`0077` also covers `0020` (group-write) and `0002` (world-write). Permission
to replace a directory entry comes from **the directory, not the file**: with
a writable parent, any such user can `rename(2)` or `unlink` + recreate
`hub.<uid>.vault`, substituting the hub's identity key outright. The file's
own `0600` is irrelevant to that operation.

So a rationale about **visibility** is being used to downgrade a
**replacement** vector to advisory text.

**Corroborating detail.** `verify_vault_dir` (`:217-263`) correctly HARD-FAILS
on `0077` for the vault directory itself. Only the parent-of-file path softens
it — the two disagree about the same class of exposure.

**Fix direction (not applied).** Split the mask: keep group/world *read* as
the documented advisory, make group/world *write* an error. This contradicts a
documented HEP decision as literally written, so it needs an owner ruling +
a HEP-CORE-0035 §4.6.2 amendment, not a quiet code change.

---

### S-2 ✅ FIXED — MED — the vault read follows symlinks; the write refuses to

`src/utils/service/vault_crypto.cpp:184` — `read_file()` is a plain
`std::ifstream`.

**The asymmetry.** The WRITE path is meticulous: `O_CREAT | O_EXCL |
O_NOFOLLOW | O_CLOEXEC` at `:122`, and again for the temp-and-rename at
`:636`, with a comment explaining that `O_NOFOLLOW` closes the
symlink-redirect attack. The read gives that guarantee up.

`verify_vault_file` compounds it by using `stat`, not `lstat` — so the ACL
check follows the link too, validating the **target's** mode and owner, and
cannot report that it was handed a symlink at all.

**Bounded by.** The target must still be `0600` AND owned by the effective
uid, so this is not "point it at an attacker's file". The realistic shape is
narrower: pointing the hub at a *different* file we own — e.g. another hub's
vault on a shared host. **S-1 is what makes planting the link feasible**, so
the two should be judged together.

**Same class, same file family — `set_keyfile_mode` (`key_file_acl.cpp:547`)**
applies `::chmod(path, 0600)` by PATH, which also follows symlinks: a planted
link means the mode is applied to the target instead. Folded here rather than
raised separately, because it is reachable only under the same preconditions.

**Fix direction (not applied).** Open with `O_NOFOLLOW` and read from the fd,
mirroring the write path; `fchmod` on that fd instead of `chmod` on the path;
consider `lstat` in `verify_vault_file` so the check can say "this is a
symlink" rather than silently validating the target. **The pattern already
exists in-tree** — `vault_crypto.cpp` deliberately uses `fchmod(fd, ...)` as a
belt-and-braces guard against a pathological umask, so this is adopting a
local convention, not inventing one.

---

### S-3 ✅ FIXED — MED — a failed peer-credential read becomes "uid 0", which passes when the hub runs as root

`src/utils/security/shm_capability_channel.cpp:432-442`,
`MemfdProducer::accept_one`.

**The chain.**

1. `AcceptedPeer result{}` — zero-initialized, so `uid`/`gid`/`pid` are `0`.
2. `if (::getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0)
   { ...fill result... }` — there is **no else**. A failed credential read is
   swallowed and the fields keep their zero defaults.
3. `attach_protocol.cpp:207` — `if (peer->uid != expected_uid_) throw`.
4. `expected_uid_` is documented as "Deployment's uid"
   (`attach_protocol.hpp:146`).

**The gap.** The in-code justification says:

> "SO_PEERCRED failure leaves the fields at zero defaults.  The L2 auth layer
> (task #250) does an explicit equality check against the expected uid; zero
> default fails closed for any non-root expectation."

It is accurate and it names its own limit. When the deployment runs as **root**,
`expected_uid_ == 0`, and a **failed** SO_PEERCRED also yields `uid == 0` — so
the values match and the check **passes on a credential read that never
succeeded**. A syscall error is silently converted into one specific, and in
that deployment privileged, identity.

**Preconditions — why MED and not HIGH.** Requires both (a) the hub deployed as
root, and (b) `getsockopt(SO_PEERCRED)` actually failing, which is rare on a
healthy AF_UNIX socket. Neither is exotic on its own — a system-service hub is
an ordinary deployment — but together they are unlikely. Severity is about
likelihood here, not about the shape, which is the same silent-downgrade
pattern as the two backdoors closed today.

**Fix direction (not applied).** Treat the failure as a failure: capture
`errno` at the call site (the file's own stated discipline, `:80`) and either
throw, or carry an explicit `credentials_valid` flag on `AcceptedPeer` that the
L2 check must consult. Do not encode "unknown" as a value that can equal a
legitimate uid.

---

### S-4 ✅ FIXED — LOW — the memfd is not sealed against shrinking; a peer can SIGBUS the other side

`src/utils/security/shm_capability_channel.cpp:119` —
`::memfd_create("plh_shm_capability", MFD_CLOEXEC)`, without
`MFD_ALLOW_SEALING`, so no seal can be applied afterwards.

Both sides `mmap` the same memfd and each holds a writable fd to it.  Nothing
prevents either from calling `ftruncate` to shrink it after the other has
mapped it — and touching mapped pages beyond the new end raises **SIGBUS**, not
an error return.  A buggy peer therefore crashes its counterpart, with a signal
the process cannot meaningfully recover from mid-access.

`F_SEAL_SHRINK` exists precisely for this and requires `MFD_ALLOW_SEALING` at
creation, so the mitigation is a creation-time flag plus one `fcntl` — it
cannot be retrofitted onto an existing memfd.

**Why LOW.** The trust boundary here is same-uid by design (kernel-enforced via
the mode-0700 `XDG_RUNTIME_DIR` directory, plus `SO_PEERCRED`), so this is "a
misbehaving peer can crash you", not a privilege boundary crossing.  Recorded
because the codebase treats unexplained crashes as real defects (cf. #75
SIGSEGV, #93/#242), and this is a crash class that no amount of caller
discipline in the victim can prevent.

**Already half-noticed.** The FreeBSD porting notes at `:869` mention
`MFD_ALLOW_SEALING` as "not used here but worth knowing if a future hardening
pass wants to **seal writes**".  The write case is the less interesting one;
the shrink/SIGBUS case is not named anywhere.

---

### S-7 ❌ LOW — the frame-size cap bounds input bytes, not what msgpack will allocate

`src/utils/hub/hub_zmq_queue.cpp:339` (`run_recv_thread_`) and the equivalent
call in `hub_inbox_queue.cpp::recv_one` both do:

```cpp
msgpack::unpack(static_cast<const char *>(msg.data()), msg.size());
```

with no `unpack_limit` argument.  `third_party/msgpack-c`'s defaults
(`v1/unpack_decl.hpp:89-95`) are `0xffffffff` for **array, map, str, bin, ext
AND depth** — effectively unlimited.

**What the existing cap does and does not do.**  Both call sites correctly
reject oversized frames BEFORE unpacking (`msg.size() >= max_frame_sz_`), so
the input BYTES are bounded to roughly one schema frame.  But a msgpack
container header declares its element count independently of how many bytes
follow: five bytes encoding `array32` with count `0xFFFFFFFF` is a valid,
tiny frame that asks the parser to allocate for four billion objects.  The
allocation is attempted before the parser discovers the buffer is short.

**Bounded by, and why LOW.**  The resulting `std::bad_alloc` is caught by the
surrounding handler, so this is a transient allocation spike, not a crash and
not memory corruption.  More importantly the sender must already hold an
admitted CURVE identity — post-#90/#91 there is no unauthenticated path to
this code at all — so it is an authenticated role attacking its own hub, which
the trust model does not defend against.  Recorded as defence-in-depth, not as
a live exposure.

**What shipped.**  Not an extra argument at each call site — a new owning entry
point, `wire_detail::decode_frame(data, size, max_payload_fields)`, returning a
`DecodedFrame`.  There were **three** raw `msgpack::unpack` sites, not two
(`hub_zmq_queue.cpp:340`, `hub_inbox_queue.cpp:600`, `:1088`), and all three
are now migrated.  The header already called itself *"the single source of
truth for the 5-tuple frame"*, but decode escaped it; this closes that.

The limits are derived from the format rather than picked:

| Axis | Bound | Why |
|---|---|---|
| array | `max(kFrameTupleSize, max_payload_fields)` | the outer frame is the 5-tuple; the payload is one element per schema field |
| map, ext | **0** | the frame format contains neither |
| str, bin | `size` | a blob cannot exceed the buffer it was read from — exact, not conservative |
| depth | `kMaxFrameDepth` (4) | outer array → payload array, plus headroom |

`DecodedFrame` bundles the `object_handle` with the `FrameEnvelope` because the
envelope's `recv_tag` / `payload` / `checksum` are **views into the zone the
handle owns**.  Held as two locals, keeping the handle alive for exactly as long
as the envelope is read was an unwritten rule every call site had to know; now
the lifetime is structural.  Moving a `DecodedFrame` is safe — the zone is
heap-allocated, so the addresses do not change.

Two library facts were verified rather than assumed, since the fix depends on
both:

- The limit check **precedes** the allocation (`unpack.hpp:114` checks
  `n > limit.array()`, `:124` then allocates `n * sizeof(msgpack::object)`), so
  the bound genuinely prevents the allocation rather than merely reporting it
  afterwards.  A 40-byte frame declaring `0xffffffff` elements was a ~68 GB
  request on a 64-bit host.
- With a null reference function msgpack **copies** every str/bin/ext into its
  own zone (`unpack.hpp:173-177`) rather than pointing at the caller's buffer —
  so a `DecodedFrame` owns its bytes and may outlive the message it was read
  from.  That property is now stated in the doc comment.

Verified: Debug 2725/2725, Release 2722/2722.

---

## Resolution — what shipped 2026-07-31

Debug 2725/2725 (2723 + the two new S-1 tests).  Release verified separately.

- **S-1** — parent-dir mask split by consequence: write bits ERROR, read bits
  advisory.  **HEP-CORE-0035 §4.6.2 amended FIRST** — the code was faithfully
  implementing what the rule literally said, so changing only the code would
  have left design and implementation disagreeing and invited a "correction"
  back.  Two new tests cover the ERROR path, which previously had none.
- **S-2** — vault read probes with `O_NOFOLLOW` and names a symlink
  explicitly; `set_keyfile_mode` uses `fchmod` on an `O_NOFOLLOW` fd so it can
  only chmod the thing it looked at.
- **S-3** — failed `SO_PEERCRED` now throws.  Brings the transport in line with
  `ShmAttachOrchestrator`, which already denies on any cannot-ascertain answer.
- **S-4** — `MFD_ALLOW_SEALING` at creation + `F_SEAL_SHRINK|F_SEAL_GROW` once
  sized.
- **S-5** — default REMOVED, not flipped.  The compiler then named **8** call
  sites, six more than grep had found, every one silently on the
  unauthenticated path.  Flipping to `true` would have changed what those
  eight tests exercise without anyone noticing.
- **S-6** — doc rewritten to name the false-NEGATIVE direction; the two
  consequential unchecked call sites now state why their trigger is
  unreachable, so the absence of a check reads as deliberate.

### The S-1 fix was wrong on the first attempt — recorded deliberately

The first version tested only `mode & 0022` and therefore rejected `/tmp`
(01777), failing 10 L2 tests in one run.  **The sticky bit is precisely the
mechanism that closes the substitution vector the rule exists to catch** — on
such a directory only the file's owner, the directory's owner, or root may
rename or unlink.  Reasoning about directory write permission without
accounting for `S_ISVTX` produced a rule that was strict rather than correct.

This is the same failure mode as F4 earlier in this review (and as the
`-Wdangling-else` misread): reasoning about a mechanism without checking its
exceptions, where the exception was already written down — forty lines below a
signature, inside a gtest macro, and in POSIX directory semantics
respectively.

`VaultFile_ParentDir_Writable_WithSticky_IsAccepted` now pins the carve-out, so
the rule cannot be "simplified" back to a plain mask test.  That test matters
more than the fix: the mistake is now something the suite refuses, not
something a comment asks people not to make.

---

## Coverage ledger

Honest accounting — "not read" means exactly that, and no conclusion should be
drawn about those files from this document.

| File | Lines | Read | Notes |
|---|---|---|---|
| `security/curve_keypair.cpp` | 156 | ✅ full | Clean. Z85 validation correct (length + alphabet; `\0` is not in the Z85 alphabet, so a validated key cannot be all-zero, which makes `empty()`'s sentinel sound). `CurveKeypair::secret_z85` is a plain `std::string` — unlocked heap, not zeroed on destroy — but that is documented and owned by #102; stack buffers ARE zeroed. |
| `security/key_file_acl.cpp` | 696 | ✅ full (POSIX) | S-1 found. Otherwise careful: `verify_ownership` compares against `geteuid()` (correct for setuid — `chmod`/`open` use the effective uid); `keyfile_inside_base_dir` canonicalizes BOTH sides before a component-wise prefix check and refuses a degenerate empty base, so `base/../../etc/x` cannot vacuously "contain"; `verify_public_key_file` intentionally skips the mode check (pubkeys are distributable) but still stats to catch the common wrong-path mistake. Windows is a documented no-op, scoped by HEP-CORE-0035 §4.6 to UNIX mode bits with encryption-at-rest as the primary protection there — a stated decision, not a gap. |
| `service/vault_crypto.cpp` | — | 🚧 read/write paths only | S-2 found. POSIX write path is genuinely careful (`O_EXCL`+`O_NOFOLLOW`+`fchmod` belt-and-braces against a pathological umask). |
| `security/secure_subsystem.cpp` | 707 | ✅ ~620 | No findings. Bringup is a singularity CAS + PANIC on `sodium_init`, `setrlimit(RLIMIT_CORE,0)` or `prctl(PR_SET_DUMPABLE,0)` failure — hardening that cannot silently not-happen. Crypto wrappers check every libsodium return, bound every output buffer, and null-check every pointer. **Two deliberate softenings examined and judged sound, NOT flagged** (see observations below). |
| `security/shm_capability_channel.cpp` | 1089 | ✅ ~900 (Linux backend full) | S-3 found. S-3 + S-4 found; otherwise this is the most carefully written file read so far. Bind: `renameat2(RENAME_NOREPLACE)` atomic non-clobbering bind (#321), `chmod 0700` BEFORE the rename so the target never exists at a looser mode, and a probe-then-refuse-if-live gate that names the same-uid two-hosts-one-channel race SO_PEERCRED cannot catch, refusing conservatively on any unrecognised errno. Consumer: rejects `MSG_CTRUNC` (multi-fd SCM_RIGHTS, #276), validates cmsg level/type/len, checks `fd >= 0`, `fstat`s and rejects a zero-size segment, closes the fd on every error path. EINTR retry on both `sendmsg` and `recvmsg`, each with a written rationale. NOT read: the non-Linux stub bodies (~180 lines of throw-with-message + porting notes). |
| `security/attach_protocol.cpp` | 771 | ✅ full | S-5 found (in the header). Acceptor + consumer-initiate paths read: boundary validation throws on programmer error but returns nullopt for the legitimate "endpoint not there yet" startup race; FdGuard RAII on every throw path; shared handshake deadline (#318/#319) so the whole exchange is bounded rather than each step; AttachProtocolTimeout → nullopt so the H3a race retries. The Frame 1/2/3 crypto is **sound** and is the strongest code in this review: nonce AND challenge freshly generated per handshake before Frame 1 (so a captured `challenge_response_b64` cannot be replayed); `role_type` whitelisted to consumer/observer; the observer branch compares the presented pubkey against the broker-published observer key with `memcmp_ct` and refuses when no accessor or no key is installed rather than waving it through; every cipher length checked before decrypt; both verifications compute `verified = (decoded == kChallengeBytes) && memcmp_ct(...)` so neither short-circuits past the compare; plaintext `memzero`d on both sides. Mutual auth, WHEN ENABLED, is complete — fresh consumer nonce/challenge, Frame 3 timeout treated as affirmative authentication failure rather than a retry, broker-supplied `producer_pubkey_z85` equality, constant-time proof compare. S-5 is about the DEFAULT only, not the mechanism. Non-Linux is `#error`, not a silent stub — the structural-refusal pattern this review recommends elsewhere. |
| `security/known_roles.cpp` | 326 | ✅ full | No findings. Strict parsing throughout: `require_string_or_empty` rejects a present-but-wrong-type field instead of silently coercing (`nlohmann::value(k, default)` returns the default for BOTH absent and wrong-type, which had masked `"pubkey_z85": null`); `validate_entry` refuses empty uid, empty pubkey, and any length != 40. The HEP-CORE-0036 §I10 one-pubkey-per-uid invariant correctly treats same-uid replacement as rotation rather than a duplicate. Its compile-time bypass is handled properly — see the observation below. |
| `security/attach_channel_shm.cpp` | 193 | ✅ full | No findings. Length-prefixed framing with `kMaxAttachFrameBytes` enforced on BOTH send and recv, zero-length rejected, and the `std::vector<char> body(len)` allocation happens only AFTER the cap check — so a hostile length prefix cannot drive an allocation. `recv_all_until`/`send_all` re-evaluate the deadline every iteration, continue on EINTR, and treat `recv()==0` as an explicit peer-closed-mid-frame error rather than a short read. |
| `security/shm_attach_orchestrator.cpp` | 243 | ✅ full | No findings — the best fail-closed reasoning in the tree; see the observation below. |
| `hub/hub_zmq_queue.cpp` | 2680 | ✅ full | S-7 + S-8 + S-9 + S-11 found. Read: the CURVE arm, `validate_curve_factory_params`, `connect_one`, and `apply_master_approval` (the trust entry point — it consumes broker-supplied JSON and configures identity + allowlist). That function validates array shape, fan-out cardinality (HEP-0017 §3.3.0 SUB has exactly one PUB), per-entry object-ness, `pubkey_z85` length 40, and DIALING-side endpoint presence; Standby fields are only filled when currently empty, so an Active queue is not mutated. The deferred-connect logic for fan-in DIALING PUSH correctly anticipates the ZAP race (connect before the peer's allowlist is seeded → terminal DENY) — the same class of bug as the inbox hang fixed earlier today. `start()` and `stop()` now read, no findings. `start()` puts the Standby gate BEFORE the `running_` exchange so a refused start can be retried, resolves the ZAP domain with explicit → instance_id → name@address fallbacks, seeds a deny-all allowlist ONLY when nothing populated one (clobbering would drop REG_ACK's `initial_allowlist` and leave an authenticated PUSH deny-all), and registers the domain BEFORE bind so an early peer connect cannot hit an unregistered domain and be denied while admission is in fact configured. `stop()` drains threads first, grace-polls detached ones for 5s, and logs honestly that a subsequent `~ZmqQueueImpl` would UAF a runaway thread rather than pretending otherwise. `finalize_connect` read — polls the readiness oracle with an explicit deadline and a cancellation hook, and treats `PermanentError` as fatal rather than retrying forever. `read_acquire`/`read_release`/`write_acquire` read — the ring copy is into a pre-allocated buffer (no per-item heap churn), and both overflow policies count a failed cycle into `data_drop_count_` rather than dropping silently. Recv thread read — checks both stop conditions, bounds the frame before decode, tracks ring overflow rather than dropping silently. Send thread read — S-11. Topology dispatch read: the fan-out SUB branch refuses an empty serverkey at the factory instead of deferring to `start()`, and its ordering `assert` is belt-and-braces over `start()`'s unconditional CURVE panic (the assert compiles out under NDEBUG; the panic does not). |
| `hub/hub_shm_queue.cpp` | 869 | ✅ full | **No findings.** State is derived from the DataBlock pointer rather than a separate flag (see observation) — structurally immune to the S-8 class. Handles are real RAII (`~SlotWriteHandle` calls `release_write_handle`; the class doc sanctions destruction as a release path), so the early-return paths in `read_acquire`/`write_acquire` hold a slot until the next acquire but never leak it. `stop()` releases outstanding handles before tearing down the DataBlock, and clears the borrowed capability fd without closing it — correct, the L1 transport owns it. Both topology factories refuse fan-in with a reason citing the physical constraint rather than a bare error. |
| `security/zap_router.cpp` | 781 | 🚧 partial | `pump_one`, domain register/unregister, `ZapPumpThread` read during the teardown work. |

---

### S-5 ✅ FIXED — MED — producer authentication is OFF by default at the API, ON by default in config

`src/include/utils/security/attach_protocol.hpp:231`:

```cpp
initiate_consumer_handshake(..., bool require_mutual_auth = false);
```

`src/include/utils/config/startup_config.hpp:46`:

```cpp
bool shm_require_mutual_auth{true};
```

**The two layers disagree about the safe default, and the API picks the unsafe
one.** With the flag false the consumer keeps the 2-frame flow: it never
verifies the producer's `producer_pubkey_z85` and never checks the
challenge-response proof, so it accepts an SCM_RIGHTS fd and `mmap`s shared
memory from a producer whose identity was never established.

**Production is NOT affected — verified.** `role_api_base.cpp:1886` passes
`pImpl->shm_require_mutual_auth` explicitly, sourced from
`config.startup().shm_require_mutual_auth`, which defaults true.  So the
shipped path authenticates.

**Why it is still worth fixing.** The dangerous option is what you get by
omission.  Both existing test callers
(`test_shm_attach_orchestrator.cpp:220`, `:558`) omit the argument and
therefore exercise the UNauthenticated flow — not as a deliberate scenario,
but because the parameter has a default.  Any future caller that forgets it
silently loses producer authentication, with nothing failing and nothing
logged.

**The stated justification looks like residue.** The header explains the false
default as "backward compatible with pre-#262 producers", and the same story is
baked into the Frame-3-timeout error text (`attach_protocol.cpp:702`: "either
the producer is a pre-#262 build that does not support mutual auth, or the peer
is not the real producer") — so an operator hitting a genuine impostor is
offered a benign explanation first.  Producer and
consumer are both pylabhub roles built from the same tree and shipped
together, and the project frame is explicitly no-partial-deployments /
no-grace-periods.  So the compatibility case the default exists to serve is one
the project says it does not support — the same shape as
`PeerAllowlist::unrestricted` citing an `--allow-anonymous-data` flag that
never existed (#91).

**Fix direction (not applied).** Flip the default to `true`, or better, remove
the default entirely so every call site states its intent — the two test
callers then declare whether they mean to test the unauthenticated flow.
Removing the default is the compile-time-refusal shape that worked for #91.

---

## Observations — examined, deliberate, NOT findings

Recorded so a later reader does not re-raise them, and so the reasoning is on
file if the surrounding assumptions ever change.

- **`kEnforceUniquePubkey` — a security relaxation for tests, done RIGHT.**
  The HEP-CORE-0036 §I10 one-pubkey-per-uid invariant is compiled out in
  DEBUG + `PYLABHUB_WITH_TEST` to let L3 in-process multi-BRC fixtures share a
  keypair.  That is the same *category* of thing as S-5, and it is worth
  studying because it is handled correctly where S-5 is not:
    1. The relaxation is a **compile-time constant**, so it cannot be flipped
       at runtime and cannot be reached in a shipped binary.
    2. `known_roles_enforces_unique_pubkey()` exposes the disposition, and
       `I10_BuildFlag_MatchesNDebugDisposition` asserts it MATCHES the build
       configuration — so a CI job accidentally built with the test flag fails
       loudly instead of quietly relaxing security.
    3. `I10_Add_DuplicatePubkey_DifferentUid_RejectedOrAllowed` branches on
       that accessor and asserts the **throw** in enforcing builds, so the
       enforcement path is genuinely exercised — by the Release sweep, which
       this project runs.
  Contrast with S-5: there the relaxation is a **runtime default argument**,
  reachable in production by omission, with no drift guard and with both test
  callers silently landing on the weak path.  The fix direction for S-5 is
  essentially "make it look like this one".

- **`apply_master_approval` uses raw `nlohmann::value()` where `known_roles.cpp`
  deliberately stopped doing so.**  `hub_zmq_queue.cpp:1425-1427` reads
  broker-supplied peer entries with `entry.value("pubkey_z85", std::string{})`.
  `value()` returns the default BOTH when a field is absent AND when it is
  present with the wrong type — the exact hazard `known_roles.cpp` fixed with
  `require_string_or_empty`, whose comment records the concrete bug it caused
  there (`"pubkey_z85": null` and `"name": 1234` silently coerced to `""`).
  **Not raised as a finding, because the consequences are absorbed:** a
  wrong-typed `pubkey_z85` becomes `""`, fails the `size() != 40` check, and
  the whole ACK is refused; a wrong-typed `endpoint` becomes `""` and is
  refused on the DIALING side where it is required, and is genuinely unused on
  the BINDING side; `role_uid` is optional metadata by contract.  So every
  dangerous case is caught by a LATER check rather than by the parse.
  Worth knowing anyway: the safety here is incidental, not designed — it holds
  because the fields that matter happen to have length or emptiness
  constraints.  A future field without one would be silently coerced.  One
  file in this codebase learned this lesson explicitly; this one has not.

- **The two `stop()` implementations order ZAP-unregister vs socket-close
  oppositely.**  `ZmqQueue::stop` releases the ZAP registration FIRST, with the
  rationale written down ("after this, no more handshakes route to `this`");
  `InboxQueue::stop` closes the socket first and resets the handle after.  Both
  are safe — closing the socket also stops new handshakes, and
  `DomainRoutingTable::unregister_domain` blocks until in-flight admission
  callbacks return — so this is a consistency note, not a defect.  Recorded
  because only one of the two carries the reasoning, and a future reader
  comparing them could reasonably think one is wrong.
  S-3, in the same subsystem.**  Every path where the answer is not a clear
  YES resolves to denial, and each one says so: `broker_query` throwing →
  `DeniedTransportFail`; returning `nullopt` → `DeniedTransportFail`, logged as
  "broker authority cannot be ascertained"; a status that is neither `success`
  nor `denied` → fail closed as a protocol violation; `cache_lookup` throwing →
  treated as cache-denied while the broker's verdict still governs.  Cache /
  broker divergence is logged in BOTH directions as pipeline-health
  observability rather than silently reconciled, and the `FdGuard` closes the
  peer on every non-`Sent` return.
  One layer down, `MemfdProducer::accept_one` takes the opposite approach to an
  indeterminate answer: a failed `SO_PEERCRED` becomes `uid = 0` and flows on
  (S-3).  Same subsystem, same authors, opposite instinct — which is why S-3 is
  worth fixing even though its trigger is unlikely: the codebase already knows
  the right pattern.

- **`SecureSubsystem` gates only `keys()` on `Initialized`; every other
  primitive is ungated** (softened 2026-07-07, rationale in-file at `:135-162`).
  It rests on libsodium self-initializing on first use, which holds in
  production because the mod pack runs bringup before any consumer. The
  softening was to unbreak Layer-0 tests that legitimately call
  `generate_uuid4()` outside a LifecycleGuard. Sound as written; the
  precondition to watch is that it would stop holding if a primitive were ever
  called concurrently from multiple threads BEFORE bringup, since libsodium's
  implicit init is not documented as thread-safe.
### S-6 ✅ FIXED — LOW — `compute_blake2b_array`'s doc describes its risk in the wrong direction

`src/include/utils/security/secure_subsystem.hpp:272-289`.

**Initially logged as an "observation, not a finding" in this document.  That
was under-called** — checking the 8 call sites changed the assessment.  Kept
LOW because the trigger is unreachable, but it is a finding, not a note.

**The behaviour.** Failure returns all-zeros.  Of the 8 callers, none check.
The most consequential is `wire_envelope.cpp:39`
(`compute_envelope_hash_impl`), which produces the `envelope_hash` enforcing
`I-ENVELOPE-BODY-BINDING` — HEP-CORE-0046's tamper contract, "mismatch =
ENVELOPE_TAMPERED, message dropped".

Were the hash ever to fail there, the tamper check **fails OPEN**: the sender
stamps all-zero, the receiver recomputes all-zero, and the two agree.  A
failure on the receiver alone is worse — any forged envelope carrying an
all-zero stamp verifies.  `schema_utils.hpp:276/:298`, `schema_blds.hpp:230`
and `compute_inbox_schema_tag` have the same shape: every input would tag
identically, so mismatch detection stops discriminating.

**Not reachable, and that is load-bearing.** `scratch.data()` is
`std::string::data()`, which never returns null even when empty, so the
`data == nullptr` path cannot fire here; and `crypto_generichash` with a
constant 32-byte outlen has no practical failure mode.  **No live defect.**

**The actual defect is the documentation.**  The header justifies the weak
marker like this:

> "All-zeros can also be a LEGITIMATE hash output for a specific input;
> treating it as a failure marker is intentionally weak."

That is a **false-positive** framing — the worry that a real hash gets mistaken
for a failure.  Producing 32 zero bytes from BLAKE2b-256 is a preimage attack
(~2^256); it will not happen, so the stated concern is negligible.

The concern the doc never states is the **false-negative**: a failure becomes a
value that compares EQUAL across different inputs, so every downstream equality
check silently passes.  A maintainer consulting this header to decide whether
checking matters is pointed at the harmless direction.

**Fix direction (not applied).** Rewrite the rationale to name the real
consequence — "if this ever fires, every equality check on the result agrees,
so integrity checks fail open" — and note at the `wire_envelope` and schema-tag
sites why the trigger is unreachable there (`std::string::data()` is non-null),
so the absence of a check is visibly deliberate rather than an oversight.

---

## Already closed by the earlier pass (context, not open items)

Recorded so this document reads as one story rather than implying these are
still live.

- **Unarmed CURVE built a working plaintext socket** — the arm was guarded on
  `if (!identity_key_name.empty())`. Now `PLH_PANIC` at `InboxQueue::start`,
  `InboxClient::start`, `ZmqQueue::start`. Caught 16 tests across two files
  standing up unauthenticated ROUTERs. (#90)
- **`PeerAllowlist::unrestricted`** — one bool that made `contains()` admit
  every identity. DELETED, so it is a compile error rather than a runtime
  abort. Named 5 more sites in `zap_router_workers.cpp`. (#91)
- **Federation keyless peer** — a peer configured without a pubkey is wired
  PLAIN and connected. NOT a live hole: federation is unfinished scaffolding
  with no app and no config surface, and the contract was never settled.
  Annotated at the site and folded into #69 item (7) so the design does not
  inherit the shape.
- Inbox delivery defects (unbounded send, discard-mode message loss, ACK
  correlation, `overflow_policy: "block"` selecting unbounded memory) — see
  HEP-CORE-0027 §3.7/§3.8 and `MESSAGEHUB_TODO` Recent Completions.
