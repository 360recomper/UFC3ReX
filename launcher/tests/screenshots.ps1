# Restyle check: screenshots of every page and dialog at 100% and 150% display scale (simulated, see Tabs/Shots.cs).
# Builds a fake play folder in -Scratch (the "game exe" is a text file holding the native marker, never a program),
# then runs the launcher with the hidden --screenshot-all option twice. Nothing is started but the launcher itself.
#   launcher\tests\screenshots.ps1 [-Out <png folder>] [-Scratch <scratch folder>] [-Bare]
# -Bare: no ufc3.exe and no game files (the first-run look: Play off, Setup selected is not shot, but disabled states show)
param(
  [string]$Out = (Join-Path $PSScriptRoot '..\..\reports\launcher-restyle'),
  [string]$Scratch = (Join-Path $env:TEMP 'ufc3-launcher-shots'),
  [switch]$Bare
)
$ErrorActionPreference = 'Stop'
$work = Join-Path $Scratch 'play'
$pub = Join-Path $Scratch '_publish'
dotnet publish (Join-Path (Split-Path $PSScriptRoot -Parent) 'UFC3Launcher.csproj') -c Release -o $pub -v q -nologo | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'publish failed' }
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force "$work\assets", "$work\mods" | Out-Null
Copy-Item "$pub\UFC3 Launcher.exe" $work
if (-not $Bare) {
  [IO.File]::WriteAllText("$work\ufc3.exe", 'not a program; native: renderer on (experimental) (fake marker)')
  Set-Content "$work\assets\default.xex" 'fake'
  Set-Content "$work\ufc3.toml" "resolution_scale = 3`nufc3_renderer = `"native`"`n"
}
Set-Content "$work\launcher.json" '{ "CheckForUpdates": false }'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Get-ChildItem $Out -Filter *.png | Remove-Item
foreach ($p in 100, 150) {
  $r = Start-Process "$work\UFC3 Launcher.exe" -ArgumentList '--screenshot-all', "`"$Out`"", $p -Wait -PassThru
  "scale $p exit $($r.ExitCode)"
}
Get-ChildItem $Out | Select-Object Name, Length | Format-Table -AutoSize
