<#
.SYNOPSIS
    Runs every RedFS correctness and memory check.

.DESCRIPTION
    Four configurations, because each catches something the others cannot:

      release   unit tests + fuzzer, fast
      debug     CRT heap leak check (steady state across passes)
      asan      AddressSanitizer over tests and fuzzer -- out-of-bounds, UAF
      install   integration sweep against a real game install (optional)

    The first three need nothing but a compiler. The fourth needs Cyberpunk 2077
    and is skipped unless -GameDir is given.

.EXAMPLE
    .\run-checks.ps1
    .\run-checks.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Cyberpunk 2077"
    .\run-checks.ps1 -FuzzIterations 200000
#>
[CmdletBinding()]
param(
    [string] $GameDir,
    [string] $TexconvDll = "C:\Work\WorkSpace\Cyberpunk\WolvenKit\WolvenKit.Common\lib\texconv.dll",
    # Both content checks need a path list: `verify` and the round-trip select
    # files by glob, and globbing needs names the dictionary knows.
    [string] $PathList = "C:\Work\WorkSpace\Cyberpunk\WolvenKit\WolvenKit.Common\Resources\usedhashes.kark",
    [string] $WolvenKitCli = "C:\Modding\wolwenKit_upacker\WolvenKit.CLI.exe",
    # One WolvenKit process per file, so this is the slow one: ~2 s each.
    [int]    $RoundTripCount = 10,
    [int]    $FuzzIterations = 30000,
    # 12000, not 4000. The sampled depot holds exactly one cubemap and it sits at
    # about index 11,000, so every smaller count skipped the cubemap encoding path
    # entirely -- and reported a clean run while doing it. Costs ~18 s.
    [int]    $VerifyCount = 12000,
    [switch] $SkipAsan,
    [switch] $Rebuild
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$cmake = 'C:\Work\cmake\bin\cmake.exe'
$ninja = 'C:/Work/ninja/ninja.exe'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $cmake))  { $cmake = 'cmake' }
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found. Adjust `$vcvars in this script." }

$script:failures = @()
function Step([string] $name, [scriptblock] $body) {
    Write-Host ""
    Write-Host "=== $name " -NoNewline -ForegroundColor Cyan
    Write-Host ("=" * [Math]::Max(0, 60 - $name.Length)) -ForegroundColor Cyan
    try {
        & $body
        if ($LASTEXITCODE -ne 0) { throw "exit code $LASTEXITCODE" }
        Write-Host "  PASS" -ForegroundColor Green
    } catch {
        Write-Host "  FAIL: $_" -ForegroundColor Red
        $script:failures += $name
    }
}

# Runs a command inside the MSVC environment.
function Msvc([string] $command) {
    cmd /c "`"$vcvars`" > nul 2>&1 && $command"
}

function Configure([string] $dir, [string] $buildType, [string[]] $extra) {
    if ($Rebuild -and (Test-Path "$root\$dir")) { Remove-Item -Recurse -Force "$root\$dir" }
    if (Test-Path "$root\$dir\build.ninja") { return }
    $args = @("-S `"$root`"", "-B `"$root\$dir`"", "-G Ninja",
              "-DCMAKE_BUILD_TYPE=$buildType", "-DCMAKE_MAKE_PROGRAM=$ninja") + $extra
    Msvc "`"$cmake`" $($args -join ' ') > nul"
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $dir" }
}

function Build([string] $dir) {
    Msvc "`"$cmake`" --build `"$root\$dir`""
    if ($LASTEXITCODE -ne 0) { throw "build failed for $dir" }
}

Write-Host "RedFS check suite" -ForegroundColor White

# --- release: the fast pass ---------------------------------------------------

Step "build (release)" {
    Configure 'build' 'Release' @()
    Build 'build'
}

Step "unit tests" {
    & "$root\build\redfs_test.exe"
}

Step "parser fuzzer ($FuzzIterations iterations)" {
    & "$root\build\redfs_fuzz.exe" $FuzzIterations 1 | Select-Object -Last 1
}

Step "lifecycle (abrupt exit, shutdown, dll unload)" {
    & "$root\build\redfs_lifecycle.exe"
}

# --- debug: leak detection ----------------------------------------------------

Step "build (debug)" {
    Configure 'build-debug' 'Debug' @('-DREDFS_BUILD_SHARED=OFF')
    Build 'build-debug'
}

Step "leak check (CRT heap, steady state)" {
    & "$root\build-debug\redfs_test.exe"
}

# --- asan: memory safety ------------------------------------------------------

if (-not $SkipAsan) {
    Step "build (asan)" {
        Configure 'build-asan' 'RelWithDebInfo' @('-DREDFS_SANITIZE=address')
        Build 'build-asan'
        # The ASan runtime is not on PATH by default; put it beside the binaries.
        $rt = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC" `
              -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
              Select-Object -First 1
        if ($rt) { Copy-Item $rt.FullName "$root\build-asan\" -Force }
        else { Write-Host "  (ASan runtime DLL not found; binaries may not start)" -ForegroundColor Yellow }
        $global:LASTEXITCODE = 0
    }

    Step "unit tests under ASan" {
        & "$root\build-asan\redfs_test.exe"
    }

    Step "fuzzer under ASan ($FuzzIterations iterations)" {
        & "$root\build-asan\redfs_fuzz.exe" $FuzzIterations 1 | Select-Object -Last 1
    }

    Step "fuzzer under ASan (varied seeds)" {
        foreach ($seed in 7, 42, 1337) {
            & "$root\build-asan\redfs_fuzz.exe" ([int]($FuzzIterations / 3)) $seed | Select-Object -Last 1
            if ($LASTEXITCODE -ne 0) { throw "seed $seed crashed" }
        }
        $global:LASTEXITCODE = 0
    }

    Step "lifecycle under ASan" {
        & "$root\build-asan\redfs_lifecycle.exe"
    }
}

# --- integration: needs a real install ----------------------------------------

if ($GameDir) {
    if (-not (Test-Path $GameDir)) { throw "GameDir does not exist: $GameDir" }

    Step "selftest against the install" {
        & "$root\build\redfs_cli.exe" --game $GameDir selftest | Select-Object -Last 2
    }

    # The archive's own SHA-1, which RedFS cannot influence. Single-segment files
    # only -- see docs/done/verification.md, oracle 7 -- so the patterns are the
    # ones where it applies. Exit 2 means everything checked passed but something
    # was skipped, which for these should not happen.
    Step "decoded bytes vs the archive's SHA-1" {
        foreach ($pat in @("*.wem", "*.opuspak", "*.json", "*.mlsetup", "*.scene")) {
            & "$root\build\redfs_cli.exe" --game $GameDir verify $PathList $pat 150 |
                Select-String -Pattern 'matched the index|coverage|FAILED|INCOMPLETE'
            if ($LASTEXITCODE -ne 0) { throw "verify $pat exited $LASTEXITCODE" }
        }
    }

    # Multi-segment resources, where that oracle does not apply and a second
    # independent reader is the only check available.
    if (Test-Path $WolvenKitCli) {
        Step "multi-segment resources vs WolvenKit's own extraction" {
            foreach ($pat in @("*.mesh", "*.xbm", "*.ent", "*.app")) {
                & "$root\tools\roundtrip.ps1" -GameDir $GameDir -List $PathList `
                    -WolvenKit $WolvenKitCli -Pattern $pat -Count $RoundTripCount |
                    Select-String -Pattern 'identical prefix|FAIL'
                if ($LASTEXITCODE -ne 0) { throw "roundtrip $pat exited $LASTEXITCODE" }
            }
        }
    } else {
        Write-Host ""
        Write-Host "  (skipping WolvenKit round-trip: $WolvenKitCli not found)" -ForegroundColor Yellow
    }

    if (Test-Path $TexconvDll) {
        Step "texture + mesh verification vs external oracles ($VerifyCount)" {
            # 'disagree' matters: the desc_of vs mesh_open cross-check fails the
            # step via its exit code, but without this pattern the reason never
            # reached the console -- a failure with no visible cause.
            & "$root\build\redfs_verify.exe" $TexconvDll $GameDir $VerifyCount |
                Select-String -Pattern 'checked|mismatch|escaping|computable|disagree|FAIL'
        }
    } else {
        Write-Host ""
        Write-Host "  (skipping DirectXTex cross-check: $TexconvDll not found)" -ForegroundColor Yellow
    }
} else {
    Write-Host ""
    Write-Host "  (skipping integration checks: pass -GameDir to enable)" -ForegroundColor Yellow
}

# --- summary ------------------------------------------------------------------

Write-Host ""
Write-Host ("=" * 66) -ForegroundColor Cyan
if ($script:failures.Count -eq 0) {
    Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
    exit 0
}
Write-Host "FAILED: $($script:failures -join ', ')" -ForegroundColor Red
exit 1
