# ============================================================================
#  build.ps1 -- build mdpsr
#
#  Usage:
#      powershell -File build.ps1                 # Release
#      powershell -File build.ps1 -Clean          # wipe build/ first
#      powershell -File build.ps1 -Run            # build, then run hot-plug + GUI selftest
#      powershell -File build.ps1 -Asan -Run      # AddressSanitizer build + selftest
#      powershell -File build.ps1 -Config Debug
#      powershell -File build.ps1 -CMake <path>
#
#  Entry : src/main/CMakeLists.txt  (also builds the test/ plugins + GUI selftest)
#  Output: build/mdpsr.exe + build/config.json + build/plugins/<name>/
#
#  GUI only (seconds):  build\mdpsr.exe --guitest
#
#  NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 decodes
#  .ps1 files using the ANSI code page unless they carry a UTF-8 BOM, so non
#  ASCII text here can shift byte pairing and corrupt the parse. All the
#  Chinese documentation lives in the C++ sources and in README/doc instead.
#
#  Generator defaults to Visual Studio (MSBuild). Ninja is avoided on purpose:
#  the localized MSVC /showIncludes prefix goes through a codepage conversion
#  and no longer matches cl.exe's real output, which leaves Ninja's dependency
#  database empty (editing a header then does not trigger a rebuild).
# ============================================================================
param(
    [string]$Config = "Release",
    [switch]$Clean,
    [string]$Generator = "Visual Studio 18 2026",
    [string]$CMake = "",
    [switch]$Asan,
    [switch]$Run,
    [int]$Cycles = 20
)

$ErrorActionPreference = "Stop"

$root  = $PSScriptRoot
$entry = Join-Path $root "src\main"
$build = Join-Path $root "build"

function Find-CMake {
    param([string]$Hint)
    $cands = @()
    if ($Hint) { $cands += $Hint }
    if ($env:ProgramFiles) { $cands += (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe") }
    $cands += "D:\Cpp\cmake\bin\cmake.exe"
    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($onPath) { $cands += $onPath.Source }
    foreach ($c in $cands) {
        if (-not [string]::IsNullOrWhiteSpace($c) -and (Test-Path -LiteralPath $c)) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    throw "cmake.exe not found. Pass -CMake <full path>."
}

$cmakeExe = Find-CMake -Hint $CMake
Write-Host "cmake     : $cmakeExe"
Write-Host "generator : $Generator"
$asanTag = ""
if ($Asan) { $asanTag = " + AddressSanitizer" }
Write-Host "config    : $Config$asanTag"

if ($Clean) {
    if (Test-Path -LiteralPath $build) {
        Write-Host "clean     : $build"
        Remove-Item -LiteralPath $build -Recurse -Force
    }
}

$cfgArgs = @("-S", $entry, "-B", $build, "-G", $Generator, "-A", "x64")
# Always pass the flag explicitly: MDPSR_ASAN lives in the CMake cache, so a bare
# rebuild after an -Asan run would silently stay sanitizer-built (and then fail with
# STATUS_DLL_NOT_FOUND once clang_rt.asan_dynamic-*.dll is not next to the exe).
$asanFlag = "OFF"
if ($Asan) { $asanFlag = "ON" }
$cfgArgs += "-DMDPSR_ASAN=$asanFlag"

Write-Host "--- configure ---"
& $cmakeExe @cfgArgs
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

Write-Host "--- build ($Config) ---"
& $cmakeExe --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# Verify every artifact exists: a missing one means a POST_BUILD step did not run.
$need = @(
    "mdpsr.exe",
    "config.json",
    "plugins\sysmgr\mdpsr_sysmgr.dll", "plugins\sysmgr\plugin.json",
    "plugins\alpha\mdpsr_alpha.dll",   "plugins\alpha\plugin.json",
    "plugins\beta\mdpsr_beta.dll",     "plugins\beta\plugin.json",
    "plugins\gamma\mdpsr_gamma.dll",   "plugins\gamma\plugin.json",
    "plugins\winmsg\mdpsr_winmsg.dll", "plugins\winmsg\plugin.json",
    "plugins\paint\mdpsr_paint.dll",   "plugins\paint\plugin.json",
    "plugins\circ_a\mdpsr_circ_a.dll", "plugins\circ_a\plugin.json",
    "plugins\circ_b\mdpsr_circ_b.dll", "plugins\circ_b\plugin.json",
    "plugins\circ_c\mdpsr_circ_c.dll", "plugins\circ_c\plugin.json"
)
$missing = @()
foreach ($f in $need) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $f))) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Write-Host "missing artifacts:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "   $_" -ForegroundColor Red }
    throw "incomplete build output"
}

Write-Host "built: $(Join-Path $build 'mdpsr.exe')" -ForegroundColor Green

if ($Run) {
    Write-Host "--- hot-plug selftest (cycles=$Cycles) ---"
    & (Join-Path $build "mdpsr.exe") --selftest --cycles $Cycles
    $rc = $LASTEXITCODE
    if ($rc -eq 0) { Write-Host "selftest PASSED" -ForegroundColor Green }
    else { Write-Host "selftest FAILED, exit code $rc" -ForegroundColor Red }
    exit $rc
}
