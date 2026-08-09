# Named-key operations — closing the courier gap in the security module

**Status:** plan, not yet started. Design lands in HEP-CORE-0043 §2.5;
this document is the how and the order.

**Why it exists:** a review on 2026-08-08 found three places holding secret
bytes outside the security module. They are not three independent bugs —
they are one API gap with three symptoms. This records the finding, the
fix, and the sequence, so the work can be picked up cold.

---

## 1. What is wrong, in one paragraph

The security module can do a job *for* you, or it can hand you a key so
you can do the job yourself. For public-key operations it does the job —
you name a key, it encrypts. For symmetric operations it does not: there
is no way to say "encrypt this under the key called X", so every caller
that needs symmetric encryption has to fetch the key first. Fetching means
holding secret bytes in ordinary memory that the operating system may write
to disk. Three callers do this today, and none of them had a choice.

---

## 2. What the review actually found

Each of these was read, not inferred.

### 2.1 The vault key is a plain stack array

`vault_crypto.cpp` derives the vault key with Argon2id into
`std::array<uint8_t, 32>` — an ordinary local. It is wiped by a scope
guard afterwards, which is the right instinct, but wiping is not the
issue: **the memory is not locked, so the OS can page it to disk while it
is still live.** Wiping RAM afterwards does not unwrite a swap block.

There is a second, subtler point. `vault_derive_key` returns the array by
value. Named-return-value optimisation is not guaranteed, so a copy may be
left behind in the callee's frame, unwiped.

**Why it is written that way:** the safe type could not be used.
`SecureBuffer<N>` wipes on destruction — but its move constructor is
deleted, so it cannot be returned from a function. A function whose whole
job is to produce a key and hand it back therefore *cannot* use it. This
is the clearest evidence that the problem is the shape of the API and not
the diligence of the author.

### 2.2a The create path is a chain of copies, and JSON is why

**Understated in the first two drafts of this document, which said "a
`std::string` copy". It is four.** Tracing `RoleVault::create` for a
freshly generated private key:

| # | Where it lands | Wiped? |
|---|---|---|
| 1 | `CurveKeypair::secret_z85` — the struct is two `std::string`s | no |
| 2 | `const std::string sec_str = std::move(kp.secret_z85)` — moved, so the same allocation, but still an unwiped `std::string` | no |
| 3 | `json payload = {…, {"secret_key", sec_str}}` — nlohmann copies it into a JSON string node | no |
| 4 | `payload.dump()` — a third string, handed to `vault_write` | no |
| 5 | `pImpl->secret_z85` — the fixed array | **yes**, in the destructor |

Only the last one is handled. The other four are freed without wiping, so
the bytes stay in released heap until something reuses that memory.

**And this is the same problem as §7's write-side finding, seen from the
other end.** Both come from one decision: **the vault payload is JSON, and
a JSON string node holds a `std::string`.** As long as the private key has
to become a value inside a JSON document, it must become an unwiped heap
string on the way in and on the way out. No amount of care at the call
site changes that — the format requires it.

That reframes step 4. Making `save_encrypted_file` take a span is
necessary but not sufficient; the caller still has to build the payload,
and if building it means `json{{"secret_key", …}}.dump()`, the leak simply
moves upstream of the new API.

Two ways out, and this is a design decision, not an implementation detail:

- **Keep the secret out of the JSON.** Payload becomes metadata (uid,
  public key — none of it secret) plus a raw key section. The secret is
  copied as bytes into locked storage and never becomes a string.
- **Never let the secret reach the caller in the first place.** Create
  goes through `keys().generate_and_add_identity(name)` — which already
  exists and already returns only the public half — and the vault is
  written by reading inside `with_seckey` straight into the output
  buffer. Same shape as arming a socket. This removes copies 1-4 outright
  and is the more consistent answer.

The second is preferable and costs a restructure of `create`. Decide
before step 4.

### 2.2 Private keys pass through `std::string`

`hub_vault.cpp` and `role_vault.cpp` both do:

```cpp
const std::string broker_secret = std::move(kp.secret_z85);
```

and the hub's save path builds another `std::string` copy of the secret
to assemble its JSON payload.

`std::string`'s destructor does not wipe. This is exactly the pattern the
codebase already removed once: both `Impl` structs carry a comment saying
the `std::string` members were replaced by fixed arrays *"whose destructors
are not guaranteed to zero heap memory"* — and then the same pattern
survives in the functions on either side of them.

### 2.3 The config loaders are couriers

`role_config.cpp` and `hub_config.cpp` call `vault.secret_key()`, receive a
`string_view` onto the secret, and pass it to the KeyStore. The window is
short and nothing is copied, but the caller is in the chain of custody for
no reason: the vault could deposit the identity itself.

### 2.4 What was already right

Worth recording so nobody "fixes" it:

- **Socket arming** already has the shape we want.
  `arm_curve_server(sock, name)` takes a name; the secret is read inside a
  callback and goes straight into the socket option.
- **Wiping discipline is good everywhere.** Both vault `Impl` structs wipe
  in their destructors; the vault key has a scope guard. The gap is
  page-locking, not wiping.
- **`box_*_using` is the model.** It already names its key. The symmetric
  side simply never got the same treatment.

### 2.5 One honest limit

libzmq's socket options take raw key bytes. Arming a socket must hand the
secret over. `with_seckey` narrows that to a callback with no copy, which
is as small as the window gets, but it is a real export and no amount of
API design removes it.

---

## 3. The fix

Add the missing operations so the three callers stop being couriers.
Defined in HEP-CORE-0043 §2.5; summarised here:

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
  keeps its explicit nonce because a protocol frame's layout is shared with
  another implementation; a sealed blob's is not.
- **A derived key lives for the process.** One password derivation at
  startup; saves afterwards are immediate. `replace_key_from_password`
  covers a password change without a restart.
- **`add_` throws on an existing name; `replace_` is separate.** Silently
  overwriting an identity key would be catastrophic and hard to notice.
- **No new guard types.** `SecureBuffer`, `LockedKey`, `with_seckey` and
  `Z85PublicKey` already exist. The one that failed here failed because a
  non-movable type cannot be returned — and the fix is to stop returning
  secrets, not to build a better container for returning them.
- **A scope-bound key handle is deferred.** Nothing above needs it;
  everything above is process-lifetime. It belongs with the ephemeral
  capability grant.

---

## 4. Order of work

Each step leaves the tree green and is independently reviewable.

| Step | Change | Removes |
|---|---|---|
| 1 | `add_random_key` | the mint-on-stack-then-`add_raw` pattern in the admin session seal |
| 2 | `secretbox_*_using` | the two `lookup_raw` spans in `admin_session.cpp` |
| 3 | `add_key_from_password` + `replace_key_from_password` | — (prerequisite for 4) |
| 4 | the three file operations; migrate `vault_crypto` | **the stack key (§2.1)** |
| 5 | `load_identity_into` on **both** vault types; drop the secret accessors | **the couriers (§2.3)** |
| 5b | restructure `create` onto `generate_and_add_identity` + `with_seckey`; delete the `secret_z85` members | **the four-copy chain (§2.2a)** and the `std::string` copies (§2.2) |
| 6 | make the raw-key `secretbox_*` private; decide `lookup_raw` | **the ability to reintroduce any of it** |

**Step 5b was missing from the first three drafts.** Step 5 removes the
*accessor* — but the vault object still holds the secret in
`Impl::secret_z85`, an ordinary heap array inside `unique_ptr<Impl>`. It
is wiped on destruction, which is the good half, but it is not locked
memory, so it can be paged to disk while the object is alive. Deposit-
into-KeyStore is only a real fix if the secret never lands in a member on
the way there. That means `open` decrypts and deposits directly from the
`SecureBuffer`, and `create` never materialises a secret at all.

Steps 1-2 are self-contained and prove the shape on a live consumer before
the vault depends on it. Step 4 is the one with real risk — it touches the
at-rest format's read and write paths.

**Step 5 is bigger than one line and needs deciding before it starts.**
Both vault types need the method — `role_config.cpp` uses
`secret_key()` and `hub_config.cpp` uses `broker_curve_secret_key()`, so
naming only `RoleVault` would leave the hub courier in place.

More importantly, **ten test sites depend on those two accessors**, across
`test_role_vault.cpp` and `test_hub_vault.cpp`, and they are not lazy
tests:

| What the test proves | Why it currently needs the secret |
|---|---|
| The vault file is genuinely encrypted | searches the raw bytes for the secret and expects **not found** |
| Create and open yield the same keypair | compares the secret across two objects |
| The keypair survives a reopen | stores the secret, reopens, compares |

Removing the accessor removes the ability to write the first one as
currently framed. The resolution is not to keep a test-only accessor —
that is a production surface existing for tests, which this project
rejects. It is to re-express each assertion:

- **Encrypted-at-rest:** search the raw bytes for the **public** key
  instead. If the payload were plaintext the pubkey would appear too, and
  the pubkey is not secret. Same proof, no secret needed.
- **Round-trip and persistence:** call `load_identity_into(name)` and
  compare `keys().pubkey(name)`. This is a *better* test than the current
  one — it exercises the production deposit path rather than an accessor
  that would then exist only for tests.

Do this re-expression as its own commit **before** deleting the accessors,
so the deletion is mechanical and the test change is reviewable on its own.

### Step 6 — close the doors, or none of the above sticks

**Missed in the first draft of this plan and it is the step that decides
whether the work holds.** Steps 1-5 migrate every *caller* off the
fetch-a-key pattern. They do not remove the *ability*. Leave the raw-key
entry points public and the next person writes a new courier, with the API
inviting them to.

The asymmetric side already shows the intended end state: there is **no
public raw-key `box_encrypt`** — only `box_encrypt_using`. The symmetric
side never got the same treatment, and that omission is the root of every
symptom in §2.

| Entry point | Production callers after step 5 | Disposition |
|---|---|---|
| `secretbox_encrypt` / `secretbox_decrypt` (raw key span) | none | **Make private.** The `_using` variants call them internally. Exactly what `box_*` already does. |
| `keys().lookup_raw(name)` | none | **Needs a decision — see below.** |

`lookup_raw` is the harder one and should not be decided in passing. Once
the named operations exist it has no production consumer, which makes it
surface that exists only for tests. But removing it also removes the only
way to read a raw secret at all, and two `key_store` tests exercise it
directly (`lookup_raw_on_missing_throws`,
`add_raw_then_lookup_raw_roundtrip`). Retiring it is a contract handoff,
not a deletion: the "a missing name throws" contract has to land somewhere
before the accessor goes.

**Do not skip step 6 and call the arc finished.** An API that still offers
the unsafe door has not consolidated anything — it has two ways to do one
job, which is the shape this whole plan exists to remove.

---

## 5. How each step gets verified

**Not by a green suite.** Every change here is invisible to behaviour: the
same bytes get written and read either way. A passing test proves nothing
about whether a secret stopped being copied.

- **Steps 1-2, 5:** the check is *absence*. After the change, grep the
  migrated file for `lookup_raw`, `secret_key()`, `std::string` holding a
  secret — and read the hits. Zero is the pass condition.
- **Step 4:** the at-rest format must not change. A vault written by the
  old code must open under the new code, and the reverse. This needs a
  fixture vault committed as a test input, not a round-trip within one
  build — a round-trip passes even if both sides changed together.

  ⚠ **This requirement contradicts one resolution of hole 0, and the
  contradiction has to be settled before step 4 starts.** Hole 0's first
  option — take the secret out of the JSON so it never becomes a string —
  *changes the at-rest format*. Both cannot hold. If that option wins,
  step 4's compatibility check becomes a **migration** check instead: an
  old-format vault must still open (read path keeps the old shape), a new
  one is written in the new shape, and the fixture set needs one of each.
  If the second option wins — never let the secret reach the caller — the
  format is untouched and the check stands as written. **Resolve hole 0
  first; it determines what step 4 is even testing.**
- **Whole arc:** the existing sodium-boundary guardrail keeps holding, and
  a new guardrail could assert that `secret_key()` has no callers once step
  5 lands.

**New tests each step must bring.** The absence checks above prove the old
pattern is gone; they say nothing about whether the new methods are
correct. Each step adds L2 coverage for what it introduces:

| Step | What must be pinned |
|---|---|
| 1 | A minted key is present, is the requested length, and the same name twice throws. |
| 2 | Encrypt-then-decrypt round-trips; a tampered byte fails to open; **two encryptions of the same plaintext under the same key differ** — that is the nonce actually being fresh, and it is the assertion that would catch a nonce bug. |
| 3 | Same password and scope give the same key; **the same password with a different scope gives a different one** — that is the property protecting one vault from another's password. `add_` throws on an existing name, `replace_` succeeds and the new key is the one used afterwards. |
| 4 | Save-then-load round-trips. Wrong password fails to open. File permissions are what §8's owners require. Plus the fixture-vault compatibility check above. |
| 5 | After `load_identity_into`, the store holds the expected public key; the vault types no longer expose a secret accessor. |
| 6 | Nothing to add — this step *removes* surface. The check is that the suite still passes with the raw-key entry points private, which proves no test was quietly depending on the door being open. |

Derive these from what the design promises, not from what the new code
happens to do — a test written by reading the implementation passes
whatever the implementation does.

---

## 6. What this unblocks

The script-facing secret store needs "validate a password once, keep the
key in protected memory, use it by name later." That is
`open_file_with_password` plus `save_encrypted_file` — the same mechanism,
no new surface. Doing this first means that work is a consumer rather than
a designer of key handling.

---

## 6a. A strategy question this plan should not dodge

**This plan is a find-every-site strategy, and the evidence says finding
every site is not reliable.** Four consecutive review passes each turned
up one more copy: the stack key, then the `std::string` couriers, then
the write-side payload, then the four-copy create chain. Each pass
believed the previous one had finished. There is no reason to think the
fifth would have been the last.

There is a defence that does not depend on the audit being complete:
`mlockall(MCL_CURRENT | MCL_FUTURE)` at startup locks **every** page the
process ever allocates, including the copies nobody has found. One call.
HEP-CORE-0035 §4.7.2's first measure asks for exactly this outcome —
"lock secret pages into RAM… apply to in-memory copies of the role and
hub secret" — and the current implementation delivers it only for
KeyStore allocations, via `sodium_malloc` inside `LockedKey`. Nothing
locks the process; `mlockall` appears nowhere in the tree.

**But it is probably wrong for this system, and the reason is worth
writing down so nobody re-derives it.** pylabhub is a data-acquisition
framework whose whole point is large shared-memory data blocks.
`MCL_FUTURE` locks every future mapping — including those blocks. That
either exhausts `RLIMIT_MEMLOCK` and starts failing allocations, or pins
gigabytes of sample data in RAM to protect a few dozen bytes of key. The
data plane makes the blunt instrument unusable.

A narrower variant — `mlock()` on specific regions — is what
`sodium_malloc` already does, so that road leads back to "get the secrets
into the KeyStore," which is this plan.

So: **the strategy stands, but its weakness is now stated.** It depends
on completeness, completeness has failed four times, and the cheap
backstop is unavailable for a reason specific to this codebase. That
argues for the parts of the plan that make a whole *class* of copy
impossible — the private raw-key doors in step 6, and generating straight
into the key store in step 5b — over the parts that fix known sites one
at a time.

## 7. Open questions and risks

**Four holes in the design itself are recorded in HEP-CORE-0043 §2.5.5.
Read that section before starting step 4.** Summarised:

0. **The payload format is the cause and it constrains the fix.** The
   vault payload is JSON and the private key is a value in it; a JSON
   string node holds a `std::string`, so the secret must become unwiped
   heap on the way in and out. §2.2a traces four such allocations during
   vault creation. Either the secret stops being a JSON value, or it
   never reaches the caller at all (generate into the key store, write
   the file from inside a scoped accessor). **Every point below is
   downstream of this one.**
1. **The write side leaks and the new API would inherit it.** Reading a
   vault was already fixed — `vault_read_secure` decrypts into a
   `SecureBuffer` span, "no `std::string` materializes." Writing was not:
   `vault_write` takes a `std::string`, and all three callers pass
   `payload.dump()`, materialising a private key in unwiped heap.
   `save_encrypted_file` must take a **span**, not a string — and
   serialising JSON into locked storage is a real problem that needs
   solving before step 4, not after. Wiping the temporary is not a fix
   (small-string optimisation, reallocation).
2. **Nothing constrains which file a caller may open.** The sandboxing
   rule namespaces key *names*; both file operations take an arbitrary
   *path*. Anything exposing these to scripts must own the directory and
   accept an entry name, never a path.
3. **`replace_key_from_password` can target any name**, including
   `hub_identity`. Unreachable from untrusted input today, which is the
   weakest guarantee available. Refusing the framework identity names
   costs a few lines.

**Not blocking, independent of this work:** the at-rest format has no
self-describing header, so a vault written under different compile-time
KDF parameters fails to open with an error indistinguishable from a wrong
password. Recorded in HEP-CORE-0043 §8.

---

## Related

- `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` — §1.0 the contract in
  plain language, §2.5 the operation definitions. Authoritative.
- `docs/todo/AUTH_TODO.md` § "Open — security posture" — the standing
  file/dir vault item this unblocks.
