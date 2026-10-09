<#
.SYNOPSIS
    Copies the FieldsBus library sources (committed HEAD) into the Dev.3 tree.

.DESCRIPTION
    GitHub FieldsBus is the master copy. Dev.3 builds SoftFieldbus145_x64(d).dll from a mirror of
    include\softeip|softmb|softfb and src\*.cpp kept under Dev\Sdk\UvX. Never edit the mirror in
    Dev.3: fix the code here, commit, then run this script and commit the result in Dev.3.

      include\softeip\*  ->  <Dev3Root>\Dev\Sdk\UvX\include\softeip\*
      include\softmb\*   ->  <Dev3Root>\Dev\Sdk\UvX\include\softmb\*
      include\softfb\*   ->  <Dev3Root>\Dev\Sdk\UvX\include\softfb\*
      src\*.cpp          ->  <Dev3Root>\Dev\Sdk\UvX\sources\SoftFieldbus\src\*.cpp

    - Copies the committed HEAD content (git archive), not the working tree.
    - Writes only files whose content changed; removes mirror files that no longer exist upstream
      (only inside the four folders above). Dev.3-only files (vcxproj, rc, include\SoftFieldbus) are
      never touched.
    - Writes <Dev3Root>\Dev\Sdk\UvX\sources\SoftFieldbus\FIELDSBUS_SYNC.txt with the commit and a
      hash per file. Hashes ignore line endings (Dev.3 git may check files out with CRLF).
    - Stops if a mirror file was edited in Dev.3 since the last sync (hash differs from the stamp),
      unless -Force.

.EXAMPLE
    pwsh tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3 -WhatIf
    pwsh tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$Dev3Root,
    [switch]$AllowDirty,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$uvx = Join-Path $Dev3Root 'Dev\Sdk\UvX'
if (-not (Test-Path (Join-Path $uvx 'include'))) {
    throw "Not a Dev.3 tree: $uvx\include does not exist"
}
$stampPath = Join-Path $uvx 'sources\SoftFieldbus\FIELDSBUS_SYNC.txt'

# Source folder (repo-relative, '/' separated) -> destination folder (Dev3Root-relative), file filter
$mappings = @(
    @{ Src = 'include/softeip'; Dst = 'Dev\Sdk\UvX\include\softeip'; Filter = '*' },
    @{ Src = 'include/softmb';  Dst = 'Dev\Sdk\UvX\include\softmb';  Filter = '*' },
    @{ Src = 'include/softfb';  Dst = 'Dev\Sdk\UvX\include\softfb';  Filter = '*' },
    @{ Src = 'src';             Dst = 'Dev\Sdk\UvX\sources\SoftFieldbus\src'; Filter = '*.cpp' }
)

function Invoke-Git([string[]]$GitArgs) {
    $out = & git -C $repo @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed: $out" }
    return $out
}

# Hash of the text with LF line endings, so CRLF/LF checkouts compare equal.
function Get-TextHash([string]$Text) {
    $norm = $Text.Replace("`r`n", "`n")
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($norm)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Read-Text([string]$Path) { return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) }

# ---- FieldsBus state --------------------------------------------------------
$dirty = Invoke-Git @('status', '--porcelain', '--', 'include', 'src')
if ($dirty -and -not $AllowDirty) {
    throw "FieldsBus include/ or src/ has uncommitted changes (the sync copies HEAD). Commit them, or pass -AllowDirty.`n$($dirty -join "`n")"
}
$commit = (Invoke-Git @('rev-parse', 'HEAD')).Trim()
$describe = (Invoke-Git @('describe', '--always', '--tags')).Trim()
$branch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Trim()
$unpushed = & git -C $repo rev-list --count '@{u}..HEAD' 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Warning "Branch $branch has no upstream: Dev.3 would reference a commit that is not on GitHub."
} elseif ([int]$unpushed -gt 0) {
    Write-Warning "$unpushed commit(s) on $branch are not pushed: push before committing the sync in Dev.3."
}

# ---- Export HEAD ------------------------------------------------------------
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("fieldsbus-sync-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -WhatIf:$false | Out-Null # scratch export, also under -WhatIf
try {
    $zip = Join-Path $tmp 'head.zip'
    Invoke-Git @('archive', '--format=zip', "--output=$zip", 'HEAD', 'include', 'src') | Out-Null
    Expand-Archive -LiteralPath $zip -DestinationPath $tmp -WhatIf:$false

    # Desired mirror: Dev3-relative path -> text
    $desired = [ordered]@{}
    foreach ($m in $mappings) {
        $srcDir = Join-Path $tmp ($m.Src -replace '/', '\')
        if (-not (Test-Path $srcDir)) { throw "HEAD has no $($m.Src)" }
        foreach ($f in Get-ChildItem -Path $srcDir -File -Filter $m.Filter | Sort-Object Name) {
            $desired[(Join-Path $m.Dst $f.Name)] = Read-Text $f.FullName
        }
    }

    # ---- Previous stamp: detect hand edits in Dev.3 --------------------------
    $stampHashes = @{}
    if (Test-Path $stampPath) {
        foreach ($line in Get-Content $stampPath) {
            if ($line -match '^file\s+([0-9a-f]{64})\s+(.+)$') { $stampHashes[$Matches[2]] = $Matches[1] }
        }
    }
    $edited = @()
    foreach ($rel in $stampHashes.Keys) {
        $p = Join-Path $Dev3Root $rel
        if ((Test-Path $p) -and ((Get-TextHash (Read-Text $p)) -ne $stampHashes[$rel])) { $edited += $rel }
    }
    if ($edited.Count -gt 0) {
        $msg = "These mirror files were edited in Dev.3 since the last sync (fix them in FieldsBus instead):`n  " + ($edited -join "`n  ")
        if (-not $Force) { throw "$msg`nPass -Force to overwrite them." }
        Write-Warning $msg
    }

    # ---- Plan the changes ------------------------------------------------------
    $added = @(); $changed = @(); $removed = @(); $same = 0
    foreach ($rel in $desired.Keys) {
        $p = Join-Path $Dev3Root $rel
        if (-not (Test-Path $p)) { $added += $rel }
        elseif ((Get-TextHash (Read-Text $p)) -ne (Get-TextHash $desired[$rel])) { $changed += $rel }
        else { $same++ }
    }
    foreach ($m in $mappings) {
        $dstDir = Join-Path $Dev3Root $m.Dst
        if (Test-Path $dstDir) {
            foreach ($f in Get-ChildItem -Path $dstDir -File) {
                $rel = Join-Path $m.Dst $f.Name
                if (-not $desired.Contains($rel)) { $removed += $rel }
            }
        }
    }

    # ---- Apply ----------------------------------------------------------------
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    foreach ($rel in @($added) + @($changed)) {
        $p = Join-Path $Dev3Root $rel
        if ($PSCmdlet.ShouldProcess($p, 'write')) {
            $dir = Split-Path $p -Parent
            if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
            $text = $desired[$rel].Replace("`r`n", "`n").Replace("`n", "`r`n") # Dev.3 is a Windows tree
            [System.IO.File]::WriteAllText($p, $text, $utf8NoBom)
        }
    }
    foreach ($rel in $removed) {
        $p = Join-Path $Dev3Root $rel
        if ($PSCmdlet.ShouldProcess($p, 'delete')) { Remove-Item -LiteralPath $p }
    }

    $anyChange = ($added.Count + $changed.Count + $removed.Count) -gt 0
    if (($anyChange -or -not (Test-Path $stampPath) -or $edited.Count -gt 0) -and $PSCmdlet.ShouldProcess($stampPath, 'write stamp')) {
        $lines = @(
            '# FieldsBus mirror stamp, written by FieldsBus tools/sync-to-dev3.ps1. Do not edit.',
            '# The mirrored files are copies of github.com/sapiovesanunivision/FieldsBus; fix them there.',
            "commit $commit",
            "describe $describe",
            "branch $branch",
            ("date " + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz')),
            "source $repo"
        )
        foreach ($rel in $desired.Keys) { $lines += "file $(Get-TextHash $desired[$rel]) $rel" }
        $dir = Split-Path $stampPath -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        [System.IO.File]::WriteAllText($stampPath, (($lines -join "`r`n") + "`r`n"), $utf8NoBom)
    }

    # ---- Report ---------------------------------------------------------------
    $short = $commit.Substring(0, 7)
    Write-Host "FieldsBus $describe ($short) -> $Dev3Root"
    Write-Host ("  added {0}, changed {1}, removed {2}, unchanged {3}" -f $added.Count, $changed.Count, $removed.Count, $same)
    foreach ($r in $added) { Write-Host "  + $r" }
    foreach ($r in $changed) { Write-Host "  * $r" }
    foreach ($r in $removed) { Write-Host "  - $r" }
    $cppDelta = @($added) + @($removed) | Where-Object { $_ -like '*.cpp' }
    if ($cppDelta) {
        Write-Warning ("Source files were added or removed: update Dev\Sdk\UvX\sources\SoftFieldbus\SoftFieldbus1x.vcxproj (.filters):`n  " + ($cppDelta -join "`n  "))
    }
    if ($anyChange) { Write-Host "Dev.3 commit message: Sync FieldsBus $short" }
    else { Write-Host "Dev.3 mirror is up to date." }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue -WhatIf:$false
}
