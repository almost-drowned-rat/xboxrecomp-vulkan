# regress.ps1 - the full regression before every merge (maintainer, 2026-10-28; porting guide M5; model: THPS 2X
# tools/regress.ps1). Copy into the port and set the four names below.
#
#   powershell -ExecutionPolicy Bypass -File tools\regress.ps1 -Exe <port exe> [-Out <new dir>]
#
# Parts, one summary line each (PASS / FAIL / SKIP + the evidence path), also in <Out>\summary.txt:
#   1. menu tour   $TourScript, screenshots -> tools\dump_png.ps1 -> tools\shot_compare.ps1 against the frozen
#                  reference (masks + manual.txt); the manual-check shots are named in the line.
#   2. split       $SplitScript: every one of its shot= lines written.
#   3. storage     $StorageTest (its own checks).
# A part whose script or tool does not exist yet is SKIP - say so in the merge report, it is not a pass.
# Game runs: throw-away saves\ + data\, RECOMP_NO_DIALOGS=1, windowed; problem lines in the log fail the part.
# A fresh run folder (tools\run_dir.ps1 New-RunDir), the shared lock (tools\lock.ps1) for the whole run.
# Exit code 0 only when every part passed.
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Console test report.')]
param(
    [Parameter(Mandatory = $true)] [string]$Exe,
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
# ── per port ──
$Who          = "<agent> (<game>)"                 # the lock's holder name
$TourScript   = "menu_tour.txt"                    # tests\input\..., with shot= lines
$SplitScript  = "split_screen.txt"                 # tests\input\..., with shot= lines
$StorageTest  = "tests\storage.ps1"                # takes -Exe and -Out
$DisplayEnv   = "GAME_DISPLAY_MODE"                # the port's display-mode override, set to windowed
$KnownUnimpl  = "(?!)"                             # [UNIMPL] lines in every build so far, with the reason, e.g.
                                                   # "\[UNIMPL\].*at 0x(00013A42|0002040E)\b" (THPS 2X: D3D port I/O)
# ──
$Repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $Repo "tools\run_dir.ps1")
. (Join-Path $Repo "tools\lock.ps1")
if (-not $Out) { $Out = "C:\rc\private\evidence\regress\$(Get-RunStamp)" }
$Out = New-RunDir $Out
$Problems = "\[NULLPTR\]|\[UNIMPL\]|FATAL|Failed to resolve|is not code|access violation|unhandled exception"
$summary = New-Object System.Collections.Generic.List[string]
$failed = 0

function Line([string]$state, [string]$part, [string]$detail, [string]$path) {
    $s = "{0,-10} {1}  {2}  ({3})" -f $part, $state, $detail, $path
    Write-Host $s -ForegroundColor $(switch ($state) { "PASS" { "Green" } "SKIP" { "Yellow" } default { "Red" } })
    $script:summary.Add($s)
    if ($state -ne "PASS") { $script:failed++ }
}

function GameRun([string]$name, [string]$script, [int]$seconds, [string]$Shots) {
    $work = New-RunDir (Join-Path $Out "work_$name")
    New-Item -ItemType Directory -Path (Join-Path $work "saves"), (Join-Path $work "data") | Out-Null
    $env:RECOMP_NO_DIALOGS = "1"
    Set-Item "Env:$DisplayEnv" "windowed"
    $env:RECOMP_SHOT_PREFIX = "$Shots\"
    $p = Start-Process -FilePath $Exe -PassThru -Wait -ArgumentList @(
        "--max-seconds", "$seconds", "--save-dir", "`"$work\saves`"", "--data-dir", "`"$work\data`"",
        "--input-script", "`"$(Join-Path $Repo "tests\input\$script")`"")
    $log = Get-ChildItem (Join-Path $work "data") -Filter *.log -ErrorAction SilentlyContinue | Select-Object -First 1
    $bad = if ($log) {
        Copy-Item $log.FullName (Join-Path $Out "$name.log")
        @(Select-String -Path $log.FullName -Pattern $Problems | Where-Object { $_.Line -notmatch $KnownUnimpl })
    } else { @("no log") }
    return @{ exit = $p.ExitCode; bad = $bad }
}

function Shots([string]$script) {
    @(Select-String -Path (Join-Path $Repo "tests\input\$script") -Pattern "^\d+:shot=").Count
}

$lock = Enter-GlobalLock -Class shared -Who $Who -What "full regression tools\regress.ps1 (untimed)"
try {
    if ((Test-Path (Join-Path $Repo "tests\input\$TourScript")) -and (Test-Path (Join-Path $Repo "tools\shot_compare.ps1"))) {
        $tour = New-RunDir (Join-Path $Out "menu_tour")
        $r = GameRun "menu_tour" $TourScript ([int](Shots $TourScript) * 12 + 60) $tour
        & (Join-Path $Repo "tools\dump_png.ps1") $tour | Out-Null
        $cmp = @(& (Join-Path $Repo "tools\shot_compare.ps1") -Run $tour 2>&1)
        $cmpExit = $LASTEXITCODE
        $cmp | Set-Content (Join-Path $Out "menu_tour_compare.txt")
        $manual = @($cmp | Where-Object { "$_" -match "manual visual check \(not compared\)" } | ForEach-Object { ("$_" -split "\s+")[0] })
        Line $(if ($cmpExit -eq 0 -and $r.exit -eq 0 -and $r.bad.Count -eq 0) { "PASS" } else { "FAIL" }) "menu tour" `
            "exit $($r.exit), problem lines $($r.bad.Count), manual check: $(if ($manual.Count) { $manual -join ', ' } else { 'none' })" $tour
    } else { Line "SKIP" "menu tour" "no $TourScript or tools\shot_compare.ps1 yet" "-" }

    if (Test-Path (Join-Path $Repo "tests\input\$SplitScript")) {
        $split = New-RunDir (Join-Path $Out "split")
        $want = Shots $SplitScript
        $r = GameRun "split" $SplitScript ([int]$want * 6 + 60) $split
        $got = @(Get-ChildItem $split -Filter *.bmp).Count
        Line $(if ($got -eq $want -and $r.exit -eq 0 -and $r.bad.Count -eq 0) { "PASS" } else { "FAIL" }) "split" `
            "$got of $want shots, exit $($r.exit), problem lines $($r.bad.Count)" $split
    } else { Line "SKIP" "split" "no $SplitScript yet" "-" }

    if (Test-Path (Join-Path $Repo $StorageTest)) {
        $stOut = Join-Path $Out "storage"
        $st = @(& (Join-Path $Repo $StorageTest) -Exe $Exe -Out $stOut *>&1)
        $stExit = $LASTEXITCODE
        $st | Set-Content (Join-Path $Out "storage.txt")
        $fails = @($st | Where-Object { "$_" -match "^\s*FAIL" }).Count
        Line $(if ($stExit -eq 0 -and $fails -eq 0) { "PASS" } else { "FAIL" }) "storage" "$fails failed check(s)" $stOut
    } else { Line "SKIP" "storage" "no $StorageTest yet" "-" }
} finally {
    Exit-GlobalLock $lock
}
$summary | Set-Content (Join-Path $Out "summary.txt")
Write-Host $(if ($failed) { "regress: $failed part(s) not passed" } else { "regress: all parts passed" })
exit $(if ($failed) { 1 } else { 0 })
