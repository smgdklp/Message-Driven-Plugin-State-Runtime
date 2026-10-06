# ============================================================================
#  test/run_all.ps1 —— 依次构建并运行 /test 下的每个独立测试项目
#
#  用法:
#      pwsh -File test\run_all.ps1                 # 全部测试
#      pwsh -File test\run_all.ps1 -Test mt        # 只跑某个
#
#  每个测试项目都有自己的 CMakeLists, 能独立编译出完整的可执行测试;
#  它们会把整个项目一起构建到 build/test/<名字>/ 下。
# ============================================================================
param(
    [string[]]$Test = @("queue", "mt"),
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 18 2026",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $root "build"

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

$vswhere = Find-First @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe")
$vs = $null
if ($vswhere) {
    $vs = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
    if ($vs) { $vs = $vs.Trim() }
}
if (-not $vs) { throw "找不到 Visual Studio (vswhere)" }

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmakeExe = Find-First @(
    "D:\Cpp\cmake\bin\cmake.exe",
    "D:\Cpp\CMake\bin\cmake.exe",
    (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    "C:\Program Files\CMake\bin\cmake.exe",
    "cmake")

if (-not $cmakeExe) { throw "找不到 cmake (约定 D:\Cpp\cmake\bin\cmake.exe)" }

$failed = 0
foreach ($t in $Test) {
    $src = Join-Path $root "test\$t"
    $bin = Join-Path $buildRoot "test\$t"

    Write-Host ""
    Write-Host "======================================================" -ForegroundColor Cyan
    Write-Host " 测试项目: $t" -ForegroundColor Cyan
    Write-Host "======================================================" -ForegroundColor Cyan

    if (-not (Test-Path -LiteralPath (Join-Path $src "CMakeLists.txt"))) {
        Write-Host "  跳过: 没有 $src\CMakeLists.txt" -ForegroundColor Yellow
        continue
    }

    if (-not $SkipBuild) {
        $line = "`"$vcvars`" >nul 2>&1 && `"$cmakeExe`" -S `"$src`" -B `"$bin`" -G `"$Generator`" -A x64 && `"$cmakeExe`" --build `"$bin`" --config $Config"
        cmd /c $line
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  构建失败, 退出码 $LASTEXITCODE" -ForegroundColor Red
            $failed++
            continue
        }
    }

    $exe = Join-Path $bin "mdpsr_test_$t.exe"
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "  找不到测试可执行文件: $exe" -ForegroundColor Red
        $failed++
        continue
    }

    & $exe
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Host "  测试失败, 退出码 $rc" -ForegroundColor Red
        $failed++
    } else {
        Write-Host "  测试通过" -ForegroundColor Green
    }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "== 全部测试通过 ==" -ForegroundColor Green
} else {
    Write-Host "== 有 $failed 个测试项目失败 ==" -ForegroundColor Red
}
exit $failed
