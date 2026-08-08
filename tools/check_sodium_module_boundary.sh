#!/usr/bin/env bash
#
# HEP-CORE-0043 security-module boundary guardrail.
#
# ONE MODULE OWNS libsodium.  Every `#include <sodium.h>` in production
# code must live inside a `security/` directory; every other translation
# unit reaches crypto through `secure()` (SecureSubsystem) or
# `secure().keys()` (KeyStore).
#
# WHY THIS IS ENFORCED RATHER THAN DOCUMENTED.  The property is true
# today — all production sodium includes are already inside the module —
# but nothing was stopping it from decaying.  A single `#include
# <sodium.h>` added to a file elsewhere would create a second crypto
# surface with its own init assumptions, its own buffer handling, and
# no `LockedKey` / `memzero` discipline, and no test would fail.  That
# is exactly how the first `sodium_init()` ordering bug reached CI.
# The rule was achieved once; this keeps it.
#
# SCOPE IS `src/` DELIBERATELY.  Test code may include <sodium.h>
# directly (four files do — attach-protocol and key-store tests build
# raw inputs the module is designed to reject).  Tests are not the
# production module and are not bound by its boundary.
#
# Usage:
#   tools/check_sodium_module_boundary.sh           # check repo root
#   tools/check_sodium_module_boundary.sh /path     # check specific tree
#
# Exit codes:
#   0 — clean (no production sodium include outside the module)
#   1 — at least one include escaped the module
#
# PowerShell mirror: tools/check_sodium_module_boundary.ps1.  The two
# scripts MUST be kept semantically equivalent — every rule added to
# one MUST be added to the other.

set -eu

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

# Any spelling of the include: <sodium.h> or a sodium/ subheader,
# with or without whitespace after `#`.
forbidden_re='^[[:space:]]*#[[:space:]]*include[[:space:]]*<sodium(/[^>]*)?\.h>'

# Exclusion-shaped on purpose (contrast check_auth_guardrail.sh, which
# greps a fixed path list): scan ALL of src/ and subtract the module,
# so a new file in a new directory is covered automatically instead of
# escaping a list nobody remembered to update.
hits=$(grep -rnE "$forbidden_re" "$root/src" \
       --include='*.cpp' --include='*.hpp' --include='*.h' 2>/dev/null \
       | grep -v '/security/' || true)

if [[ -n "$hits" ]]; then
    echo "FAIL — libsodium included outside the security module:" >&2
    echo "$hits" >&2
    echo "" >&2
    echo "One module owns libsodium (HEP-CORE-0043).  Reach crypto via" >&2
    echo "  sec::secure().<op>(...)        — random_bytes, memzero," >&2
    echo "                                   secretbox_*, pwhash_*, memcmp_ct" >&2
    echo "  sec::secure().keys()           — KeyStore (LockedKey-backed)" >&2
    echo "" >&2
    echo "If an operation genuinely has no wrapper, ADD IT TO THE MODULE" >&2
    echo "rather than including <sodium.h> at the call site — see" >&2
    echo "src/utils/service/vault_crypto.cpp for a consumer that needed" >&2
    echo "pwhash + secretbox and got them this way." >&2
    exit 1
fi

echo "OK — HEP-CORE-0043: no libsodium include outside the security module."
