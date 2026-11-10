# Music tab check, headless: publishes the launcher, copies it to a scratch play folder and runs --music-selftest
# (generated tone -> trimmed from 5 s -> m4a -> fighter_music.toml round trip). Starts no game and opens no window.
#   launcher\tests\music-test.ps1 [-Assets <extracted game files>] [-Scratch <folder>]
param(
  [string]$Assets = '',
  [string]$Scratch = (Join-Path $env:TEMP 'ufc3-music-test')
)
$ErrorActionPreference = 'Stop'
$pub = Join-Path $Scratch '_publish'
dotnet publish (Join-Path (Split-Path $PSScriptRoot -Parent) 'UFC3Launcher.csproj') -c Release -o $pub -v q -nologo | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'publish failed' }
$work = Join-Path $Scratch 'play'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item "$pub\UFC3 Launcher.exe" $work
if ($Assets) { $a = $Assets.Replace([string][char]92, '/'); Set-Content "$work\ufc3.toml" "game_data_root = `"$a`"" }
$p = Start-Process "$work\UFC3 Launcher.exe" -ArgumentList '--music-selftest' -Wait -PassThru
Get-Content "$work\music-selftest-log.txt"
"exit $($p.ExitCode)"
exit $p.ExitCode
