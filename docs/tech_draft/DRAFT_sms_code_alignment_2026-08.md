# Aligning the security module's code to HEP-CORE-0043

**Status:** plan, not started. HEP-CORE-0043 is the source of truth; this
document is the route from today's code to it.

**Scope note up front, because it is smaller than it looks and the reason
matters.**

---

## 0. What "this module" actually is

`src/*/security/` holds 28 files and about 10,200 lines. **HEP-CORE-0043
governs roughly a quarter of it.** The directory is a *location*, not an
ownership boundary — six different HEPs own code that happens to live
there:

| Governing HEP | Files in `security/` |
|---|---|
| **HEP-CORE-0043** | `secure_subsystem`, `key_store` |
| HEP-CORE-0040 | `secure_buffer`, `curve_keypair` |
| HEP-CORE-0035 | `peer_admission`, `known_roles`, `pubkey_origin`, `key_file_acl`, `attested_key` |
| HEP-CORE-0036 | `zap_router` |
| HEP-CORE-0037 | `domain_routing_table` |
| HEP-CORE-0041 | `shm_capability_channel`, `shm_attach_orchestrator`, `attach_channel_shm` |
| HEP-CORE-0044 | `attach_protocol` |

Taken from each file's own header citation, not inferred.

Aligning `zap_router` to HEP-0043 would be a category error — it answers
to HEP-0036. **The alignable surface is `secure_subsystem` + `key_store`,
about 2,100 lines**, plus the two files in the ownership dispute below.

---

## 1. Step 0 — resolve two ownership conflicts first

**"Align the code to the HEP" has no well-defined meaning until these are
settled.** Both are doc-vs-doc, not doc-vs-code, and both are cheap.

### 1.1 HEP-0040 and HEP-0043 both claim key memory

`secure_buffer.hpp` and `curve_keypair.hpp` cite **HEP-CORE-0040** as
their governing HEP. HEP-0040's status is *"Design — impl in flight"* —
it does not say it is superseded.

Meanwhile HEP-0043 §7 says it *"carries the full API surface preserved
from HEP-CORE-0040 §5.2"* and is *"now the primary reference"*, and
HEP-0043 §13 states plainly that **nothing is superseded by HEP-0043**.

So two live HEPs claim the same material, and §13 explicitly forecloses
the resolution that would settle it. This is precisely the shape
HEP-0043 §1.0 warns about — a decision with two owners — sitting inside
the document that warns about it.

Three ways out, all cheap; the choice is the owner's:

- **0040 becomes the key-memory HEP and 0043 cites it.** 0043 §7 stops
  claiming to be primary. Matches the file citations as they stand.
- **0043 absorbs it and 0040 is marked superseded.** Requires amending
  §13, which currently says nothing is.
- **Split by layer:** 0040 owns the *storage types* (`SecureBuffer`,
  `LockedKey`, `CurveKeypair`), 0043 owns the *module surface* that
  exposes them. Closest to how the code is actually arranged.

### 1.2 `attach_channel.hpp` cites HEP-0043 for HEP-0044's subject

Its sibling `attach_channel_shm` cites HEP-0041 and `attach_protocol`
cites HEP-0044. `attach_channel` citing 0043 is almost certainly a stale
pointer from before HEP-0044 was split out of 0043 §9.2. Verify against
0044's scope and repoint. Mechanical.

---

## 2. What alignment means, section by section

With scope settled, the work is a section-by-section audit of
`secure_subsystem` and `key_store` against HEP-0043. Each row is *"read
the section, read the code, record the delta"* — not a rewrite.

| HEP section | Subject | Known state |
|---|---|---|
| §1.1 Nature | one libsodium owner | **Aligned + guarded.** Six sodium includes, all inside the module; `SecurityGuardrail_SodiumConfinedToModule` enforces it. |
| §1.2 Init gate | `sodium_init` once, gate policy | Claims a specific policy: `keys()` gated, `box_*_using` gated transitively, everything else ungated. **Audit needed** — verify each method matches. |
| §1.3 Singularity | five mechanisms | **One known defect, already fixed in the doc:** §1.3 named `secure().crypto()`, which does not exist. Verify the other four mechanisms are present in code. |
| §1.4 Use-not-export | secret bytes stay inside | **This is the main body of work — see §3.** |
| §1.5 Rotation & lifetime | — | **Unaudited.** Nothing in this pass checked what §1.5 requires against what exists. Likely the largest unknown. |
| §1.6 Cross-platform layering | — | **Unaudited.** |
| §2.1 Two-category facade | class shape | Marked shipped. Spot-verify the category split still matches the header. |
| §2.2 KeyStore submodule | member, not peer | **Aligned** — `secure().keys()`, gated, verified. |
| §2.3 Lifecycle registration | Logger-shape static module | Marked shipped; verify. |
| §2.4 Cross-platform stubs | — | **Unaudited.** |
| §2.5 Named-key operations | the courier gap | **Nine operations missing.** Fully planned separately — see §4. |
| §3-§6 Primitives | random, hash, KDF, secretbox, box | Marked shipped. Verify declared signatures match the HEP's descriptions. |
| §7 KeyStore API | full surface | Blocked on the §1.1 ownership conflict above. |
| §8 Vault at rest | index only | Index, nothing to align in this module. |
| §9 Wire protocols | index only | Index; the code belongs to other HEPs. |
| §10 Script API | not designed | Nothing to align. |
| §11 Cross-platform matrix | per-OS support | **Corrected recently** (the script-vault row was false, Windows permission enforcement is partial). Verify the remaining rows against code rather than trusting them. |

**Rows marked "unaudited" are the honest output of this pass.** §1.5,
§1.6, §2.4 were never checked against code — not in this planning pass and
not, as far as the record shows, before it. They are where unknown deltas
most likely are.

---

## 3. The known deltas

Everything found so far is one API gap with several symptoms, planned in
full elsewhere:

- The vault key on the stack, the four-copy create chain, the courier
  loaders, and the unlocked vault member.
- **Plan:** `DRAFT_named_key_operations_2026-08.md`, seven steps, with
  two decisions to settle before any code.

This alignment plan does **not** restate that work. It carries it as one
component and adds the audit of everything the named-key work does not
touch.

---

## 4. Order

1. **Settle the two ownership conflicts** (§1). Cheap, and nothing below
   is well-defined without them.
2. **Audit the three unaudited sections** — §1.5, §1.6, §2.4 — and record
   deltas. Read-only; produces findings, not commits.
3. **Spot-verify the sections marked shipped** — §1.2's gate policy,
   §1.3's remaining mechanisms, §2.1, §2.3, §3-§6 signatures, §11's rows.
   Also read-only.
4. **Then execute the named-key plan** (its own seven steps).
5. **Re-run the whole table** and record what remains.

Steps 2-3 come before 4 deliberately: they are cheap, they change nothing,
and they may find deltas that alter the named-key work. Doing the code
change first and the audit after is how you discover a conflict with the
expensive half already built.

---

## 5. How this gets verified

The named-key work has its own verification rules — absence checks, a
fixture vault, per-step coverage. For the audit half:

- **A delta is recorded only when the section and the code have both been
  read.** Not "grep found nothing", which has produced four false findings
  in this codebase within a week.
- **An "aligned" verdict names the evidence** — the file and what was
  read — so a later pass can check the check.
- **Where the HEP is wrong and the code is right, fix the HEP.** The
  document being the source of truth does not make it correct; it makes it
  the place corrections land. Three such fixes already came out of the
  last review round (the storage-layout diagram, the deleted `crypto()`
  accessor, the CI-lint claim).

---

## Related

- `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` — the source of truth.
- `docs/tech_draft/DRAFT_named_key_operations_2026-08.md` — the largest
  known delta, planned in full.
- `docs/todo/AUTH_TODO.md` § "Open — security posture" — tracker.
