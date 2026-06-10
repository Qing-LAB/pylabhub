#!/usr/bin/env bash
#
# HEP-CORE-0036 §I11 audit S3 guardrail.
#
# Verifies that no script-side surface exposes an allowlist MUTATOR.
# The framework remains the only mutator of the producer's allowlist
# (broker-pull → atomic-apply path per §6.5).  Scripts read via
# `api.allowed_peers` and observe via `on_allowlist_changed`; they
# must NEVER be given `api.set_*peers*` or similar.
#
# Usage:
#   tools/check_auth_guardrail.sh           # check repo root
#   tools/check_auth_guardrail.sh /path     # check specific tree
#
# Exit codes:
#   0 — clean (no forbidden symbol bound)
#   1 — at least one forbidden symbol found in script-binding surface

set -eu

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

# Patterns forbidden in script-binding files.  Conservative: matches
# anything that looks like a setter named after "peers" or "allowlist".
# Note: framework-internal `ZmqQueue::set_peer_allowlist` is the
# AUTHORIZED mutator and lives outside the searched paths; it is NOT a
# script binding.
forbidden_re='\bset_(allowed_peers|peer_allowlist|allowlist|authorized_peers)\b'

# Search ONLY in files that bind script-visible API surface.  These are
# the gateways through which a script could reach a mutator.  ZmqQueue
# itself is intentionally OUT of scope — it's framework internal.
search_paths=(
    "$root/src/scripting"
    "$root/src/producer/producer_api.hpp"
    "$root/src/producer/producer_api.cpp"
    "$root/src/consumer/consumer_api.hpp"
    "$root/src/consumer/consumer_api.cpp"
    "$root/src/processor/processor_api.hpp"
    "$root/src/processor/processor_api.cpp"
)

hits=$(grep -rnE "$forbidden_re" "${search_paths[@]}" \
       --include='*.cpp' --include='*.hpp' --include='*.h' 2>/dev/null || true)

if [[ -n "$hits" ]]; then
    echo "FAIL — HEP-CORE-0036 §I11 audit S3 violation:" >&2
    echo "$hits" >&2
    echo "" >&2
    echo "Scripts must NOT be exposed to allowlist mutators.  The" >&2
    echo "framework is the only authorized mutator (§6.5 producer-side" >&2
    echo "flow: BRC pull → ZmqQueue::set_peer_allowlist)." >&2
    exit 1
fi

echo "OK — HEP-CORE-0036 §I11 audit S3: no script-side allowlist mutator bound."
