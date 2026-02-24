# demo.ps1 — run the pylabhub counter demo (broker + producer + consumer) on Windows
#
# Run explicitly from PowerShell (this script is intentionally not marked executable):
#   powershell -ExecutionPolicy Bypass -File share\scripts\demo.ps1 [-BuildType Debug|Release]
#
# For Linux/macOS use the companion bash script:
#   bash share/scripts/demo.sh
#
# Prerequisites:
#   cmake --build build
#   (python-build-standalone env must already be prepared by cmake)

param(
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$ExamplesDir = Join-Path $ScriptDir "python\examples"
$BinDir      = Join-Path $RepoRoot "build\stage-$BuildType\bin"

# ── Verify binaries ────────────────────────────────────────────────────────────
foreach ($Bin in @("pylabhub-broker.exe", "pylabhub-actor.exe")) {
    $Path = Join-Path $BinDir $Bin
    if (-not (Test-Path $Path)) {
        Write-Error "ERROR: $Path not found.`nRun: cmake --build build --target $($Bin -replace '\.exe','')"
        exit 1
    }
}

foreach ($Cfg in @("producer_counter.json", "consumer_logger.json")) {
    $Path = Join-Path $ExamplesDir $Cfg
    if (-not (Test-Path $Path)) {
        Write-Error "ERROR: $Path not found."
        exit 1
    }
}

# ── Process handles ────────────────────────────────────────────────────────────
$BrokerProc   = $null
$ProducerProc = $null

function Stop-Demo {
    Write-Host ""
    Write-Host "[demo] shutting down..."
    if ($null -ne $ProducerProc -and -not $ProducerProc.HasExited) {
        $ProducerProc.Kill()
        $ProducerProc.WaitForExit(3000) | Out-Null
        Write-Host "[demo] producer stopped (pid=$($ProducerProc.Id))"
    }
    if ($null -ne $BrokerProc -and -not $BrokerProc.HasExited) {
        $BrokerProc.Kill()
        $BrokerProc.WaitForExit(3000) | Out-Null
        Write-Host "[demo] broker stopped (pid=$($BrokerProc.Id))"
    }
    Write-Host "[demo] done."
}

# ── Start broker ───────────────────────────────────────────────────────────────
Write-Host "[demo] starting pylabhub-broker..."
$BrokerProc = Start-Process `
    -FilePath (Join-Path $BinDir "pylabhub-broker.exe") `
    -PassThru `
    -NoNewWindow

Start-Sleep -Milliseconds 500   # wait for broker to bind

if ($BrokerProc.HasExited) {
    Write-Error "ERROR: broker exited immediately — check logs."
    exit 1
}
Write-Host "[demo] broker running (pid=$($BrokerProc.Id))"

# ── Start producer ────────────────────────────────────────────────────────────
Write-Host "[demo] starting producer_counter..."
$ProducerProc = Start-Process `
    -FilePath (Join-Path $BinDir "pylabhub-actor.exe") `
    -ArgumentList "--config", (Join-Path $ExamplesDir "producer_counter.json") `
    -PassThru `
    -NoNewWindow

Start-Sleep -Milliseconds 700   # wait for producer SHM creation + broker registration

if ($ProducerProc.HasExited) {
    Write-Error "ERROR: producer exited immediately — check logs."
    Stop-Demo
    exit 1
}
Write-Host "[demo] producer running (pid=$($ProducerProc.Id))"

# ── Start consumer (foreground) ───────────────────────────────────────────────
Write-Host "[demo] starting consumer_logger... (Ctrl-C to stop)"
Write-Host ""

try {
    $ConsumerArgs = "--config", (Join-Path $ExamplesDir "consumer_logger.json")
    & (Join-Path $BinDir "pylabhub-actor.exe") @ConsumerArgs
}
finally {
    Stop-Demo
}
