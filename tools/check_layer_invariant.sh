#!/usr/bin/env bash
#
# HEP-CORE-0036 §I9.1 locality invariant guardrail (D-3).
#
# Fails the build when role-side code contains any of the forbidden
# patterns.  Topology and transport are queue-internal per §I9.1;
# role-host code, RoleAPIBase, and framework-level layers use
# topology-agnostic verbs only.
#
# **PowerShell mirror**: `tools/check_layer_invariant.ps1`.  The two
# scripts MUST be kept semantically equivalent — every rule added to
# one MUST be mirrored in the other.  CMake dispatches the
# platform-appropriate script at test time (see
# `tests/CMakeLists.txt`).  See HEP-CORE-0036 §I9.1 "Enforcement"
# for the full description.
#
# Usage:
#   tools/check_layer_invariant.sh           # check repo root
#   tools/check_layer_invariant.sh /path     # check specific tree
#
# Exit codes:
#   0 — clean (no violation)
#   1 — at least one violation found

set -eu

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

# Role-side + framework-level files bound by §I9.1.  The queue layer
# (src/utils/hub/, src/include/utils/hub_queue.hpp,
#  src/include/utils/hub_zmq_queue.hpp, etc.) is INTENTIONALLY OUT OF
# SCOPE — that layer owns the topology/transport facts and may
# introspect them freely.  BRC internals
# (src/utils/network_comm/broker_request_comm.cpp) also out of scope —
# BRC hosts the wire methods the queue-side oracle uses.
role_paths=(
    "$root/src/producer"
    "$root/src/consumer"
    "$root/src/processor"
    "$root/src/include/utils/role_api_base.hpp"
    "$root/src/include/utils/role_host_helpers.hpp"
    "$root/src/include/utils/role_host_core.hpp"
    "$root/src/utils/service/role_api_base.cpp"
    "$root/src/utils/service/role_host_frame.cpp"
    "$root/src/utils/service/cycle_ops.hpp"
    "$root/src/utils/service/data_loop.hpp"
    "$root/src/utils/service/native_engine.cpp"
)

# grep_role — scan role-side paths for a regex, drop comments and log
# strings (single-line `//`, `/*`, and lines containing "OR" in
# free-form comment context).  A conservative filter — false positives
# are documented and can be silenced by moving the comment mention off
# the same line as any word matching the pattern.
grep_role() {
    local pattern="$1"
    grep -rnE "$pattern" "${role_paths[@]}" \
        --include='*.cpp' --include='*.hpp' --include='*.h' \
        2>/dev/null \
        | grep -v -E '^[^:]*:[0-9]+:\s*(//|\*)' \
        || true
}

fail=0
report() {
    local reason="$1"
    local hits="$2"
    if [[ -n "$hits" ]]; then
        echo "FAIL — HEP-CORE-0036 §I9.1: $reason" >&2
        echo "$hits" >&2
        echo "" >&2
        fail=1
    fi
}

# ── 1. topology::parse in role code
# Role code MUST NOT parse topology strings.  Config-to-topology
# resolution is a queue-factory concern (hub_queue_factory).
report "direct topology::parse in role code" \
    "$(grep_role 'topology::parse\(')"

# ── 2. ChannelTopology enum comparisons in role code
# Role code MUST NOT branch on ChannelTopology::FanIn/FanOut/OneToOne
# — comparisons and switch cases only.  Struct-field default values
# (e.g., `ChannelTopology topology{ChannelTopology::OneToOne}` in
# TxQueueOptions / RxQueueOptions) are legitimate — those structs
# carry the topology into the QUEUE factory, which is where the
# branch belongs.
report "ChannelTopology enum comparison in role code" \
    "$(grep_role '(==|!=|>=|<=|<|>)\s*ChannelTopology::(FanIn|FanOut|OneToOne)\b')"
report "ChannelTopology as if-condition in role code" \
    "$(grep_role 'ChannelTopology::(FanIn|FanOut|OneToOne)\s*(==|!=|>=|<=|<|>)')"
report "ChannelTopology in switch/case in role code" \
    "$(grep_role 'case\s+ChannelTopology::(FanIn|FanOut|OneToOne)\b')"

# ── 3. is_binding_side() at all in role-side code
# §I9.1: queue may expose is_binding_side() for logging; callers
# MUST NOT branch on it.  Role code uses binding_role_type() when
# it needs to ask "am I binding" (non-empty return means yes).
report "raw is_binding_side() in role code (use binding_role_type() per §I9.1)" \
    "$(grep_role '\bis_binding_side\(\)')"

# ── 4. Retired method / helper names re-appearing
# These were retired in commit c665de0c.  Re-adding them re-opens the
# layer breakage the arc closed.
report "retired dial_now (replaced by finalize_channel_connect)" \
    "$(grep_role '\bdial_now\s*\(')"
report "retired wait_for_peer_ready (queue owns finalize_connect polling)" \
    "$(grep_role '\bwait_for_peer_ready\s*\(')"
report "retired channel_auth_applied_consumer (consolidated with role_type param)" \
    "$(grep_role '\bchannel_auth_applied_consumer\s*\(')"

# ── 5. Retired public API resurrection on RoleAPIBase headers
# Even if BRC methods keep check_peer_ready internally, the PUBLIC
# RoleAPIBase surface must not re-expose it.  Check headers only.
hits=$(grep -nE '^\s*(bool|std::optional|void)\s+check_peer_ready\b' \
        "$root/src/include/utils/role_api_base.hpp" 2>/dev/null \
        | grep -v -E ':\s*(//|\*)' || true)
report "check_peer_ready re-declared on RoleAPIBase public surface" "$hits"

hits=$(grep -nE '^\s*(bool|std::optional|void)\s+dial_now\b' \
        "$root/src/include/utils/role_api_base.hpp" 2>/dev/null \
        | grep -v -E ':\s*(//|\*)' || true)
report "dial_now re-declared on RoleAPIBase public surface" "$hits"

if [[ $fail -eq 0 ]]; then
    echo "OK — HEP-CORE-0036 §I9.1 locality invariant clean."
else
    echo "" >&2
    echo "See docs/HEP/HEP-CORE-0036 §I9.1 for the invariant text." >&2
    echo "See docs/archive/transient-2026-07-12/DRAFT_queue_owned_topology_and_layer_cleanup_2026-07-11.md" >&2
    echo "for the arc that closed these violations (2026-07-12)." >&2
    exit 1
fi
