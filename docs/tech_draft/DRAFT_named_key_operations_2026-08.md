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
| 5 | `RoleVault::load_identity_into`; drop `secret_key()` from the public surface | **the couriers (§2.3)** and the `std::string` copies (§2.2) |

Steps 1-2 are self-contained and prove the shape on a live consumer before
the vault depends on it. Step 4 is the one with real risk — it touches the
at-rest format's read and write paths.

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
- **Whole arc:** the existing sodium-boundary guardrail keeps holding, and
  a new guardrail could assert that `secret_key()` has no callers once step
  5 lands.

---

## 6. What this unblocks

The script-facing secret store needs "validate a password once, keep the
key in protected memory, use it by name later." That is
`open_file_with_password` plus `save_encrypted_file` — the same mechanism,
no new surface. Doing this first means that work is a consumer rather than
a designer of key handling.

---

## 7. Open question

None blocking. The one live uncertainty is whether the at-rest format
should gain a self-describing header — today a vault written under
different KDF parameters fails to open with an error indistinguishable
from a wrong password. That is recorded in HEP-CORE-0043 §8 and is
independent of this work.

---

## Related

- `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` — §1.0 the contract in
  plain language, §2.5 the operation definitions. Authoritative.
- `docs/todo/AUTH_TODO.md` § "Open — security posture" — the standing
  file/dir vault item this unblocks.
