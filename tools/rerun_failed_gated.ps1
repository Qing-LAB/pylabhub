# rerun_failed_gated.ps1 — gated `ctest --rerun-failed` wrapper.
#
# PowerShell mirror of tools/rerun_failed_gated.sh.  The two scripts
# MUST be kept semantically equivalent.  See the .sh header for the
# full rationale and the enforcement contract.

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Command,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)

$ErrorActionPreference = 'Stop'

$BuildDir = $env:PYLABHUB_BUILD_DIR
if (-not $BuildDir) {
    $BuildDir = Join-Path (Split-Path (Split-Path $PSCommandPath -Parent) -Parent) 'build'
}
$LogsDir  = Join-Path $BuildDir 'Testing/logs'
$Marker   = Join-Path $LogsDir '.evidence-read'
$LastTest = Join-Path $BuildDir 'Testing/Temporary/LastTest.log'

function Usage {
    [Console]::Error.WriteLine("usage: rerun_failed_gated.ps1 read '<summary text>'")
    [Console]::Error.WriteLine("       rerun_failed_gated.ps1 run  [extra ctest args...]")
    exit 2
}

switch ($Command) {
    'read' {
        if (-not $Rest -or $Rest.Count -eq 0 -or -not $Rest[0]) {
            [Console]::Error.WriteLine("FAIL — 'read' requires a non-empty summary argument.")
            [Console]::Error.WriteLine("  Example:")
            [Console]::Error.WriteLine("  rerun_failed_gated.ps1 read 'producer log shows checksum error at slot N=3'")
            exit 1
        }
        New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null
        $Now = Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz'
        $Lines = @(
            "read_at: $Now",
            "summary: $($Rest -join ' ')"
        )
        if (Test-Path $LastTest) {
            $Mtime = (Get-Item $LastTest).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss zzz')
            $Lines += "referenced_LastTest_mtime: $Mtime"
        }
        $Lines | Set-Content -Path $Marker -Encoding UTF8
        [Console]::Error.WriteLine("[rerun_failed_gated] marker written: $Marker")
        exit 0
    }
    'run' {
        if (-not (Test-Path $Marker)) {
            [Console]::Error.WriteLine("REFUSED — evidence-read marker missing.")
            [Console]::Error.WriteLine("  Expected: $Marker")
            [Console]::Error.WriteLine("")
            [Console]::Error.WriteLine("  Read the failure evidence FIRST:")
            if (Test-Path $LastTest) {
                [Console]::Error.WriteLine("    Get-Content $LastTest")
            }
            $Artifacts = Join-Path $BuildDir 'stage-debug/test_artifacts'
            if (Test-Path $Artifacts) {
                [Console]::Error.WriteLine("    Get-ChildItem $Artifacts")
            }
            [Console]::Error.WriteLine("")
            [Console]::Error.WriteLine("  Then record what you found:")
            [Console]::Error.WriteLine("    rerun_failed_gated.ps1 read '<one-line summary of the failure>'")
            [Console]::Error.WriteLine("")
            [Console]::Error.WriteLine("  Only after the marker is written may you rerun.")
            exit 1
        }
        if (Test-Path $LastTest) {
            $LastTime = (Get-Item $LastTest).LastWriteTime
            $MarkerTime = (Get-Item $Marker).LastWriteTime
            if ($LastTime -gt $MarkerTime) {
                [Console]::Error.WriteLine("REFUSED — evidence-read marker is STALE.")
                [Console]::Error.WriteLine("  marker:    $Marker")
                [Console]::Error.WriteLine("  LastTest:  $LastTest (newer than marker)")
                [Console]::Error.WriteLine("")
                [Console]::Error.WriteLine("  Refresh the marker via 'read ...' before retrying.")
                exit 1
            }
        }
        $Ts = Get-Date -Format 'yyyyMMdd-HHmmss'
        $Backup = Join-Path $LogsDir "LastTest-pre-rerun-$Ts.log"
        if (Test-Path $LastTest) {
            New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null
            Copy-Item -Path $LastTest -Destination $Backup -Force
            [Console]::Error.WriteLine("[rerun_failed_gated] pre-rerun backup: $Backup")
        }
        Push-Location $BuildDir
        try {
            $Args = @('--rerun-failed', '--output-on-failure')
            if ($null -ne $Rest) { $Args += $Rest }
            & ctest @Args
            $RC = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        [Console]::Error.WriteLine("")
        [Console]::Error.WriteLine("[rerun_failed_gated] rerun complete (exit $RC).")
        [Console]::Error.WriteLine("  pre-rerun log preserved: $Backup")
        exit $RC
    }
    default {
        Usage
    }
}
