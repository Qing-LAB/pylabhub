# HEP-CORE-0036 §I9.1 locality invariant guardrail (D-3).
#
# PowerShell mirror of `tools/check_layer_invariant.sh`.  The two
# scripts MUST be kept semantically equivalent — every rule added to
# one MUST be mirrored in the other.  If you edit this file, the
# reciprocal edit lives in `check_layer_invariant.sh`; if you edit
# the .sh, mirror your change here.  See HEP-CORE-0036 §I9.1
# "Enforcement" for the full description.
#
# Usage:
#   powershell -File tools/check_layer_invariant.ps1              # repo root
#   powershell -File tools/check_layer_invariant.ps1 C:\src\pylab # explicit tree
#
# Exit codes:
#   0 — clean (no violation)
#   1 — at least one violation found

param([string]$Root = "")

if (-not $Root) {
    $Root = Split-Path (Split-Path $PSCommandPath -Parent) -Parent
}

# Role-side + framework-level files bound by §I9.1.  MUST MIRROR the
# `role_paths` array in check_layer_invariant.sh.  Queue-layer files
# (src\utils\hub, src\include\utils\hub_queue.hpp,
# hub_zmq_queue.hpp, ...) are INTENTIONALLY OUT OF SCOPE — that
# layer owns topology/transport facts and may introspect them
# freely.  BRC internals also out of scope.
$RolePaths = @(
    (Join-Path $Root 'src\producer'),
    (Join-Path $Root 'src\consumer'),
    (Join-Path $Root 'src\processor'),
    (Join-Path $Root 'src\include\utils\role_api_base.hpp'),
    (Join-Path $Root 'src\include\utils\role_host_helpers.hpp'),
    (Join-Path $Root 'src\include\utils\role_host_core.hpp'),
    (Join-Path $Root 'src\utils\service\role_api_base.cpp'),
    (Join-Path $Root 'src\utils\service\role_host_frame.cpp'),
    (Join-Path $Root 'src\utils\service\cycle_ops.hpp'),
    (Join-Path $Root 'src\utils\service\data_loop.hpp'),
    (Join-Path $Root 'src\utils\service\native_engine.cpp')
)

# Search-Role: scan the role-side paths for a regex.  Returns an
# array of "path:line:content" strings.  Drops lines that look like
# single-line // comments or block * continuations.  Mirror of the
# `grep_role` helper in the .sh.
function Search-Role {
    param([string]$Pattern)
    $results = @()
    foreach ($p in $RolePaths) {
        if (-not (Test-Path $p)) { continue }
        $item = Get-Item $p
        if ($item.PSIsContainer) {
            $files = Get-ChildItem -Path $p -Recurse `
                     -Include '*.cpp','*.hpp','*.h' -ErrorAction SilentlyContinue
        } else {
            $files = @($item)
        }
        foreach ($f in $files) {
            $matches = Select-String -Path $f.FullName -Pattern $Pattern `
                       -ErrorAction SilentlyContinue
            foreach ($m in $matches) {
                if ($m.Line -match '^\s*(//|\*)') { continue }
                $results += "{0}:{1}:{2}" -f $m.Path, $m.LineNumber, $m.Line
            }
        }
    }
    return $results
}

$script:Fail = 0

function Report {
    param([string]$Reason, [array]$Hits)
    if ($null -ne $Hits -and $Hits.Count -gt 0) {
        [Console]::Error.WriteLine("FAIL — HEP-CORE-0036 §I9.1: $Reason")
        $Hits | ForEach-Object { [Console]::Error.WriteLine($_) }
        [Console]::Error.WriteLine("")
        $script:Fail = 1
    }
}

# ── 1. topology::parse in role code
Report "direct topology::parse in role code" `
    (Search-Role 'topology::parse\(')

# ── 2. ChannelTopology enum comparisons in role code
# Struct-field default values (e.g., `ChannelTopology topology
# {ChannelTopology::OneToOne}` in TxQueueOptions / RxQueueOptions)
# are legitimate — those structs carry the topology into the queue
# factory, which is where the branch belongs.
Report "ChannelTopology enum comparison in role code" `
    (Search-Role '(==|!=|>=|<=|<|>)\s*ChannelTopology::(FanIn|FanOut|OneToOne)\b')
Report "ChannelTopology as if-condition in role code" `
    (Search-Role 'ChannelTopology::(FanIn|FanOut|OneToOne)\s*(==|!=|>=|<=|<|>)')
Report "ChannelTopology in switch/case in role code" `
    (Search-Role 'case\s+ChannelTopology::(FanIn|FanOut|OneToOne)\b')

# ── 3. is_binding_side() at all in role-side code
# §I9.1: queue may expose is_binding_side() for logging; callers
# MUST NOT branch on it.  Role code uses binding_role_type() when
# it needs to ask "am I binding" (non-empty return means yes).
Report "raw is_binding_side() in role code (use binding_role_type() per §I9.1)" `
    (Search-Role '\bis_binding_side\(\)')

# ── 4. Retired method / helper names re-appearing
# These were retired in commit c665de0c.  Re-adding them re-opens
# the layer breakage the arc closed.
Report "retired dial_now (replaced by finalize_channel_connect)" `
    (Search-Role '\bdial_now\s*\(')
Report "retired wait_for_peer_ready (queue owns finalize_connect polling)" `
    (Search-Role '\bwait_for_peer_ready\s*\(')
Report "retired channel_auth_applied_consumer (consolidated with role_type param)" `
    (Search-Role '\bchannel_auth_applied_consumer\s*\(')

# ── 5. Retired public API resurrection on RoleAPIBase header
$hdr = Join-Path $Root 'src\include\utils\role_api_base.hpp'
if (Test-Path $hdr) {
    $checkPeerReady = Select-String -Path $hdr `
                      -Pattern '^\s*(bool|std::optional|void)\s+check_peer_ready\b' `
                      -ErrorAction SilentlyContinue |
                      Where-Object { $_.Line -notmatch '^\s*(//|\*)' } |
                      ForEach-Object { "{0}:{1}:{2}" -f $_.Path, $_.LineNumber, $_.Line }
    Report "check_peer_ready re-declared on RoleAPIBase public surface" $checkPeerReady

    $dialNow = Select-String -Path $hdr `
               -Pattern '^\s*(bool|std::optional|void)\s+dial_now\b' `
               -ErrorAction SilentlyContinue |
               Where-Object { $_.Line -notmatch '^\s*(//|\*)' } |
               ForEach-Object { "{0}:{1}:{2}" -f $_.Path, $_.LineNumber, $_.Line }
    Report "dial_now re-declared on RoleAPIBase public surface" $dialNow
}

if ($script:Fail -eq 0) {
    Write-Output "OK — HEP-CORE-0036 §I9.1 locality invariant clean."
    exit 0
} else {
    [Console]::Error.WriteLine("")
    [Console]::Error.WriteLine("See docs/HEP/HEP-CORE-0036 §I9.1 for the invariant text.")
    [Console]::Error.WriteLine("See docs/archive/transient-2026-07-12/DRAFT_queue_owned_topology_and_layer_cleanup_2026-07-11.md")
    [Console]::Error.WriteLine("for the arc that closed these violations (2026-07-12).")
    exit 1
}
