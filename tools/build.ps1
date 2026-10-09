# Quiet build: errors/warnings only, then one OK line. Forwards to
# ..\dlaa-core\tools\build.ps1 (options: -Config, -Target a,b, -Clangd, -Reconfigure).
#
#   tools\build.ps1 -Install    # build, then install d3d11.dll + merged DS3_DLSS.ini into the game
#
# Install: refuses while DarkSoulsIII.exe runs, backs up the game's dll/ini to
# build\install_backup\<time>, keeps every value the game ini already has, and
# verifies the copy. Touches nothing else in the Game folder.
param(
  [switch]$Install,
  [string]$GameDir = 'G:\SteamLibrary\steamapps\common\DARK SOULS III\Game',
  [string]$Config = 'Release',
  [string[]]$Target = @(),
  [switch]$Clangd,
  [switch]$Reconfigure
)
$repo = Split-Path -Parent $PSScriptRoot
$fwd = @{ Root = $repo; Config = $Config; Target = $Target; Clangd = $Clangd; Reconfigure = $Reconfigure }
& (Join-Path $PSScriptRoot '..\..\dlaa-core\tools\build.ps1') @fwd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $Install) { exit 0 }

$dll = Join-Path $repo "build\$Config\d3d11.dll"
$ini = Join-Path $repo 'DS3_DLSS.ini'
$gameDll = Join-Path $GameDir 'd3d11.dll'
$gameIni = Join-Path $GameDir 'DS3_DLSS.ini'
if (Get-Process DarkSoulsIII -ErrorAction SilentlyContinue) {
  Write-Output 'NOT INSTALLED: DarkSoulsIII.exe is running (the dll is locked)'
  exit 2
}

$backup = Join-Path $repo ('build\install_backup\' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force $backup | Out-Null
foreach ($f in $gameDll, $gameIni) { if (Test-Path $f) { Copy-Item $f $backup } }

# Merge: the repo's lines, comments and keys, with the game ini's value wherever
# it already has that [section] key. Section headers are whole lines only.
function Read-IniValues([string]$path) {
  $values = @{}; $section = ''
  if (-not (Test-Path $path)) { return $values }
  foreach ($l in [IO.File]::ReadAllLines($path)) {
    if ($l -match '^\s*\[([^\]]+)\]\s*$') { $section = $Matches[1].Trim().ToLower(); continue }
    if ($l -match '^\s*[;#]') { continue }
    if ($l -match '^\s*([^=]+?)\s*=\s*(.*?)\s*$') { $values["$section|$($Matches[1].ToLower())"] = $Matches[2] }
  }
  $values
}
$user = Read-IniValues $gameIni
$text = [IO.File]::ReadAllText($ini)
$nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$section = ''; $kept = @()
$merged = foreach ($l in ($text -split "`r?`n")) {
  if ($l -match '^\s*\[([^\]]+)\]\s*$') { $section = $Matches[1].Trim(); $l; continue }
  if ($l -notmatch '^\s*[;#]' -and $l -match '^(\s*([^=]+?)\s*=\s*)(.*?)\s*$') {
    $k = "$($section.ToLower())|$($Matches[2].ToLower())"
    if ($user.ContainsKey($k) -and $user[$k] -ne $Matches[3]) {
      $kept += "[$section] $($Matches[2])=$($user[$k])"
      $Matches[1] + $user[$k]; continue
    }
  }
  $l
}
[IO.File]::WriteAllText($gameIni, ($merged -join $nl), [Text.Encoding]::ASCII)
Copy-Item $dll $gameDll -Force

$ok = (Get-FileHash $dll).Hash -eq (Get-FileHash $gameDll).Hash
Write-Output ("INSTALLED  dll hash {0}  backup {1}" -f ($(if ($ok) { 'match' } else { 'MISMATCH' })), $backup.Replace("$repo\", ''))
if ($kept) { Write-Output ('  kept game ini values: ' + ($kept -join ', ')) }
if (-not $ok) { exit 1 }
exit 0
