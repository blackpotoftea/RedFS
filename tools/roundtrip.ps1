<#
.SYNOPSIS
    Byte-compares RedFS extraction against WolvenKit's, for the resources the
    archive's own SHA-1 cannot verify.

.DESCRIPTION
    `redfs_cli verify` checks decoded bytes against the SHA-1 in the archive
    index, which is an oracle RedFS cannot influence -- but it only applies to
    single-segment files (see docs/done/verification.md, oracle 7). Meshes,
    textures, entities and anything else with attached buffers need a second
    reader instead, and WolvenKit is the obvious one.

    THE COMPARISON IS PREFIX-ONLY, AND THAT IS NOT A WEAKENING. The two tools
    answer different questions: WolvenKit reassembles the on-disk CR2W container,
    writing the document and then the buffers re-embedded in the form the CR2W
    buffer table describes, while RedFS hands back decompressed segments and
    re-embeds nothing. So the files legitimately differ in length, and RedFS's
    main segment must appear as an exact leading run of WolvenKit's file. A
    whole-file `fc /b` reports a mismatch on a file that is correct.

    Every byte RedFS produced is still checked -- the prefix is 100% of RedFS's
    output, not a sample of it. What goes unchecked is the buffer payload, which
    WolvenKit stores compressed and RedFS does not.

.EXAMPLE
    .\tools\roundtrip.ps1
    .\tools\roundtrip.ps1 -Pattern "*.xbm" -Count 50
#>
[CmdletBinding()]
param(
    [string] $GameDir   = "D:\SteamLibrary\steamapps\common\Cyberpunk 2077",
    [string] $List      = "C:\Work\WorkSpace\Cyberpunk\WolvenKit\WolvenKit.Common\Resources\usedhashes.kark",
    [string] $Cli       = (Join-Path $PSScriptRoot "..\build\redfs_cli.exe"),
    [string] $WolvenKit = "C:\Modding\wolwenKit_upacker\WolvenKit.CLI.exe",
    [string] $Pattern   = "*.mesh",
    [int]    $Count     = 25
)

foreach ($p in @($Cli, $WolvenKit)) {
    if (-not (Test-Path $p)) { Write-Host "not found: $p"; exit 3 }
}
if (-not (Test-Path $GameDir)) { Write-Host "no game install at $GameDir"; exit 3 }

$work = Join-Path $env:TEMP "redfs_roundtrip"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null

# `find` reads no file contents, so picking candidates costs nothing.
$rows = @()
foreach ($l in (& $Cli --game $GameDir find $List $Pattern $Count 2>&1 | Select-String '^\s+0x')) {
    $f = $l.ToString().Trim() -split '\s+', 3
    $rows += [pscustomobject]@{ Hex = $f[0]; Path = $f[2] }
}
Write-Host ("{0} candidates for {1}" -f $rows.Count, $Pattern)

$ok = 0; $bad = 0; $skip = 0
foreach ($r in $rows) {
    $arc = ((& $Cli --game $GameDir stat $r.Hex 2>&1 | Select-String '^archive').ToString() -replace '^archive\s+', '')
    $dec = [Convert]::ToUInt64($r.Hex.Substring(2), 16)

    $out = Join-Path $work ([IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory $out | Out-Null
    & $WolvenKit unbundle $arc -o $out --hash $dec 2>&1 | Out-Null
    $wk = Get-ChildItem $out -Recurse -File | Select-Object -First 1
    if (-not $wk) {
        $skip++
        Write-Host ("  SKIP  {0}  (WolvenKit produced nothing)" -f $r.Path)
        continue
    }

    $mine = Join-Path $work "redfs.bin"
    & $Cli --game $GameDir extract $r.Hex $mine main 2>&1 | Out-Null

    $a = [IO.File]::ReadAllBytes($wk.FullName)
    $b = [IO.File]::ReadAllBytes($mine)
    $same = 0
    while ($same -lt $b.Length -and $same -lt $a.Length -and $a[$same] -eq $b[$same]) { $same++ }

    # An empty main segment matching "everything" would be a vacuous pass.
    if ($b.Length -gt 0 -and $same -eq $b.Length) {
        $ok++
        Write-Host ("  ok    {0,9:N0} of {1,9:N0}  {2}" -f $b.Length, $a.Length, (Split-Path $r.Path -Leaf))
    } else {
        $bad++
        Write-Host ("  FAIL  diverged at {0} of {1} (wk {2})  {3}" -f $same, $b.Length, $a.Length, $r.Path)
    }
    Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $mine -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host ("{0}: {1} identical prefix, {2} mismatched, {3} skipped" -f $Pattern, $ok, $bad, $skip)
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

# Nothing compared is not a pass -- the usual cause is a pattern the dictionary
# does not know, which would otherwise report success having checked nothing.
if ($ok -eq 0) { exit 1 }
if ($bad -gt 0) { exit 1 }
exit 0
