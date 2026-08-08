# check_sodium_module_boundary.ps1 — one module owns libsodium.
#
# PowerShell mirror of tools/check_sodium_module_boundary.sh.  The two
# scripts MUST be kept semantically equivalent.  See the .sh header for
# the rationale, the scope decision (src/ only — tests may include
# <sodium.h> directly), and the remediation guidance.

param([string]$Root = "")

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    $Root = Split-Path (Split-Path $PSCommandPath -Parent) -Parent
}

# Mirrors the .sh regex: angle OR quoted form, bare header or a sodium/
# subheader, tolerating whitespace after '#'.  The quoted form is covered
# deliberately — it compiles identically and would otherwise be a
# one-character bypass.  See the .sh header.
$ForbiddenRe = '^\s*#\s*include\s*[<"]sodium(/[^>"]*)?\.h[>"]'

$SrcRoot = Join-Path $Root 'src'

$Files = Get-ChildItem -Path $SrcRoot -Recurse -File `
                       -Include '*.cpp', '*.hpp', '*.h' `
                       -ErrorAction SilentlyContinue |
         # Exclusion-shaped, like the .sh: scan all of src/ and subtract
         # the module, so a new directory is covered automatically.
         Where-Object { $_.FullName -notmatch '[\\/]security[\\/]' }

$Violations = @()

foreach ($f in $Files) {
    $matches = Select-String -Path $f.FullName -Pattern $ForbiddenRe
    foreach ($m in $matches) {
        $Violations += "$($m.Path):$($m.LineNumber):$($m.Line.Trim())"
    }
}

if ($Violations.Count -gt 0) {
    Write-Error @"
FAIL — libsodium included outside the security module:
$($Violations -join "`n")

One module owns libsodium (HEP-CORE-0043).  Reach crypto via
  sec::secure().<op>(...)        — random_bytes, memzero,
                                   secretbox_*, pwhash_*, memcmp_ct
  sec::secure().keys()           — KeyStore (LockedKey-backed)

If an operation genuinely has no wrapper, ADD IT TO THE MODULE rather
than including <sodium.h> at the call site — see
src/utils/service/vault_crypto.cpp for a consumer that needed pwhash +
secretbox and got them this way.
"@
    exit 1
}

Write-Output "OK — HEP-CORE-0043: no libsodium include outside the security module."
exit 0
