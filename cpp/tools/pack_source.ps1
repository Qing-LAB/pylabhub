<# 
tools/pack_source.ps1
--------------------------------------------------------------------
Archives project source files into a gzipped tarball.

- Uses 'git ls-files' to reliably list all tracked files.
- Excludes third_party/ (except third_party/CMakeLists.txt and third_party/cmake/**),
  build artifacts, IDE folders, and common archive files.
- Packages sources under a versioned root directory.
- If tar supports --transform, use it; otherwise create a staging tree.
--------------------------------------------------------------------
#>

[CmdletBinding()]
param(
    [string]$Output,
    [string]$SourceDir,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'

function Show-Usage {
    $msg = @"
Usage: pack_source.ps1 [-Output <output_file>] [-SourceDir <source_dir>] [-Help]

Packs project source files into a .tar.gz archive.
This script relies on 'git ls-files' and must be run within a Git repository.

Options:
  -Output     Specify the output archive file path.
  -SourceDir  Source directory to archive (project root).
  -Help       Show this help message.
"@
    Write-Host $msg
}

if ($Help) {
    Show-Usage
    exit 0
}

# --- Determine project root (one level up from script) ---
$ScriptPath = $MyInvocation.MyCommand.Path
if (-not $ScriptPath) {
    # When run interactively, MyInvocation.MyCommand.Path can be empty
    $ScriptDir = (Get-Location).Path
} else {
    $ScriptDir = Split-Path -Parent $ScriptPath
}
try {
    $ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
} catch {
    $ProjectRoot = Resolve-Path $ScriptDir
}

if (-not $SourceDir) {
    $SourceDir = $ProjectRoot
} else {
    try {
        $SourceDir = Resolve-Path $SourceDir
    } catch {
        # allow relative paths by resolving against current dir
        $SourceDir = Resolve-Path (Join-Path (Get-Location) $SourceDir)
    }
}

$ProjectName = Split-Path $ProjectRoot -Leaf

if (-not $Output) {
    $Output = Join-Path $ProjectRoot "$ProjectName-src.tar.gz"
} else {
    try {
        $resolvedOut = Resolve-Path -LiteralPath $Output -ErrorAction SilentlyContinue
        if ($resolvedOut) {
            $Output = $resolvedOut.Path
        } else {
            $Output = (Resolve-Path (Join-Path (Get-Location) $Output)).Path
        }
    } catch {
        if (-not [System.IO.Path]::IsPathRooted($Output)) {
            $Output = Join-Path (Get-Location) $Output
        }
    }
}

# --- Pre-run checks ---
Set-Location $SourceDir

# ensure we are in a git repo
& git rev-parse --is-inside-work-tree *> $null 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "Source directory '$SourceDir' is not a git repository."
    exit 1
}

# Normalize PathInfo vs string for comparisons below
if ($SourceDir -is [System.Management.Automation.PathInfo]) { $SourceDirPath = $SourceDir.Path } else { $SourceDirPath = $SourceDir }
if ($ProjectRoot -is [System.Management.Automation.PathInfo]) { $ProjectRootPath = $ProjectRoot.Path } else { $ProjectRootPath = $ProjectRoot }

if ($SourceDirPath -ne $ProjectRootPath) {
    Write-Warning "Specified source directory differs from detected project root."
    Write-Warning "  Script implies root: $ProjectRootPath"
    Write-Warning "  Provided source:     $SourceDirPath"
}

# --- Exclusion patterns (regex, anchored) ---
$ExcludePatterns = @(
    # Note: we will handle third_party specially below, so DO NOT include ^third_party/ here.
    '^build/',
    '^\.(git|idea|vscode)/',
    '^cmake-build-debug/',
    '\.zip$',
    '\.tar\.gz$',
    '\.tgz$'
)
$ExcludeRegex = ($ExcludePatterns -join '|')

Write-Host "--------------------------------------------------"
Write-Host "Project root:     $SourceDirPath"
Write-Host "Output archive:   $Output"
Write-Host "Archive root dir: $ProjectName-src"
Write-Host "--------------------------------------------------"

Write-Host "Collecting files to archive..."

# Get file list from git (line-delimited). git ls-files should not emit newlines in filenames.
$gitOutput = & git ls-files 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "git ls-files failed. Ensure git is installed and the current directory is a repository."
    exit 1
}

# --- Build $Files with special third_party rules ---
$Files = @()

# Strict third_party allowlist (exactly these):
#  - third_party/CMakeLists.txt
#  - third_party/cmake/** (the 'cmake' directory directly under third_party and its contents)
$tpAllowRegexes = @(
    '^third_party/CMakeLists\.txt$',  # exact top-level CMakeLists
    '^third_party/cmake(/|/.*)'       # third_party/cmake and its contents
)

foreach ($line in $gitOutput -split "`n") {
    $trimmed = $line.Trim("`r", "`n")
    if ($trimmed -eq '') { continue }

    if ($trimmed -like 'third_party/*') {
        # Only accept if it matches our strict allowlist for third_party
        $allowed = $false
        foreach ($r in $tpAllowRegexes) {
            if ($trimmed -match $r) { $allowed = $true; break }
        }
        if (-not $allowed) { continue }  # skip everything else inside third_party

        # For allowed third_party entries still apply global exclusions (e.g. .zip)
        if ($trimmed -notmatch $ExcludeRegex) {
            $Files += $trimmed
        } else {
            # excluded by general rule (e.g., archive file) -> skip
        }
    } else {
        # Not under third_party: apply the normal exclusions
        if ($trimmed -notmatch $ExcludeRegex) {
            $Files += $trimmed
        }
    }
}

$file_count = $Files.Count

if ($file_count -eq 0) {
    Write-Error "No files found to archive after applying exclusions."
    exit 1
}

Write-Host "Found $file_count files. Creating archive..."

# --- Helper: detect tar transform support ---
function TarSupportsTransform {
    try {
        $help = & tar.exe --help 2>&1
        if ($help -match '--transform' -or $help -match '\btransform\b' -or $help -match '-s,') {
            return $true
        } else {
            return $false
        }
    } catch {
        return $false
    }
}

$ArchiveRoot = "$ProjectName-src"

if (TarSupportsTransform) {
    Write-Host "Using tar --transform to prefix files with '$ArchiveRoot/'"

    # Create a temporary list file (one file per line)
    $ListFile = [System.IO.Path]::GetTempFileName()
    try {
        # Ensure Unix line endings in list file
        $Files -join "`n" | Set-Content -NoNewline -Encoding UTF8 $ListFile

        & tar.exe -czvf $Output --files-from $ListFile --transform "s,^,$ArchiveRoot/,"
        if ($LASTEXITCODE -ne 0) {
            throw "tar returned non-zero exit code: $LASTEXITCODE"
        }
    } finally {
        if (Test-Path $ListFile) { Remove-Item $ListFile -Force -ErrorAction SilentlyContinue }
    }

} else {
    Write-Host "tar.exe does not support --transform. Using temp staging directory to create root '$ArchiveRoot/'."

    $TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
    $StagingRoot = Join-Path $TempDir $ArchiveRoot

    try {
        # Create staging root
        New-Item -ItemType Directory -Path $StagingRoot -Force | Out-Null

        # Copy each selected file into the staging tree preserving directory structure
        foreach ($f in $Files) {
            $srcPath = Join-Path $SourceDirPath $f
            $destPath = Join-Path $StagingRoot $f
            $destDir  = Split-Path -Parent $destPath
            if (-not (Test-Path $destDir)) {
                New-Item -ItemType Directory -Path $destDir -Force | Out-Null
            }
            Copy-Item -LiteralPath $srcPath -Destination $destPath -Force
        }

        # Create archive by tarring the staged root directory (change to temp dir so paths are correct)
        Push-Location $TempDir
        try {
            & tar.exe -czvf $Output $ArchiveRoot
            if ($LASTEXITCODE -ne 0) {
                throw "tar returned non-zero exit code: $LASTEXITCODE"
            }
        } finally {
            Pop-Location
        }
    } finally {
        # Cleanup staging area
        if (Test-Path $TempDir) {
            Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Write-Host "--------------------------------------------------"
Write-Host "Archive created successfully:"
Write-Host "  => $Output"
Write-Host "--------------------------------------------------"

exit 0
