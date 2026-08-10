# Taking the secret out of the vault's JSON payload

**Status:** design, nothing implemented.
**Task:** #137 step 6 remainder. **Design authority:** HEP-CORE-0043 §2.5.5 hole 0.
**Prerequisite work already shipped:** `512fe1d5`, `2fbc54f7`, `ce8c67e3`.

---

## 1. What is wrong

A role vault decrypts to this:

```json
{ "role_uid":   "prod.sensor1.uid3a7f2b1c",
  "public_key": "rq:rM>}U?@Lns47E1%kR.o@%&BqW=Ib!r]Gv:{)}",
  "secret_key": "JTKVSB%%)wK0E.X)V>+}o?pNmC{O&4W4b!Ni{Lh6" }
```

The private key is a **value inside a JSON document**. Every JSON library
holds string values as ordinary strings, so getting the key in or out
means it becomes ordinary heap memory — memory that is freed without
being wiped, and that the OS may page to disk before then.

Tracing a freshly generated key through `RoleVault::create`
(`role_vault.cpp:88-128`):

```
generate_curve_keypair()
        │
        ▼
  ① kp.secret_z85         std::string   ← freed WITHOUT zeroing
        │  std::move  (same allocation)
        ▼
     sec_str
        │  nlohmann copies it into a JSON string node
        ▼
  ② payload["secret_key"] std::string   ← freed WITHOUT zeroing
        │  .dump()
        ▼
  ③ dumped JSON text      std::string   ← freed WITHOUT zeroing
        │  encrypt + write
        │  memcpy
        ▼
  ④ pImpl->secret_z85     array         ← the ONLY one wiped
```

Three unwiped copies. `open()` is better — it uses `get_ref` plus a
`wipe_on_exit` guard on the JSON's own string — but it is mitigation
around the same underlying shape.

**This is not a call-site problem.** No amount of care at the call site
removes it while the key is a JSON value. That is why it needs a format
change and not another refactor.

## 2. What is NOT wrong, and must not change

Worth stating plainly, because it bounds the work:

- **The encryption is fine.** XSalsa20-Poly1305 over the whole payload,
  Argon2id from the password, per-vault salt from the uid. None of that
  moves.
- **Confidentiality at rest is fine.** The JSON is already inside the
  ciphertext. This is purely about **in-process memory hygiene**, not
  about what an attacker reads off the disk.
- **The file envelope is fine.** `[nonce(24) ‖ MAC(16) ‖ ciphertext]`
  stays exactly as it is.

Only the *plaintext layout inside the envelope* changes.

## 3. What the payload should become

Split it by whether a field is secret:

| Vault | Non-secret (stays JSON) | Secret (moves to a raw section) |
|---|---|---|
| Role | `role_uid`, `public_key` | `secret_key` (32 raw bytes) |
| Hub | `broker.curve_public_key`, `known_roles` | `broker.curve_secret_key` (32 raw), `admin.token` (32 raw) |

Proposed plaintext layout:

```
 ┌──────────┬───────────────┬────────────────────────┬──────────────────┐
 │ version  │ metadata_len  │ metadata (JSON, plain) │ raw secret bytes │
 │ 1 byte   │ 2 bytes (LE)  │ metadata_len bytes     │ fixed by version │
 └──────────┴───────────────┴────────────────────────┴──────────────────┘
```

Writing: the security module copies the raw key straight from the
KeyStore into the buffer inside a `with_seckey` callback. The JSON never
sees it. Reading: parse the metadata (which holds no secrets), then hand
the raw tail directly to `add_identity` — no `std::string` anywhere on
the secret's path.

Note the secrets become **raw 32-byte** rather than 40-char Z85. That
matches how the KeyStore already stores them (HEP-CORE-0040 §8.5.2), so
it removes a Z85 encode/decode step as well.

## 4. Telling old files from new — the actual constraint

**There is no version field, and no magic bytes, anywhere in a vault
file today.** It is nonce followed by ciphertext. Verified: no `version`
or `magic` in `vault_crypto.hpp`, and no header written by
`vault_write`.

So a reader cannot know which format it is holding until after it
decrypts. This is the same gap already noted against HEP-CORE-0043 §8:
a vault written under different KDF settings fails to open
*indistinguishably from a wrong password*.

Two placements for the version:

- **Inside the plaintext** (as drawn above). Simple, but you must
  decrypt before you can dispatch — fine here, since decryption does not
  depend on the version.
- **In a cleartext header before the nonce.** A version number is not
  secret. This lets a reader dispatch, and it could also carry the KDF
  profile, which would close the §8 gap at the same time — a wrong-KDF
  vault could then say so instead of impersonating a wrong password.

The second is better and barely more work, but it is a wider change.
Worth deciding deliberately rather than by default.

## 5. Migration — the real decision

| Option | Operator does | Cost |
|---|---|---|
| **A. Read both, write new** | nothing; vaults upgrade on next save | the old JSON read path — *with its unwiped copies* — stays in the tree indefinitely, and you can never prove every vault converted. Under our no-grace-periods frame this is the weakest option: it keeps the defect alive to support files that may not exist. |
| **B. Hard cutover, re-keygen** | `--keygen` again for every role, then re-register every pubkey with the hub | cleanest code, but it **destroys existing identities**. Every role's allowlist entry must be re-added. That is a large operational event for what is a container change. |
| **C. One-shot converter** | run a convert subcommand once per vault | **preserves the keypair** — same identity, new container. New code reads only the new format and refuses old ones with a message naming the fix. No legacy read path lingers. |

**Recommendation: C.** It satisfies the hard-enforce frame (no dual-read
path, no silent grace period) without the identity churn of B. The
converter is small: it is the only place allowed to run the old read
path, and it can be deleted a release later.

**One question that changes the answer:** if no vault exists outside
development yet, B is free and simplest — nothing to migrate. That is
an ownership fact, not something the code can tell me.

## 6. What this buys

- Copies ①②③ stop existing. The secret's only unlocked form becomes the
  raw bytes in the write buffer, which the security module owns and can
  wipe.
- Combined with the remaining part of step 6 (`open()` depositing during
  parse and the vault storing no member at all), the vault stops holding
  a secret in unlocked memory entirely.

## 7. What it does NOT buy

Say this out loud so it is not oversold: **an attacker who can read
process memory still wins.** This narrows a window — pages reaching swap
or a hibernation file, and freed-but-unzeroed heap — it does not make
the key unreadable to something already inside the process.

## 8. Work, in order

1. Decide migration (§5) and version placement (§4). **Blocking — both
   change what gets built.**
2. Define the layout as a struct with a static_assert on its size; put
   the constants where `vault_crypto.hpp` already keeps its sizes.
3. Write path: build the buffer inside `with_seckey`; the JSON metadata
   is assembled separately and never touches a secret.
4. Read path: parse metadata, hand the raw tail to `add_identity`.
5. Converter (if C) — the sole remaining user of the old read path.
6. Delete the old read/write path and the `secret_z85` members.
7. Tests: a **committed fixture of each format** — this is the first
   step in this arc where the on-disk bytes move, so a round-trip test
   in one process proves nothing about compatibility.

## 9. Risk

The riskiest step is 6. Deleting the old path is what makes the change
real, and it is also the point of no return for any vault that was not
converted. Sequence it as its own commit, after the converter has been
available for at least one release.
