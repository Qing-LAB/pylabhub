# pylabhub-pyenv.ps1 — Manage the pyLabHub bundled Python environment (Windows).
#
# This wrapper locates the bundled standalone Python interpreter and
# invokes pylabhub-pyenv.py with it.  It works in both the source tree
# (tools/) and the staged/installed layout (bin/).
#
# Usage:
#   .\pylabhub-pyenv.ps1 install [-r requirements.txt] [--wheels-dir DIR]
#   .\pylabhub-pyenv.ps1 verify  [-r requirements.txt]
#   .\pylabhub-pyenv.ps1 info
#   .\pylabhub-pyenv.ps1 freeze
#
# Environment variables:
#   PYLABHUB_PYTHON  — Override path to the Python interpreter.
#                      Default: auto-detect from <prefix>\opt\python\python.exe

[CmdletBinding()]
param(
    [Parameter(Position=0, ValueFromRemainingArguments=$true)]
    [string[]]$Arguments
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PyenvScript = Join-Path $ScriptDir "pylabhub-pyenv.py"

# --- Locate the Python interpreter ---
if ($env:PYLABHUB_PYTHON -and (Test-Path $env:PYLABHUB_PYTHON)) {
    $Python = $env:PYLABHUB_PYTHON
} else {
    # Staged/installed layout: bin\..\opt\python\python.exe
    $Candidate = Join-Path (Split-Path $ScriptDir -Parent) "opt\python\python.exe"
    if (Test-Path $Candidate) {
        $Python = $Candidate
    } else {
        Write-Error @"
ERROR: Cannot find the bundled Python interpreter.
Expected at: $Candidate
Set `$env:PYLABHUB_PYTHON = 'C:\path\to\python.exe' to override.
"@
        exit 1
    }
}

$Python = (Resolve-Path $Python).Path

# --- Locate the .py script ---
if (-not (Test-Path $PyenvScript)) {
    Write-Error "ERROR: Cannot find pylabhub-pyenv.py at: $PyenvScript"
    exit 1
}

# --- Run ---
& $Python $PyenvScript @Arguments
exit $LASTEXITCODE
