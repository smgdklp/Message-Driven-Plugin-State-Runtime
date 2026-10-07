# ============================================================================
#  build.ps1 -- build mdpsr
#
#  Usage:
#      pwsh -File build.ps1                  # Release
#      pwsh -File build.ps1 -Config Debug
#      pwsh -File build.ps1 -Clean           # wipe build/ first
#      pwsh -File build.ps1 -CMake <path>
#
#  Entry: src/main/CMakeLists.txt (organises the whole project)
#  Output: ./build/
#      build/mdpsr.exe              host
#      build/config.json            init config (must sit next to the exe)
#      build/core/                  kernel component
#      build/ticker/                sample plugin
#
#  Generator defaults to Visual Studio (MSBuild). Ninja is avoided on purpose:
#  the localized MSVC /showIncludes prefix goes through a codepage conversion
#  and no longer matches cl.exe's real output, which leaves Ninja's dependency
#  database empty (editing a header then does not trigger a rebuild).
# ============================================================================
param(
    [string]$Config = "Release",
    [switch]$Clean,
    [string]$CMake,
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root  = $PSScriptRoot
$entry = Join-Path $root "src\main"
$build = Join-Path $root "build"

function Find-First {
    param([string[]]$Candidates)
    foreach ($c in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($c)) { continue }
        if ($c -match '[\\/]' -or $c -match '^[A-Za-z]:') {
            if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
        } else {
            $cmd = Get-Command $c -ErrorAction SilentlyContinue
            if ($cmd) { return $cmd.Source }
        }
    }
    return $null
}

# ---------------------------------------------------------------- MSVC
$vswhere = Find-First @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
)
$vs = $null
if ($vswhere) {
    $vs = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
    if ($vs) { $vs = $vs.Trim() }
}
if (-not $vs) {
    $probe = Find-First @(
        "D:\Program Files\vs\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
    if ($probe) { $vs = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $probe)) }
}
if (-not $vs) {
    Write-Host "Visual Studio (MSVC) toolchain not found." -ForegroundColor Red
    exit 1
}
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

# ---------------------------------------------------------------- cmake
$cmakeCands = @()
if ($CMake) { $cmakeCands += $CMake }
$cmakeCands += @(
    "D:\Cpp\cmake\bin\cmake.exe",
    "D:\Cpp\CMake\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe",
    "cmake"
)
$cmakeExe = Find-First $cmakeCands
if (-not $cmakeExe) {
    Write-Host "cmake not found." -ForegroundColor Red
    exit 1
}

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Write-Host "== cleaning $build ==" -ForegroundColor Yellow
    Remove-Item -Recurse -Force -LiteralPath $build
}

Write-Host "== toolchain ==" -ForegroundColor Cyan
Write-Host "  VS        : $vs"
Write-Host "  cmake     : $cmakeExe"
Write-Host "  generator : $Generator"
Write-Host "  entry     : $entry"
Write-Host "  config    : $Config"
Write-Host ""

# NOTE: do not touch PATH here -- $env:PATH is expanded while the whole line is
# parsed, which would drop whatever vcvars just injected.
$line = "`"$vcvars`" >nul 2>&1 && `"$cmakeExe`" -S `"$entry`" -B `"$build`" -G `"$Generator`" -A x64"
$line += " && `"$cmakeExe`" --build `"$build`" --config $Config"

cmd /c $line
$code = $LASTEXITCODE

Write-Host ""
if ($code -eq 0) {
    Write-Host "== build OK ==" -ForegroundColor Green
    Get-ChildItem $build -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in ".exe", ".dll" -and $_.FullName -notmatch '\\CMakeFiles\\|\\.vs\\' } |
        ForEach-Object { Write-Host ("  {0,-56} {1,12:N0} B" -f $_.FullName.Substring($root.Length + 1), $_.Length) }
} else {
    Write-Host "== build FAILED, exit code $code ==" -ForegroundColor Red
}
exit $code
