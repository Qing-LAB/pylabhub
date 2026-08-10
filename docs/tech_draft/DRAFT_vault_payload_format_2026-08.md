# Vault file format — redesign

**Status:** design, nothing implemented.
**Task:** #137 step 6 remainder. **Design authority:** HEP-CORE-0043 §2.5.5 hole 0, §8.
**Prerequisite work shipped:** `512fe1d5`, `2fbc54f7`, `ce8c67e3`, `072787e6`.

> **Owner ruling 2026-08-09: this is a framework redesign. Backward
> compatibility is NOT a consideration.** The format is chosen for
> correct handling of secrets and for data integrity, not to remain
> readable by anything that exists today. Development vaults are
> regenerated with `--keygen`. No converter, no dual-read path, no
> version negotiation with the past.

---

## 1. The two defects this fixes

### 1.1 The secret is a JSON value

A role vault decrypts to this:

```json
{ "role_uid":   "prod.sensor1.uid3a7f2b1c",
  "public_key": "rq:rM>}U?@Lns47E1%kR.o@%&BqW=Ib!r]Gv:{)}",
  "secret_key": "JTKVSB%%)wK0E.X)V>+}o?pNmC{O&4W4b!Ni{Lh6" }
```

JSON libraries hold string values as ordinary strings. So getting the
key in or out means it becomes ordinary heap memory — freed without
wiping, and pageable to disk before then. Tracing a fresh key through
`RoleVault::create` (`role_vault.cpp:88-128`):

```
generate_curve_keypair()
        │
  ① kp.secret_z85         std::string   ← freed WITHOUT zeroing
        │  std::move  (same allocation)
  ② payload["secret_key"] std::string   ← freed WITHOUT zeroing
        │  .dump()
  ③ dumped JSON text      std::string   ← freed WITHOUT zeroing
        │  encrypt + write / memcpy
  ④ pImpl->secret_z85     array         ← the ONLY one wiped
```

No amount of care at the call site removes this while the key is a JSON
value. That is why it is a format change and not another refactor.

### 1.2 The file describes nothing about itself

A vault today is `[nonce(24)][MAC(16) ‖ ciphertext]`. There is no magic,
no version, no record of how the key was derived — verified: no `version`
or `magic` anywhere in `vault_crypto.hpp` or `vault_write`.

Consequences, both real:

- A vault written under one Argon2id profile and read under another
  fails **indistinguishably from a wrong password** (the HEP-0043 §8
  gap). The reader derives with *its own* compile-time profile and has
  no way to learn the file's.
- A corrupt file, a truncated file, and a file that was never a vault
  all fail the same way.

## 2. What does NOT change

Bounding the work:

- **Argon2id from the password, salted with the uid.** Unchanged.
- **Confidentiality at rest.** The payload was already inside the
  ciphertext; this is about in-process memory hygiene and integrity, not
  about what an attacker reads off the disk.
- **The 0600 / 0700 file and directory discipline.** Unchanged.

## 3. The format

```
 cleartext, authenticated as AAD          encrypted
 ┌────────────────────────────────┐ ┌──────┐ ┌──────────────────────────┐
 │ magic   "PLHVAULT"    8 bytes  │ │nonce │ │ tag 16 │ ciphertext      │
 │ version               1 byte   │ │  24  │ └──────────────────────────┘
 │ kdf_profile           1 byte   │ └──────┘
 │ vault_kind            1 byte   │
 │ reserved (zero)       1 byte   │
 └────────────────────────────────┘
        12 bytes

 plaintext inside the ciphertext:
 ┌──────────────┬────────────────────────┬─────────────────────┐
 │ metadata_len │ metadata (JSON, plain) │ raw secret section  │
 │ 2 bytes LE   │ metadata_len bytes     │ fixed by vault_kind │
 └──────────────┴────────────────────────┴─────────────────────┘
```

Split by whether a field is secret:

| Vault kind | Metadata JSON (non-secret) | Raw secret section |
|---|---|---|
| Role | `role_uid`, `public_key` | seckey — 32 raw bytes |
| Hub | `broker.curve_public_key`, `known_roles` | broker seckey 32 ‖ admin token 32 |

Secrets become **raw bytes, not Z85**. That is how the KeyStore already
stores them (HEP-CORE-0040 §8.5.2), so it also removes an encode/decode
step from the path.

**Writing:** the security module copies the raw key from the KeyStore
into the buffer inside a `with_seckey` callback. The JSON is assembled
separately and never contains a secret.
**Reading:** parse the metadata, hand the raw tail straight to
`add_identity`. No `std::string` on the secret's path in either
direction.

## 4. Why the header forces an AEAD

The chain is forced, not stylistic:

1. We want the file to record which Argon2id profile wrote it, so a
   reader stops guessing (§1.2).
2. The reader needs that **before** it can derive the key — so it must
   be cleartext.
3. Cleartext that nothing authenticates is a new integrity hole: flip
   the profile byte and you change what the reader computes.
4. `crypto_secretbox` has no associated-data input, so it cannot
   authenticate a header.
5. Therefore: **XChaCha20-Poly1305 IETF** with the 12-byte header as
   associated data. Same 24-byte nonce, same 16-byte tag, and the header
   is covered.

Confirmed available in our vendored libsodium —
`crypto_aead_xchacha20poly1305_ietf_*`, NPUBBYTES 24, ABYTES 16
(`third_party/libsodium/.../crypto_aead_xchacha20poly1305.h`).

Note this makes an old comment true: `key_file_acl.hpp` used to call the
vault layer "libsodium AEAD" when it was secretbox. It will be AEAD.

### What the header buys

- **`kdf_profile` read from the file, not from the build.** Any binary
  can open any vault. The "do NOT use test-mode-built binaries against a
  production vault" warning disappears, and the CI fast-KDF flag becomes
  purely a *write-time* choice — a CI-built binary can still read a
  production vault.
- **Tampering is detected, not mistaken for a wrong password.** Flipping
  any header byte fails the AAD check.
- **`magic` separates "not a vault" from "wrong password."** Today they
  are the same error.
- **`vault_kind`** makes opening a hub vault as a role vault an explicit
  refusal rather than a confusing parse failure.

### Integrity details worth writing down

- `metadata_len` is validated against the decrypted length before it is
  used to slice — a length field is a parse primitive and gets checked.
- The secret section is fixed-size per `vault_kind`, with a
  `static_assert` tying it to the KeyStore's sizes, so a mismatch is a
  build error.
- `reserved` must be zero on read. A non-zero byte means a writer we do
  not understand; refuse rather than guess.

## 5. Work, in order

1. Header + layout constants, with the static_asserts. One place, beside
   the existing `kVault*` sizes.
2. Swap the primitive to the AEAD form inside the security module, with
   the header passed as associated data.
3. Write path: build the buffer inside `with_seckey`; JSON never sees a
   secret.
4. Read path: verify magic/version/kind, derive using the file's
   profile, decrypt, validate `metadata_len`, deposit the raw tail
   directly.
5. Delete the old read/write path, the `secret_z85` members, and the
   compile-time-profile-as-read-input assumption.
6. Regenerate development vaults (`--keygen`).

Steps 1-4 can land together; 5 is the point of no return and should be
its own commit.

## 6. Tests

The on-disk bytes move for the first time in this arc, so a round-trip
inside one process proves almost nothing. Needed:

- A **committed fixture vault** of the new format, opened by the test —
  that is the only thing that catches an accidental layout change later.
- Tamper each header field in turn; each must fail, and fail
  *distinguishably* from a wrong password.
- A vault written with one KDF profile, opened by a build configured for
  another — must succeed. This is the §8 gap, and it is the assertion
  that proves the profile is being read from the file.
- Truncation at each boundary (mid-header, mid-nonce, mid-tag,
  mid-metadata).
- The existing memory-hygiene checks continue to apply: no plaintext
  field name and no public key in the raw file bytes.

## 7. What this does not buy

Stated so it is not oversold: **an attacker who can already read process
memory still wins.** This removes the secret from freed heap and from
pages that could reach swap or a hibernation file. It does not make a
live process's memory safe, and it is not a defence against a debugger
attached to the running role.
