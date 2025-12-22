<# 
tools/pack_source.ps1
--------------------------------------------------------------------
Archives project source files into a gzipped tarball.

- Uses 'git ls-files' to reliably list all tracked files.
- Excludes third_party/, build artifacts, IDE folders, etc.
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
    $ScriptDir = Get-Location
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
    # If output is relative, make it absolute relative to current dir
    try {
        $resolvedOut = Resolve-Path -LiteralPath $Output -ErrorAction SilentlyContinue
        if ($resolvedOut) {
            $Output = $resolvedOut.Path
        } else {
            $Output = (Resolve-Path (Join-Path (Get-Location) $Output)).Path
        }
    } catch {
        # fallback: build absolute path manually
        if ([System.IO.Path]::IsPathRooted($Output)) {
            # keep as-is
        } else {
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

if ($SourceDir.Path -ne $ProjectRoot.Path) {
    Write-Warning "Specified source directory differs from detected project root."
    Write-Warning "  Script implies root: $ProjectRoot"
    Write-Warning "  Provided source:     $SourceDir"
}

# --- Exclusion patterns (regex, anchored) ---
$ExcludePatterns = @(
    '^third_party/',
    '^build/',
    '^\.git',
    '^\.idea/',
    '^\.vscode/',
    '^cmake-build-debug/',
    '\.zip$',
    '\.tar\.gz$',
    '\.tgz$'
)
$ExcludeRegex = ($ExcludePatterns -join '|')

Write-Host "--------------------------------------------------"
Write-Host "Project root:     $SourceDir"
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

# convert to array of lines and filter exclusions
$Files = @()
foreach ($line in $gitOutput -split "`n") {
    $trimmed = $line.Trim("`r", "`n")
    if ($trimmed -eq '') { continue }
    if ($trimmed -notmatch $ExcludeRegex) {
        $Files += $trimmed
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
        # Ensure Unix line endings in list file (some tar implementations accept either)
        $Files -join "`n" | Set-Content -NoNewline -Encoding UTF8 $ListFile

        # Attempt to call tar with files-from + transform.
        # Note: Some tar builds accept --null/--files-from -; others accept --files-from <file>
        # We wrote the list to a temp file so we avoid null-delimited issues on Windows.
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
            $srcPath = Join-Path $SourceDir $f
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
