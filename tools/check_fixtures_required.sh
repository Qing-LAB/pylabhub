#!/usr/bin/env bash
#
# check_fixtures_required.sh — enforce that every gtest_discover_tests
# call in the test tree includes `FIXTURES_REQUIRED Guardrails`.
#
# **Why this exists.**  Guardrails only run before other tests when
# CTest sees `FIXTURES_REQUIRED Guardrails` on those tests.  If a new
# test target's `gtest_discover_tests(...)` block omits the property,
# CTest silently runs that test WITHOUT its guardrail dependency —
# the exact bypass that closed 2026-07-12's evidence-discipline arc
# needs to prevent going forward.  This guardrail scans every
# CMakeLists.txt under tests/ for gtest_discover_tests calls and
# fails the build if any is missing FIXTURES_REQUIRED.
#
# **PowerShell mirror**: tools/check_fixtures_required.ps1.  Both
# scripts MUST be kept semantically equivalent.
#
# Usage (via ctest label `guardrail`):
#   ctest -L guardrail
#
# Manual:
#   tools/check_fixtures_required.sh                # repo root
#   tools/check_fixtures_required.sh /path/to/tree  # explicit tree
#
# Exit codes:
#   0 — clean (all gtest_discover_tests calls include the requirement)
#   1 — at least one call is missing the requirement

set -eu

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

fail=0
declare -a violations

# Walk every CMakeLists.txt under tests/ (excluding test_framework
# which houses no tests of its own) and check every
# gtest_discover_tests block for FIXTURES_REQUIRED Guardrails.
#
# awk state machine:
#   - When a line matches `^gtest_discover_tests(`, start a block.
#   - Accumulate lines until matching `)` at line start or in-line.
#   - When the block closes, check if it contains
#     `FIXTURES_REQUIRED Guardrails`; if not, report the block's
#     first line with a file:line: prefix.
#   - Skip COMMENT lines (leading `#` in CMake) so commented-out
#     example blocks don't trigger false positives.

while IFS= read -r f; do
    awk -v file="$f" '
        function report(line, first_line) {
            print file ":" line ":" first_line
        }
        # Start of a gtest_discover_tests call (skip comments).
        /^[[:space:]]*#/ { next }
        /^gtest_discover_tests\(/ {
            in_block = 1
            block_start = NR
            block_first_line = $0
            block_text = $0
            depth = gsub(/\(/, "(", $0) - gsub(/\)/, ")", $0)
            if (depth <= 0) {
                # Single-line call (rare).
                check_block()
                in_block = 0
            }
            next
        }
        in_block {
            block_text = block_text "\n" $0
            depth += gsub(/\(/, "(", $0) - gsub(/\)/, ")", $0)
            if (depth <= 0) {
                check_block()
                in_block = 0
            }
        }
        function check_block(   ok) {
            ok = (block_text ~ /FIXTURES_REQUIRED[[:space:]]+Guardrails/)
            if (!ok) {
                report(block_start, block_first_line)
            }
        }
    ' "$f"
done < <(find "$root/tests" -type f -name 'CMakeLists.txt' -not -path '*/test_framework/*') > /tmp/check_fixtures_$$.out

if [ -s /tmp/check_fixtures_$$.out ]; then
    echo "FAIL — gtest_discover_tests calls missing FIXTURES_REQUIRED Guardrails:" >&2
    cat /tmp/check_fixtures_$$.out >&2
    echo "" >&2
    echo "  Every gtest_discover_tests call MUST include" >&2
    echo "  'FIXTURES_REQUIRED Guardrails' in its PROPERTIES clause." >&2
    echo "  See docs/IMPLEMENTATION_GUIDANCE.md § 'Test Evidence Discipline'" >&2
    echo "  Rule 4 for the rationale." >&2
    fail=1
fi
rm -f /tmp/check_fixtures_$$.out

if [ "$fail" -eq 0 ]; then
    echo "OK — every gtest_discover_tests call has FIXTURES_REQUIRED Guardrails."
    exit 0
else
    exit 1
fi
