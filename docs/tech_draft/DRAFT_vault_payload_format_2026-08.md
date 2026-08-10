# Bringing the vault code to the format contract

**Status:** implementation plan. Nothing built.
**Contract (authoritative): `HEP-CORE-0035 §4.6.6 Vault file format`.**
Read that first — it holds the design, the diagram, the example, and
invariants VF-1..VF-8. This document does not restate them and must not
drift from them; if the two disagree, the HEP is right.

Related: HEP-CORE-0043 §2.5 (named-key operations, shipped),
HEP-CORE-0040 §8.5.2 (raw key storage form).
Task: #137 final item. Tracker: `docs/todo/AUTH_TODO.md`.

> **Owner ruling: this is a framework redesign. Backward compatibility
> is NOT a consideration.** No converter, no dual-read path, no version
> negotiation with the past. Development vaults are regenerated with
> `--keygen`.

---

## 1. The gap between the contract and the code

| Contract | Code today |
|---|---|
| VF-1 magic + version checked before deriving | no header exists at all — the file is nonce followed by MAC-then-ciphertext |
| VF-2 derive at the file's recorded cost | derives at the reader's own compile-time profile; a vault written under another fails **indistinguishably from a wrong password** |
| VF-3 header authenticated as associated data | nothing to authenticate; `crypto_secretbox` has no associated-data input |
| VF-6 secret never in the metadata document | the secret **is** a JSON value — `"secret_key": "..."` |
| VF-7 no unwiped copy on either path | `create` builds three unwiped `std::string`s before the one wiped array |
| VF-8 secret section sized by compile-time assertion | no secret section exists |

The VF-6 and VF-7 rows are the same defect seen twice. Tracing a fresh
key through `RoleVault::create` (`role_vault.cpp:88-128`):

```
generate_curve_keypair()
  ① kp.secret_z85          std::string   ← freed WITHOUT zeroing
       │ std::move (same allocation)
  ② payload["secret_key"]  std::string   ← freed WITHOUT zeroing
       │ .dump()
  ③ dumped JSON text       std::string   ← freed WITHOUT zeroing
       │ encrypt + write / memcpy
  ④ pImpl->secret_z85      array         ← the ONLY one wiped
```

`open()` is better — `get_ref` plus a `wipe_on_exit` guard — but it is
mitigation around the same shape. While the key is a document value, no
call-site discipline removes the copies. That is why the contract
changes the format rather than the code changing its habits.

## 2. What stays

Bounding the work, so the diff does not sprawl:

- Argon2id from the password, salt from the uid. Unchanged.
- The `0600` file / `0700` directory discipline of §4.6.1. Unchanged.
- `known_roles` stays in the hub's metadata — it is not secret, and
  §4.8 already owns its schema.
- The nonce stays 24 bytes and the tag 16, so only the header is added
  to the file's overhead.

## 3. Order of work

1. **Header and layout constants**, with the VF-8 assertion, beside the
   existing `kVault*` sizes in `vault_crypto.hpp`.
2. **Swap the primitive** to `crypto_aead_xchacha20poly1305_ietf_*`
   inside the security module, header passed as associated data.
   Confirmed present in the vendored libsodium (NPUBBYTES 24,
   ABYTES 16).
3. **Write path** — assemble metadata with no secret in it; copy the
   secret section directly out of the key store inside `with_seckey`.
4. **Read path** — verify header (VF-1, VF-5), derive at the file's
   profile (VF-2), decrypt with associated data (VF-3), check the
   plaintext covers the secret section (VF-4), deposit that section
   straight into the key store (VF-7).
5. **Delete** the old read/write path, the `secret_z85` members, and
   every place that treats the build's KDF profile as a read input.
6. Regenerate development vaults.

Steps 1-4 can land as one commit. Step 5 is the point of no return and
should be its own.

## 4. Tests

The on-disk bytes move for the first time in this arc. A round-trip
inside one process would pass even if the layout silently changed, so it
proves almost nothing on its own.

- **A committed fixture vault**, opened by the test. This is the only
  thing that catches an accidental layout change later.
- **Tamper each header field in turn.** Each must fail, and VF-1 / VF-5
  failures must be distinguishable from a wrong password.
- **Cross-profile open** — a vault written at one KDF cost, opened by a
  build configured for another, must succeed. This is the VF-2
  assertion, and it is the one that proves the profile is read from the
  file rather than assumed.
- **Truncation** at each boundary: mid-header, mid-nonce, mid-tag,
  mid-metadata.
- The existing hygiene checks continue to apply: no field name and no
  public key visible in the raw file bytes.

## 5. Consequences worth expecting

- The "do NOT use test-mode-built binaries against a production vault"
  warning in `vault_crypto.hpp` stops being true and should go. Under
  VF-2 any binary opens any vault.
- The CI fast-KDF flag becomes purely a **write-time** choice. A
  CI-built binary can still read a production vault.
- `key_file_acl.hpp` describes the vault layer as "libsodium AEAD".
  That was inaccurate for `secretbox`; after step 2 it is accurate.
- Moving a secret into the key store makes its holder require the
  security module to be up. That pattern has already cost one round of
  test fallout in this arc; budget for it again on the `create` path.
