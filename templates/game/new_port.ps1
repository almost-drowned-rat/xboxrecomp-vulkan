# new_port.ps1 - create a game port repo from templates\game (see README.md).
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Interactive script.')]
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Prefix,
    [string]$Region = "USA",
    [string]$Iso = "",
    [string]$XbeSha1 = "0000000000000000000000000000000000000000",
    [string]$TitleId = "00000000"
)
$ErrorActionPreference = "Stop"
if (Test-Path $Dest) { throw "$Dest exists - choose a new folder" }
$map = @{ '@TITLE@' = $Title; '@NAME@' = $Name; '@EXE@' = $Exe; '@PREFIX@' = $Prefix; '@REGION@' = $Region;
          '@ISO@' = $Iso; '@XBESHA1@' = $XbeSha1; '@TITLEID@' = $TitleId }
$src = $PSScriptRoot
Get-ChildItem $src -Recurse -File | Where-Object { $_.Name -ne 'new_port.ps1' -and $_.Name -ne 'README.md' } | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length + 1)
    if ($rel -eq 'README.port.md') { $rel = 'README.md' }   # the port's own README (AI-assistance note, A13)
    $out = Join-Path $Dest $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
    $text = [IO.File]::ReadAllText($_.FullName)
    foreach ($k in $map.Keys) { $text = $text.Replace($k, $map[$k]) }
    [IO.File]::WriteAllText($out, $text, (New-Object Text.UTF8Encoding $false))
}
Write-Host "Created $Dest - next: docs/porting_guide.md, M0"
