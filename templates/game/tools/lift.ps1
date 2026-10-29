# lift.ps1 - @TITLE@: disc image -> generated C (the toolkit's lifting pipeline).
#
#   powershell -ExecutionPolicy Bypass -File tools\lift.ps1 [-Iso <path>] [-Layout <dir>]
#
# Steps (same order as refs\defjam_recomp\installeur\installer.ps1):
#   1. game files: extract the disc into <Layout>\game\ once (read-only access to the ISO)
#   2. xbe_parser -> disasm (with seeds\*.json) -> func_id -> abi_analysis -> recomp
# Everything generated goes to private\ (never committed):
#   private\@NAME@\pipeline\  intermediate JSON
#   private\@NAME@\gen\       generated C + headers (CMake: RECOMP_GEN_DIR)
# A full rebuild is needed after every lift (CLAUDE.md: only after lifter changes).

[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Interactive script; console status output only.')]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingBrokenHashAlgorithms', '', Justification = 'SHA1 identifies the XBE version (docs/xbe.md), not a security use.')]
param(
    [string]$Iso = "@ISO@",
    [string]$Layout
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "paths.ps1")   # $Repo, $Private, $ToolsRoot
$Toolkit  = Join-Path $ToolsRoot "xboxrecomp"
$LiftDir  = Join-Path $Private "@NAME@"
if (-not $Layout) { $Layout = Join-Path $Private "install\@TITLE@ Recomp" }
$Game     = Join-Path $Layout "game"
$Xbe      = Join-Path $Game "default.xbe"
$Pipe     = Join-Path $LiftDir "pipeline"
$Gen      = Join-Path $LiftDir "gen"
$XbeSha1  = "@XBESHA1@"   # @TITLE@ (@REGION@), docs/xbe.md

function Run([string]$what, [string[]]$a) {
    Write-Host "== $what"
    & py -3 @a
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

Push-Location $Toolkit
try {
    if (-not (Test-Path -LiteralPath $Xbe)) {
        if (-not (Test-Path -LiteralPath $Iso)) { throw "Disc image not found: $Iso" }
        New-Item -ItemType Directory -Force -Path $Game | Out-Null
        Run "extract disc -> $Game" @("-m", "tools.xiso", "unpack", $Iso, "-o", $Game)
    }
    $h = (Get-FileHash -Algorithm SHA1 -LiteralPath $Xbe).Hash
    if ($h -ne $XbeSha1) { Write-Warning "unknown default.xbe version (SHA1 $h) - continuing" }

    $dis = Join-Path $Pipe "disasm"; $fid = Join-Path $Pipe "func_id"
    $abi = Join-Path $Pipe "abi";    $rec = Join-Path $Pipe "recomp"
    foreach ($d in @($dis, $fid, $abi, $rec, $Gen)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    $analysis = Join-Path $Pipe "xbe.json"

    $seeds = @()
    foreach ($f in Get-ChildItem (Join-Path $Repo "seeds") -Filter "*_seeds.json") {
        $seeds += @("--seed-functions", $f.FullName)
    }

    Run "xbe_parser" @("-m", "tools.xbe_parser", $Xbe, "--json", $analysis)
    Run "disasm" (@("-m", "tools.disasm", $Xbe, "-o", $dis, "--analysis-json", $analysis, "--force") + $seeds)
    Run "func_id" @("-m", "tools.func_id", $Xbe, "--functions", (Join-Path $dis "functions.json"),
        "--strings", (Join-Path $dis "strings.json"), "--xrefs", (Join-Path $dis "xrefs.json"), "-o", $fid)
    Run "abi_analysis" @("-m", "tools.abi_analysis", $Xbe, "--disasm-dir", $dis, "--func-id-dir", $fid, "--output-dir", $abi)

    Get-ChildItem $Gen -File -ErrorAction SilentlyContinue | Remove-Item
    Run "recomp" @("-m", "tools.recomp", $Xbe, "--all", "--split", "1000", "--game-name", "@EXE@",
        "--gen-dir", $Gen, "--exclude-manual", (Join-Path $Repo "src\recomp_manual.c"),
        "--disasm-dir", $dis, "--func-id-dir", $fid, "--abi-dir", $abi, "-o", $rec)
} finally { Pop-Location }

$n = (Get-ChildItem $Gen -Filter "*.c").Count
Write-Host "Done: $n C files in $Gen"
