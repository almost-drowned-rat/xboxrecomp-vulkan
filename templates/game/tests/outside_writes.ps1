# outside_writes.ps1 - fails if the port writes anywhere outside its install folder during a short run.
#
#   powershell -ExecutionPolicy Bypass -File tests\outside_writes.ps1 -Layout "<...>\<Game> Recomp" -Exe <Game>.exe
#       [-Sec 60] [-Script tests\input\main_menu.txt]
#
# The portable install layout is a hard rule (toolkit docs/porting_guide.md, docs/storage.md): nothing is written
# outside "<Game> Recomp\" - no AppData, no registry, no temp files left behind. This test (after the THPS 2X
# storage test, tests\storage.ps1 there) runs the exe for -Sec seconds against a fresh copy of saves\ and data\
# inside a temporary copy of the layout's folders, with TEMP/TMP pointed at a catch folder, and compares:
#   - %APPDATA%, %LOCALAPPDATA%, %PROGRAMDATA%, Documents: new entries and changed files (two levels deep; a
#     folder's own date changes whenever any program writes into it, so changed folders do not count)
#   - the catch folder that TEMP/TMP point to during the run: anything left behind
#   - HKCU\Software, HKCU\Software\Classes: new keys
# Other programs running at the same time can write there too: every hit is listed with its path, so a false
# positive is easy to tell from the port. Exit code 0 = nothing written outside, 1 = something was.
# Hold the build/test lock (tools\lock.ps1, class shared) when other agents use the machine.
param(
    [Parameter(Mandatory = $true)][string]$Layout,
    [Parameter(Mandatory = $true)][string]$Exe,
    [int]$Sec = 60,
    [string]$Script
)
$ErrorActionPreference = "Stop"
$work = Join-Path $env:LOCALAPPDATA "Temp\outside_writes_$PID"   # removed again below
$realTemp = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
$roots = @($env:APPDATA, $env:LOCALAPPDATA, $env:ProgramData, [Environment]::GetFolderPath("MyDocuments")) |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

function Get-Snapshot {
    $files = @{}
    foreach ($r in $roots) {
        foreach ($i in Get-ChildItem -LiteralPath $r -Force -ErrorAction SilentlyContinue) {
            if ($i.FullName.StartsWith($realTemp, [StringComparison]::OrdinalIgnoreCase)) { continue }   # see the catch folder
            $files[$i.FullName] = @($i.LastWriteTimeUtc, $i.PSIsContainer)
            if ($i.PSIsContainer) {
                foreach ($j in Get-ChildItem -LiteralPath $i.FullName -Force -ErrorAction SilentlyContinue) {
                    $files[$j.FullName] = @($j.LastWriteTimeUtc, $j.PSIsContainer)
                }
            }
        }
    }
    $keys = @{}
    foreach ($k in "HKCU:\Software", "HKCU:\Software\Classes") {
        foreach ($s in Get-ChildItem $k -ErrorAction SilentlyContinue) { $keys[$s.Name] = 1 }
    }
    return @{ Files = $files; Keys = $keys }
}

$saves = Join-Path $Layout "saves"; $data = Join-Path $Layout "data"
$tmpSaves = Join-Path $Layout "saves.outside_writes"; $tmpData = Join-Path $Layout "data.outside_writes"
$catch = Join-Path $Layout "data.outside_writes\temp_catch"
Remove-Item -Recurse -Force $tmpSaves, $tmpData -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmpSaves, $tmpData, $catch | Out-Null

$before = Get-Snapshot
$t0 = [DateTime]::UtcNow
$args = @("--max-seconds", "$Sec", "--save-dir", "`"$tmpSaves`"", "--data-dir", "`"$tmpData`"")
if ($Script) { $args += @("--input-script", "`"$Script`"") }
$oldTemp = $env:TEMP; $oldTmp = $env:TMP
$env:TEMP = $catch; $env:TMP = $catch
try {
    $p = Start-Process -FilePath (Join-Path $Layout $Exe) -WorkingDirectory $Layout -ArgumentList $args -PassThru -Wait
} finally { $env:TEMP = $oldTemp; $env:TMP = $oldTmp }
Start-Sleep -Seconds 2
$after = Get-Snapshot

$hits = @()
foreach ($f in $after.Files.Keys) {
    if ($f -like "$work*") { continue }
    if (-not $before.Files.ContainsKey($f)) { $hits += "new      $f" }
    elseif (-not $after.Files[$f][1] -and $after.Files[$f][0] -gt $t0 -and $after.Files[$f][0] -ne $before.Files[$f][0]) {
        $hits += "changed  $f"
    }
}
foreach ($k in $after.Keys.Keys) { if (-not $before.Keys.ContainsKey($k)) { $hits += "registry $k" } }
foreach ($i in Get-ChildItem -LiteralPath $catch -Recurse -Force -ErrorAction SilentlyContinue) { $hits += "temp     $($i.FullName) (left in TEMP)" }

"exe exit code: $($p.ExitCode)"
Remove-Item -Recurse -Force $tmpSaves, $tmpData -ErrorAction SilentlyContinue
# GPU driver shader caches: every Vulkan/D3D/GL program makes the driver write there, the port cannot stop it
# (THPS 2X, RTX 3050 Ti: AppData\Local\NVIDIA\DXCache). Listed, not counted.
$driver = '\\NVIDIA\\(DXCache|GLCache|ComputeCache)|\\NVIDIA Corporation\\NV_Cache|\\AMD\\(DxCache|DxcCache|VkCache|GLCache)|\\Intel\\ShaderCache|\\D3DSCache'
$info = @($hits | Where-Object { $_ -match $driver })
$hits = @($hits | Where-Object { $_ -notmatch $driver })
foreach ($i in $info) { "  info (GPU driver cache, not the port): $i" }
if ($hits.Count) {
    "written outside the install folder ($($hits.Count)) - check each path, other programs can cause some:"
    $hits | Sort-Object | ForEach-Object { "  $_" }
    exit 1
}
"nothing written outside $Layout"
exit 0
