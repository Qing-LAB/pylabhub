# ctest_evidence.ps1 — ctest wrapper that always preserves failure logs.
#
# PowerShell mirror of tools/ctest_evidence.sh.  The two scripts MUST be
# kept semantically equivalent.  See the .sh header for the full
# rationale.
#
# Usage:
#   powershell -File tools/ctest_evidence.ps1 -L layer4 -j 2
#   powershell -File tools/ctest_evidence.ps1 --rerun-failed
#
# Exit codes: propagates ctest's exit code.

param([Parameter(ValueFromRemainingArguments = $true)] [string[]]$CtestArgs)

$ErrorActionPreference = 'Stop'

$BuildDir = $env:PYLABHUB_BUILD_DIR
if (-not $BuildDir) {
    $BuildDir = Join-Path (Split-Path (Split-Path $PSCommandPath -Parent) -Parent) 'build'
}
if (-not (Test-Path $BuildDir)) {
    [Console]::Error.WriteLine("FAIL — build dir '$BuildDir' not found.")
    [Console]::Error.WriteLine("Set PYLABHUB_BUILD_DIR or run from repo root with 'build/' present.")
    exit 2
}

$LogsDir = Join-Path $BuildDir 'Testing/logs'
New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null

# Sub-second timestamp — plain seconds collide under parallel
# invocations.  .NET's DateTime.Ticks gives 100-ns resolution which
# is more than enough.
$Ts = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$Pid = [System.Diagnostics.Process]::GetCurrentProcess().Id
$LogPath      = Join-Path $LogsDir "ctest-$Ts-pid$Pid.log"
$BackupLast   = Join-Path $LogsDir "LastTest-preserved-$Ts-pid$Pid.log"
$BackupFailed = Join-Path $LogsDir "LastTestsFailed-preserved-$Ts-pid$Pid.log"

$TempDir = Join-Path $BuildDir 'Testing/Temporary'
$LastTest   = Join-Path $TempDir 'LastTest.log'
$LastFailed = Join-Path $TempDir 'LastTestsFailed.log'

# STEP 1 — Back up prior LastTest.log + LastTestsFailed.log.  Backup
# failure is FATAL: a silent evidence-preservation failure is exactly
# the anti-pattern this wrapper exists to prevent.
if (Test-Path $LastTest) {
    try {
        Copy-Item -Path $LastTest -Destination $BackupLast -Force
    } catch {
        [Console]::Error.WriteLine("FAIL [ctest_evidence] could not back up LastTest.log to $BackupLast")
        [Console]::Error.WriteLine("  $($_.Exception.Message)")
        [Console]::Error.WriteLine("  Refusing to run ctest — silent evidence-preservation failure is the")
        [Console]::Error.WriteLine("  exact anti-pattern this wrapper exists to prevent.")
        exit 3
    }
    [Console]::Error.WriteLine("[ctest_evidence] backed up LastTest.log -> $BackupLast")
}
if (Test-Path $LastFailed) {
    try {
        Copy-Item -Path $LastFailed -Destination $BackupFailed -Force
    } catch {
        [Console]::Error.WriteLine("FAIL [ctest_evidence] could not back up LastTestsFailed.log to $BackupFailed")
        [Console]::Error.WriteLine("  $($_.Exception.Message)")
        exit 3
    }
    [Console]::Error.WriteLine("[ctest_evidence] backed up LastTestsFailed.log -> $BackupFailed")
}

# STEP 2 — Assemble ctest args: preserve caller's, add missing flags.
$Args = @()
$HasOutputOnFailure = $false
$HasOutputLog = $false
if ($null -ne $CtestArgs) {
    foreach ($a in $CtestArgs) {
        if ($a -eq '--output-on-failure') { $HasOutputOnFailure = $true }
        if ($a -eq '--output-log' -or $a -like '--output-log=*') { $HasOutputLog = $true }
        $Args += $a
    }
}
if (-not $HasOutputOnFailure) { $Args += '--output-on-failure' }
if (-not $HasOutputLog) { $Args += '--output-log'; $Args += $LogPath }

[Console]::Error.WriteLine("[ctest_evidence] evidence log -> $LogPath")
[Console]::Error.WriteLine("[ctest_evidence] running: ctest $($Args -join ' ')")

# STEP 3 — Run ctest.
Push-Location $BuildDir
try {
    & ctest @Args
    $CtestRC = $LASTEXITCODE
} finally {
    Pop-Location
}

# STEP 4 — On failure, print evidence locations.
if ($CtestRC -ne 0) {
    [Console]::Error.WriteLine("")
    [Console]::Error.WriteLine("[ctest_evidence] ctest exited $CtestRC — evidence preserved:")
    if ((Test-Path $LogPath) -and ((Get-Item $LogPath).Length -gt 0)) {
        [Console]::Error.WriteLine("  ctest output log:            $LogPath")
    }
    if (Test-Path $LastTest) {
        [Console]::Error.WriteLine("  fresh LastTest.log:          $LastTest")
    }
    if (Test-Path $LastFailed) {
        [Console]::Error.WriteLine("  fresh LastTestsFailed.log:   $LastFailed")
    }
    if ((Test-Path $BackupLast) -and ((Get-Item $BackupLast).Length -gt 0)) {
        [Console]::Error.WriteLine("  prior-run LastTest backup:   $BackupLast")
    }
    if ((Test-Path $BackupFailed) -and ((Get-Item $BackupFailed).Length -gt 0)) {
        [Console]::Error.WriteLine("  prior-run failed-tests bkp:  $BackupFailed")
    }
    [Console]::Error.WriteLine("")
    [Console]::Error.WriteLine("[ctest_evidence] READ THE LOG BEFORE ANY --rerun-failed.  See")
    [Console]::Error.WriteLine("  docs/HEP/HEP-CORE-0011 § Session hygiene + feedback_read_log_before_rerun.")
}

exit $CtestRC
