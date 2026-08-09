# Named-key operations — closing the courier gap in the security module

**Status:** plan, not started. The design lands in HEP-CORE-0043 §2.5;
this document is the how, the order, and what to settle first.

**Why it exists:** a review found secret bytes held outside the security
module in several places. They are not several bugs — they are one API
gap with several symptoms. This records the finding, the fix, the
sequence, and honestly, the ways this plan could still fail.

> **Restructured after five review passes.** Each pass added a section
> and the document became an accretion: sections out of order, `2.2a`
> beside `2.2` saying the same thing, and the caveat that questions the
> whole approach buried near the end. Content is unchanged; the order now
> matches what a reader picking this up cold actually needs.

---

## 1. What is wrong, in one paragraph

The security module can do a job *for* you, or it can hand you a key so
you can do the job yourself. For public-key operations it does the job —
you name a key, it encrypts. For symmetric operations it does not: there
is no way to say "encrypt this under the key called X", so every caller
needing symmetric encryption must fetch the key first. Fetching means
holding secret bytes in ordinary memory the OS may write to disk. Several
callers do this, and none of them had a choice.

---

## 2. Settle these two before writing any code

Both change what the work *is*. Starting step 4 without them means
building the wrong thing carefully.

### 2.1 The payload format is the root cause, and it constrains the fix

The vault payload is a JSON document and the private key is a value
inside it. **A JSON string node holds a `std::string`, so the secret must
become unwiped heap on the way in and on the way out.** No care at the
call site changes that; the format requires it. §3.2 traces four such
allocations during a single vault creation.

Two ways out:

- **Keep the secret out of the JSON.** Payload becomes non-secret
  metadata (uid, public key) plus a raw key section. **This changes the
  at-rest format** — see the consequence in §6.
- **Never let the secret reach the caller.** Creation goes through
  `keys().generate_and_add_identity(name)`, which already exists and
  already returns only the public half; the file is written by reading
  inside `with_seckey` straight into the output buffer, the same shape as
  arming a socket. **This leaves the format untouched.**

The second is preferable and is the one consistent with the module's
promise. It costs a restructure of `create`.

**✅ DECIDED — the second, and the on-disk bytes do not move at all.**
Confirmed by reading both sides of the format:

```
on disk today:   [ nonce(24) ][ MAC(16) ‖ ciphertext ]
   vault_write        assembles exactly this
   vault_read_secure  parses exactly this
```

That is **byte-for-byte what `secretbox_encrypt_using` produces** when it
generates the nonce and prepends it — which is the shape §2.5.3 already
settled on for its own reason (a caller cannot reuse a nonce it cannot
supply). The two decisions happen to agree.

Consequence, and it removes the biggest risk in the plan: a vault written
by the old code opens under the new code and vice versa, because they
write identical bytes. **Step 4's fixture-vault check becomes a
straightforward compatibility test rather than a migration**, and the
contradiction flagged in §6 dissolves — there is no format change to
reconcile.

### 2.2 The strategy depends on completeness, and completeness has failed

**This plan finds and fixes copies one at a time. Five review passes each
turned up one more** — the stack key, the `std::string` couriers, the
write-side payload, the four-copy create chain. Each pass believed the
one before it had finished. There is no reason to think the next would
be the last.

The defence that does not need a complete audit is
`mlockall(MCL_CURRENT | MCL_FUTURE)`: one call, locks every page the
process ever allocates, covers the copies nobody found. HEP-CORE-0035
§4.7.2's first measure asks for that outcome, and the implementation
delivers it only for KeyStore allocations via `sodium_malloc` inside
`LockedKey`. `mlockall` appears nowhere in the tree.

**It is almost certainly wrong here, and the reason is worth recording so
nobody proposes and re-rejects it.** This is a data-acquisition framework
built on large shared-memory blocks, and `MCL_FUTURE` locks those too —
either exhausting `RLIMIT_MEMLOCK` or pinning gigabytes of sample data to
protect a few dozen bytes of key. Region-scoped `mlock` is what
`sodium_malloc` already does, which is why "get the secret into the key
store" is the answer rather than a process-wide switch.

**Consequence for how to spend effort:** prefer the steps that make a
whole *class* of copy impossible — closing the raw-key doors (step 7),
generating straight into the key store (step 6) — over the ones that fix
known sites individually. Only the former survive an incomplete audit.

---

## 3. What the review found

Each of these was read, not inferred.

### 3.1 The vault key is a plain stack array

`vault_crypto.cpp` derives the vault key with Argon2id into
`std::array<uint8_t, 32>` — an ordinary local. It is wiped by a scope
guard, which is the right instinct, but wiping is not the issue: **the
memory is not locked, so the OS can page it to disk while it is live.**
Wiping RAM afterwards does not unwrite a swap block.

`vault_derive_key` also returns the array by value, and named-return-value
optimisation is not guaranteed, so a copy may be left in the callee's
frame unwiped.

**Why it is written that way:** the safe type could not be used.
`SecureBuffer<N>` wipes on destruction — but its move constructor is
deleted, so it cannot be returned from a function. A function whose whole
job is to produce a key and hand it back therefore *cannot* use it. That
is the clearest evidence the problem is the shape of the API, not the
diligence of the author.

### 3.2 The create path is a chain of four copies

Tracing a freshly generated private key through `RoleVault::create`:

| # | Where it lands | Wiped? |
|---|---|---|
| 1 | `CurveKeypair::secret_z85` — the struct is two `std::string`s | no |
| 2 | `const std::string sec_str = std::move(kp.secret_z85)` — moved, so the same allocation, still an unwiped `std::string` | no |
| 3 | `json payload = {…, {"secret_key", sec_str}}` — nlohmann copies it into a JSON string node | no |
| 4 | `payload.dump()` — a third string, handed to `vault_write` | no |
| 5 | `pImpl->secret_z85` — the fixed array | **yes**, in the destructor |

Only the last is handled. The other four are freed without wiping, so the
bytes remain in released heap until something reuses it.

The same shape appears on the hub side, and `hub_vault.cpp`'s save path
builds one more `std::string` copy to assemble its payload. Note what
this contradicts: both `Impl` structs carry a comment saying their
`std::string` members were replaced by fixed arrays *"whose destructors
are not guaranteed to zero heap memory"* — and the pattern survives in the
functions on either side of them.

All of it traces back to §2.1.

### 3.3 The config loaders are couriers

`role_config.cpp` calls `vault.secret_key()` and `hub_config.cpp` calls
`vault.broker_curve_secret_key()`; each receives a `string_view` onto the
secret and passes it to the KeyStore. The window is short and nothing is
copied, but the caller is in the chain of custody for no reason — the
vault could deposit the identity itself.

### 3.4 The vault object holds the secret for its whole lifetime

`RoleVault::Impl` and `HubVault::Impl` keep the secret in fixed-size
members and wipe them in the destructor — the good half. But the
allocation is ordinary heap inside a `unique_ptr`, **not locked**, so it
can be paged out while the object is alive. Depositing into the key store
is only a real fix if the secret never lands in a member on the way there.

### 3.5 What was already right

Recorded so nobody "fixes" it:

- **Socket arming** already has the shape we want.
  `arm_curve_server(sock, name)` takes a name; the secret is read inside
  a callback and goes straight into the socket option.
- **Reading a vault** was already fixed. `vault_read_secure` decrypts
  into a caller-supplied `SecureBuffer` span, and its own comment says
  *"no `std::string` materializes."* Both callers use it. Writing never
  got the same treatment — that asymmetry is §2.1.
- **Wiping discipline is good everywhere.** The gap is page-locking, not
  wiping.
- **`box_*_using` is the model.** It already names its key; the symmetric
  side simply never got the same treatment.

### 3.6 One honest limit

libzmq's socket options take raw key bytes, so arming a socket must hand
the secret over. `with_seckey` narrows that to a callback with no copy,
which is as small as the window gets, but it is a real export and no API
design removes it.

---

## 4. The fix

Nine operations, defined in HEP-CORE-0043 §2.5; summarised:

```
keys().add_random_key(name, byte_count)
keys().add_key_from_password(name, password, scope)
keys().replace_key_from_password(name, password, scope)

secretbox_encrypt_using(name, plaintext, out)
secretbox_decrypt_using(name, sealed, out)

save_encrypted_file(path, payload, key_name)
load_encrypted_file(path, key_name)
open_file_with_password(path, password, scope, key_name)

RoleVault::load_identity_into(key_name)
HubVault::load_identity_into(key_name)
```

**Decisions already taken, not open for re-litigation:**

- **The symmetric operations take no nonce.** It is generated inside and
  written into the output, so a caller cannot reuse one. `box_*_using`
  keeps its explicit nonce because a protocol frame's layout is shared
  with another implementation; a sealed blob's is not.
- **A derived key lives for the process.** One password derivation at
  startup; saves afterwards are immediate. `replace_key_from_password`
  covers a password change without a restart.
- **`add_` throws on an existing name; `replace_` is separate.** Silently
  overwriting an identity key would be catastrophic and hard to notice.
- **No new guard types.** `SecureBuffer`, `LockedKey`, `with_seckey` and
  `Z85PublicKey` already exist. The one that failed here failed because a
  non-movable type cannot be returned — and the fix is to stop returning
  secrets, not to build a better container for returning them.
- **A scope-bound key handle is deferred.** Nothing here needs it;
  everything here is process-lifetime. It belongs with the ephemeral
  capability grant.

---

## 5. Order of work

> **Status 2026-08-09 (commit `512fe1d5`).** Steps 1-4 are shipped and the
> §3.1 stack key — the defect that started this — is closed.  Steps 5-7
> remain, and step 7 is still the one that decides whether any of it
> holds: steps 1-6 move callers, they do not remove the ability.
>
> Two findings worth carrying, both from doing the work rather than
> planning it:
>
> - **The vault now requires SMS to be `Initialized`**, because its key
>   lives in the KeyStore.  59 tests were creating vaults with no
>   lifecycle at all and began aborting on the `keys()` gate.  They were
>   exercising a configuration production cannot have; they now run the
>   module.  Any remaining step that moves a secret into the KeyStore
>   should expect the same class of fallout and budget for it.
> - **The at-rest format did not move.**  `secretbox_encrypt_using`
>   emits exactly the layout `vault_crypto` used to build by hand, so
>   the migration-fixture question raised under decision (a) never
>   arose for this step.  It still will for step 6, which changes what
>   is *inside* the payload.


Seven steps, each leaving the tree green and independently reviewable.

| Step | Change | Removes |
|---|---|---|
| 1 | `add_random_key` ✅ **DONE** | the mint-on-stack-then-`add_raw` pattern in the admin session seal |
| 2 | `secretbox_*_using` ✅ **DONE** | the two `lookup_raw` spans in `admin_session.cpp` — **NOT yet migrated**; the operations exist and the vault uses them, but `admin_session` still hand-assembles the identical `[nonce ‖ MAC ‖ ct]` blob around the raw-key form.  Carried below. |
| 3 | `add_key_from_password` + `replace_key_from_password` ✅ **DONE** | — (prerequisite for 4) |
| 4 | migrate `vault_crypto` ✅ **DONE** | **the stack key (§3.1) — CLOSED.**  The file operations did NOT need to become new SMS methods: `vault_write` / `vault_read_secure` already were the file layer, so they took a `key_name` instead of a password and the key left the file entirely.  Fewer new methods than the plan assumed. |
| 5 | re-express the vault tests off the secret accessors | — (prerequisite for 6) |
| 6 | `load_identity_into` on both vaults; restructure `create` onto `generate_and_add_identity` + `with_seckey`; delete the `secret_z85` members and the secret accessors | **the couriers (§3.3)**, the four-copy chain (§3.2), the unlocked member (§3.4) |
| 7 | make the raw-key `secretbox_*` private; decide `lookup_raw` | **the ability to reintroduce any of it** |

Steps 1-2 prove the shape on a live consumer before the vault depends on
it. Step 4 carries the real risk — it touches the at-rest read and write
paths.

### Step 1 — done, and one design choice worth recording

`KeyStore::add_random_key(name, byte_count)` shipped; the admin
session-seal key now uses it.

**The choice that mattered: a naive implementation would have recreated
the problem inside the module.** `LockedKey`'s only constructor took a
source span — safe, because it wipes that source, but a caller still has
to materialise the bytes *somewhere* first, and that somewhere is
ordinary memory the OS may page out before the wipe runs. Implementing
`add_random_key` on top of it would have moved the exposure window from
`admin_session.cpp` into `key_store.cpp` and called it fixed.

So `LockedKey` gained a **fill-in-place constructor**: allocate the
locked region, then have the CSPRNG write directly into it. There is no
source buffer at any point, so there is nothing to wipe and no window at
all. That is the difference between relocating a problem and removing
one, and it is the shape the remaining steps should follow.

**Verified:** mutation-checked — deleting the `randombytes_buf` call
makes the test fail on the all-zeroes assertion, which is the assertion
that exists for exactly that. Absence check on the migrated caller: no
`add_raw`, no key-bearing buffer, no key `memzero`. One `random_bytes`
call remains there for the *nonce* — that is step 2's target, not step
1's residue.

### Why step 5 exists as its own step

**Ten test sites** across `test_role_vault.cpp` and `test_hub_vault.cpp`
use the two secret accessors, and they are not lazy tests:

| What the test proves | Why it currently needs the secret |
|---|---|
| The vault file is genuinely encrypted | searches the raw bytes for the secret and expects **not found** |
| Create and open yield the same keypair | compares the secret across two objects |
| The keypair survives a reopen | stores the secret, reopens, compares |

Deleting the accessor removes the ability to write the first one as
framed. The resolution is **not** a test-only accessor — that is a
production surface existing for tests, which this project rejects. It is
to re-express each assertion:

- **Encrypted-at-rest:** search the raw bytes for the **public** key. If
  the payload were plaintext the pubkey would appear too, and the pubkey
  is not secret. Same proof, no secret needed.
- **Round-trip and persistence:** call `load_identity_into(name)` and
  compare `keys().pubkey(name)`. Better than the current test — it
  exercises the production deposit path rather than an accessor that
  would otherwise exist only for tests.

Doing this before step 6 keeps the deletion mechanical and the test
change reviewable on its own.

### Why step 7 decides whether any of it holds

Steps 1-6 migrate every *caller* off the fetch-a-key pattern. They do not
remove the *ability*. Leave the raw-key entry points public and the next
person writes a new courier, with the API inviting them to.

The asymmetric side shows the intended end state: there is **no public
raw-key `box_encrypt`** — only `box_encrypt_using`. The symmetric side
never got the same treatment, and that omission is the root of every
symptom in §3.

| Entry point | Production callers after step 6 | Disposition |
|---|---|---|
| `secretbox_encrypt` / `secretbox_decrypt` (raw key span) | none | **Make private.** The `_using` variants call them internally, exactly as `box_*` already does. |
| `keys().lookup_raw(name)` | none | **Needs a decision.** No production consumer, which makes it surface existing only for tests — but removing it removes the only way to read a raw secret, and two `key_store` tests drive it directly. Retiring it is a contract handoff, not a deletion. |

**Do not skip step 7 and call the arc finished.** An API that still offers
the unsafe door has two ways to do one job, and the unsafe one is shorter
to type.

---

## 6. How each step gets verified

**Not by a green suite.** Every change here is invisible to behaviour:
the same bytes get written and read either way. A passing test proves
nothing about whether a secret stopped being copied.

- **Absence is the pass condition.** After each migration, grep the file
  for `lookup_raw`, the secret accessors, and `std::string` holding a
  secret — and **read the hits**. Zero.
- **Step 4 needs a committed fixture vault.** A round-trip inside one
  build passes even if both sides changed together.

  ⚠ **This requirement contradicts one resolution of §2.1.** Taking the
  secret out of the JSON *changes the at-rest format*; both cannot hold.
  If that option wins, this becomes a **migration** check: an old-format
  vault must still open, a new one is written in the new shape, and the
  fixture set needs one of each. If instead the secret never reaches the
  caller, the format is untouched and the check stands as written.
  **§2.1 determines what step 4 is testing.**
- **Step 7's check is that the suite still passes with the doors shut** —
  which proves no test was quietly depending on them being open.

**Absence alone is not enough.** Each step must also add L2 coverage for
what it introduces, derived from what the design promises rather than
from what the new code does:

| Step | What must be pinned |
|---|---|
| 1 | A minted key is present, is the requested length, and the same name twice throws. |
| 2 | Encrypt-then-decrypt round-trips; a tampered byte fails to open; **two encryptions of the same plaintext under the same key differ** — that is the nonce actually being fresh, and it is the assertion that catches a nonce bug. |
| 3 | Same password and scope give the same key; **the same password with a different scope gives a different one** — the property protecting one vault from another's password. `add_` throws on an existing name; `replace_` succeeds and the new key is the one used afterwards. |
| 4 | Save-then-load round-trips. Wrong password fails to open. File permissions match what the vault's owning HEPs require. Plus the fixture check above. |
| 6 | After `load_identity_into`, the store holds the expected public key; the vault types no longer expose a secret accessor; `create` never materialises a secret. |
| 7 | Nothing to add — this step *removes* surface. |

---

## 7. What this unblocks

The script-facing secret store needs "validate a password once, keep the
key in protected memory, use it by name later." That is
`open_file_with_password` plus `save_encrypted_file` — the same
mechanism, no new surface. Doing this first makes that work a consumer of
key handling rather than a designer of it.

---

## 8. Residual risks

Beyond the two decisions in §2. HEP-CORE-0043 §2.5.5 lists **four** —
the payload-format one is numbered 0 there and is §2.1 here, promoted
because it gates the work rather than sitting alongside it.

1. **The write side leaks and the new API would inherit it.**
   `save_encrypted_file(path, payload, key_name)` must take a **span**,
   not a `std::string` — otherwise it blesses the very pattern §3.2
   documents. Serialising into locked storage is a real problem, and
   wiping a temporary afterwards is not a fix: small-string optimisation
   and reallocation mean the bytes may already have been elsewhere.
2. **Nothing constrains which file a caller may open.** The sandboxing
   rule namespaces key *names*; both file operations take an arbitrary
   *path*. Anything exposing these to scripts must own the directory and
   accept an entry name, never a path — otherwise a script reaches
   another role's vault, or the hub's, and the name sandbox never sees it.
3. **`replace_key_from_password` can target any name**, including
   `hub_identity` — swapping the hub's identity for one derived from an
   attacker-chosen password. Unreachable from untrusted input today,
   which is the weakest guarantee available. Refusing the framework
   identity names costs a few lines.

**Not blocking, independent of this work:** the at-rest format has no
self-describing header, so a vault written under different compile-time
KDF parameters fails to open with an error indistinguishable from a wrong
password. Recorded in HEP-CORE-0043 §8.

---

## Related

- `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` — §1.0 the contract in
  plain language, §2.5 the operation definitions, §2.5.5 the holes.
  Authoritative.
- `docs/todo/AUTH_TODO.md` § "Open — security posture" — the tracker
  entry, and the standing file/dir vault item this unblocks.
