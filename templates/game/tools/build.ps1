# build.ps1 - the only way to build @TITLE@ (CLAUDE.md, Resources).
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Clean] [-Jobs 2]
#
# - one build at a time: a lock file in the build directory (strictly sequential)
# - max. 2 compiler processes, incremental; -Clean only after lifter changes
# - one build directory per worktree, outside the repo:
#     private\build\<worktree folder name>
# - output goes into the portable install layout, never into the repo:
#     private\install\@TITLE@ Recomp\@EXE@.exe
# Paths: tools\paths.ps1 (space-free; llvm-mingw clang + Ninja use them directly).
# glslang/SDL3 for the Vulkan renderer: built with the same compiler by tools\deps.ps1 (docs/toolkit.md).

[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Interactive script; console status output only.')]
param(
    [switch]$Clean,
    [int]$Jobs = 2,
    [string]$Layout
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "paths.ps1")           # $Repo, $Private, $ToolsRoot, $Shared
$Name   = Split-Path -Leaf $Repo
$Src    = $Repo
$Build  = Join-Path $Private "build\$Name"
if (-not $Layout) { $Layout = Join-Path $Private "install\@TITLE@ Recomp" }
if ($Jobs -gt 2) { $Jobs = 2 }

$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw "clang (llvm-mingw) not on PATH - run tools\setup.ps1" }
$bin = Split-Path -Parent $clang
$Deps = Join-Path $Shared "deps\install"           # glslang + SDL3 (tools\deps.ps1)
if (-not (Test-Path (Join-Path $Deps "lib\libglslang.a"))) { throw "glslang/SDL3 missing - run tools\deps.ps1" }

New-Item -ItemType Directory -Force -Path $Build, $Layout | Out-Null
$lockPath = Join-Path $Build "build.lock"
try {
    $lock = [System.IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
} catch {
    throw "Another build is running (lock $lockPath)"
}
try {
    if ($Clean) {
        Get-ChildItem $Build -Exclude "build.lock" | Remove-Item -Recurse -Force
    }
    function Cm($p) { return ($p -replace '\\', '/') }
    # Configure every time: cheap with a cache, and picks up changed settings.
    & cmake -S $Src -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_C_COMPILER=$(Cm $clang)" `
        "-DCMAKE_CXX_COMPILER=$(Cm (Join-Path $bin 'clang++.exe'))" `
        "-DCMAKE_PREFIX_PATH=$(Cm $Deps)" `
        "-DCMAKE_RC_COMPILER=$(Cm (Join-Path $bin 'llvm-rc.exe'))" `
        "-D@PREFIX@_OUTPUT_DIR=$(Cm $Layout)" `
        "-DXBOXRECOMP_DIR=$(Cm (Join-Path $ToolsRoot 'xboxrecomp'))" `
        "-DRECOMP_GEN_DIR=$(Cm (Join-Path $Private '@NAME@\gen'))" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    $t = [Diagnostics.Stopwatch]::StartNew()
    & cmake --build $Build --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    Write-Host ("Built in {0:N0} s: {1}" -f $t.Elapsed.TotalSeconds, (Join-Path $Layout "@EXE@.exe"))
} finally {
    $lock.Close()
}
