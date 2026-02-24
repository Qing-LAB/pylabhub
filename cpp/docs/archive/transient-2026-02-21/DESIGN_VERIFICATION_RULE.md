# Design and Documentation Verification Rule

**Document ID**: RULE-VERIFY-001  
**Version**: 1.0.0  
**Status**: Mandatory  
**Applies to**: All design docs, API docs, and implementation claims

---

## The Rule

**All API and design claims MUST be verified against actual code before being stated as fact in documentation.**

- Documentation does **not** mean the behavior is implemented. Claims must be checked against source.
- If a design or API is **confirmed implemented**, the document MUST include a **verification checkbox** (e.g. `- [x]`) with a **code reference** (file:line or function name).
- If a claim is **not yet verified**, the document MUST either:
  - Mark it as unverified (e.g. `- [ ]` with "Not yet verified against code"), or
  - Omit the claim until verification is done.
- **Be suspicious** of any documented behavior that has no verification checkbox or code reference.

---

## Verification Checkbox Format

In design/API documentation, use this format:

```markdown
### Feature: Dual-schema storage in producer

- [x] **Verified** – Producer template stores both schema hashes in header.
  - Code: `data_block.cpp` `create_datablock_producer_impl()` lines 3069–3095:
    - `flexzone_schema != nullptr` → `memcpy(header->flexzone_schema_hash, ...)`
    - `datablock_schema != nullptr` → `memcpy(header->datablock_schema_hash, ...)`
  - Call path: `create_datablock_producer<F,D>()` (header ~1416) → `generate_schema_info<F>`, `generate_schema_info<D>` → `create_datablock_producer_impl(..., &flexzone_schema, &datablock_schema)`.
```

For **unverified** or **planned** behavior:

```markdown
### Feature: Consumer throws on schema mismatch

- [ ] **Not verified** – Doc claimed "throws SchemaValidationException"; actual code returns `nullptr` (see DESIGN_VERIFICATION_CHECKLIST.md).
```

---

## What Must Be Verified

1. **API contracts**: Parameters, return values, thrown exceptions, error codes.
2. **Design invariants**: "Schema validation always runs", "Config is required", etc.
3. **Code paths**: "When template is used, X happens" – trace to actual `if (x != nullptr)` or equivalent.
4. **Cross-layer behavior**: C API vs C++ API; what is stored in shared memory vs what is validated.

---

## Where This Rule Is Applied

- **CODE_REVIEW_GUIDANCE.md** – Reviewers must check design/API claims against code (see §2 and §3).
- **IMPLEMENTATION_GUIDANCE.md** – Implementers must not document behavior without verifying it in code.
- **All design docs** (e.g. PHASE4_DUAL_SCHEMA_API_DESIGN.md, API_SURFACE_DOCUMENTATION.md) – Must include verification checkboxes and code references for implemented behavior; unverified claims must be marked.
- **DESIGN_VERIFICATION_CHECKLIST.md** – Central checklist of "design claim vs actual code"; updated when code or design changes.

---

## Summary

| Principle | Action |
|-----------|--------|
| Trust only code | Every design/API claim must be traced to actual source. |
| Document only what is verified | Use `[x]` + code ref for confirmed behavior. |
| Unverified = suspicious | Use `[ ]` or "Not verified" until checked. |
| Single source of truth | Code is the truth; docs must match code. |

---

**Document Control**  
Created: 2026-02-15  
Maintained by: Core team  
Review: When updating any design or API documentation
