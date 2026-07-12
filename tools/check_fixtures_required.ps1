# check_fixtures_required.ps1 — enforce every gtest_discover_tests
# call includes FIXTURES_REQUIRED Guardrails.
#
# PowerShell mirror of tools/check_fixtures_required.sh.  The two
# scripts MUST be kept semantically equivalent.  See .sh header for
# rationale.

param([string]$Root = "")

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    $Root = Split-Path (Split-Path $PSCommandPath -Parent) -Parent
}

$Violations = @()

$Files = Get-ChildItem -Path (Join-Path $Root 'tests') -Recurse `
                       -Filter 'CMakeLists.txt' -File `
                       -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notmatch [regex]::Escape('test_framework') }

foreach ($f in $Files) {
    $lines = Get-Content -Path $f.FullName
    $inBlock = $false
    $blockStart = 0
    $blockFirstLine = ''
    $blockText = ''
    $depth = 0
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        # Skip comment lines (leading # in CMake).
        if ($line -match '^\s*#') { continue }
        if (-not $inBlock -and $line -match '^gtest_discover_tests\(') {
            $inBlock = $true
            $blockStart = $i + 1
            $blockFirstLine = $line
            $blockText = $line
            $depth = ($line.ToCharArray() | Where-Object { $_ -eq '(' }).Count -
                     ($line.ToCharArray() | Where-Object { $_ -eq ')' }).Count
            if ($depth -le 0) {
                if ($blockText -notmatch 'FIXTURES_REQUIRED\s+Guardrails') {
                    $Violations += "$($f.FullName):$blockStart`:$blockFirstLine"
                }
                $inBlock = $false
            }
            continue
        }
        if ($inBlock) {
            $blockText = $blockText + "`n" + $line
            $depth += ($line.ToCharArray() | Where-Object { $_ -eq '(' }).Count -
                      ($line.ToCharArray() | Where-Object { $_ -eq ')' }).Count
            if ($depth -le 0) {
                if ($blockText -notmatch 'FIXTURES_REQUIRED\s+Guardrails') {
                    $Violations += "$($f.FullName):$blockStart`:$blockFirstLine"
                }
                $inBlock = $false
            }
        }
    }
}

if ($Violations.Count -gt 0) {
    [Console]::Error.WriteLine("FAIL — gtest_discover_tests calls missing FIXTURES_REQUIRED Guardrails:")
    foreach ($v in $Violations) { [Console]::Error.WriteLine($v) }
    [Console]::Error.WriteLine("")
    [Console]::Error.WriteLine("  Every gtest_discover_tests call MUST include")
    [Console]::Error.WriteLine("  'FIXTURES_REQUIRED Guardrails' in its PROPERTIES clause.")
    [Console]::Error.WriteLine("  See docs/IMPLEMENTATION_GUIDANCE.md § 'Test Evidence Discipline'")
    [Console]::Error.WriteLine("  Rule 4 for the rationale.")
    exit 1
}

Write-Output "OK — every gtest_discover_tests call has FIXTURES_REQUIRED Guardrails."
exit 0
