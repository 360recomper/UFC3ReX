# Builds the two public downloads (GitHub Releases, like RRXX-Recomp-vX + RRXX-DLSS5-Addon-vX):
#   <OutDir>\UFC3-Recomp-<Version>.zip       launcher + game exe, no game files
#   <OutDir>\UFC3-DLSS5-Addon-<Version>.zip  our DLSS 5 scripts/templates + README, no third-party files
#   tools\release.ps1 [-GameBuild <dir with ufc3.exe>] [-OutDir dist\<date>] [-Version v1.0] [-AllowRdDlls]
# The Mod Pack is separate (tools\build_modpack.py, hosted elsewhere).
# Recomp zip, inside one UFC3-Recomp-<Version>\ folder: UFC3 Launcher.exe, ufc3.exe, SDK DLLs + app-local
# VC++ runtime, ufc3.toml, dlss5\ (scripts + templates), README.txt, LICENSE.txt, CREDITS.md, empty mods\.
# Builds with the native renderer also carry the shader cache tools: ufc3_shader_cache.exe, XenosRecomp.exe,
# ufc3_prelink.exe, dxcompiler.dll, dxil.dll (the launcher runs them on the player's own disc files).
# Plus pipelines_seed.bin (config\native) so first-use pipelines are built at load, not mid-fight.
# No game files, PDB, assets, saves, shader caches or logs.
# Default GameBuild: out\build\win-amd64-release (tools\build_release.bat); the RelWithDebInfo build
# works with -AllowRdDlls.
param(
  [string]$GameBuild,
  [string]$Date = (Get-Date -Format 'yyyy-MM-dd'),
  [string]$OutDir = "$(Split-Path $PSScriptRoot -Parent)\dist\$Date",
  [string]$Version = 'v1.0',
  [switch]$AllowRdDlls
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$main = Split-Path $PSScriptRoot -Parent   # untracked launcher\publish may live in the main checkout

if (-not $GameBuild) {
  foreach ($c in "$root\out\build\win-amd64-release", "$main\out\build\win-amd64-release") {
    if (Test-Path "$c\ufc3.exe") { $GameBuild = $c; break }
  }
}
if (-not $GameBuild -or -not (Test-Path "$GameBuild\ufc3.exe")) { throw "no ufc3.exe: pass -GameBuild" }
$launcher = @("$root\launcher\publish\UFC3 Launcher.exe", "$main\launcher\publish\UFC3 Launcher.exe") |
  Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $launcher) { throw "no launcher: dotnet publish launcher -c Release -o launcher\publish" }

$name = "UFC3-Recomp-$Version"
$stageRoot = Join-Path $OutDir "_release_stage"
$stage = Join-Path $stageRoot $name
$zip = Join-Path $OutDir "$name.zip"
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (Test-Path $stageRoot) { Remove-Item -Recurse -Force $stageRoot }   # no junctions in here: we make it
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage, "$stage\mods", "$stage\dlss5\templates" | Out-Null

Copy-Item $launcher $stage
Copy-Item "$GameBuild\ufc3.exe" $stage
Get-ChildItem $GameBuild -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $stage }
$rd = Get-ChildItem $stage -Filter *.dll | Where-Object { $_.Name -match 'rd\.dll$|Tracy' }
if ($rd -and -not $AllowRdDlls) { throw "dev (rd/Tracy) DLLs in package: $($rd.Name -join ', ') - use a Release build or -AllowRdDlls" }

# Native renderer builds: same check as the launcher (launcher/NativeRenderer.cs). Without these five the launcher
# can't build the cache and Native falls back to Emulated, so a native package missing one is an error.
$exeText = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes("$GameBuild\ufc3.exe"))
if ($exeText.Contains('native: renderer on (experimental')) {
  foreach ($f in 'ufc3_shader_cache.exe', 'XenosRecomp.exe', 'ufc3_prelink.exe', 'dxcompiler.dll', 'dxil.dll') {
    if (-not (Test-Path "$GameBuild\$f")) { throw "native build without $f in $GameBuild : build it with build.bat native" }
    Copy-Item "$GameBuild\$f" $stage -Force
  }
  # Pipeline seed (NR-15b): the game reads pipelines_seed.bin beside shader_cache.bin (beside the exe by default).
  # Without it every pipeline compiles on first use during play. Shader content hashes only, no game data.
  $seed = "$root\config\native\pipelines_seed.bin"
  if (-not (Test-Path $seed)) { throw "native build without $seed (tools\nr_pso_seed.py)" }
  Copy-Item $seed $stage -Force
  'Native renderer build: shader cache tools + pipeline seed included'
}
Remove-Variable exeText

# App-local VC++ runtime so players need no redistributable installer.
$crt = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -ErrorAction SilentlyContinue |
  Sort-Object Name -Descending | Select-Object -First 1
if ($crt) {
  foreach ($d in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt.FullName $d) $stage
  }
} else { Write-Warning 'VC++ redist folder not found: CRT DLLs not bundled' }

# DLSS 5 scripts + templates the launcher uses (binaries come from the DLSS 5 tab's setup).
Copy-Item "$root\dlss5\templates\*" "$stage\dlss5\templates" -Force
foreach ($f in 'align-dlss5-panel.ps1', 'show-dlss5-settings.ps1', 'show-dlss5-settings.bat') {
  Copy-Item "$root\dlss5\$f" "$stage\dlss5"
}

# Default settings: no paths (the exe uses assets\, userdata\, mods\ next to itself).
@"
# UFC Undisputed 3 Recomp settings (F4 in game edits these)
resolution_scale = 2
"@ | Set-Content -Encoding ascii "$stage\ufc3.toml"

Copy-Item "$PSScriptRoot\release\README.txt" "$stage\README.txt"
# GPL-3.0: the licence and the third-party credits travel with every download.
Copy-Item "$PSScriptRoot\..\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$PSScriptRoot\..\CREDITS.md" "$stage\CREDITS.md"

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } |
  Select-Object @{n = 'File'; e = { $_.FullName.Substring($stage.Length + 1) } }, Length | Format-Table -AutoSize
Remove-Item -Recurse -Force $stageRoot

# DLSS 5 add-on: our files only (CRLF text), nothing third-party.
$azip = Join-Path $OutDir "UFC3-DLSS5-Addon-$Version.zip"
if (Test-Path $azip) { Remove-Item -Force $azip }
$map = [ordered]@{
  'dlss5/README-DLSS5.txt'        = "$PSScriptRoot\release\README-DLSS5.txt"
  'dlss5/align-dlss5-panel.ps1'   = "$root\dlss5\align-dlss5-panel.ps1"
  'dlss5/show-dlss5-settings.ps1' = "$root\dlss5\show-dlss5-settings.ps1"
  'dlss5/show-dlss5-settings.bat' = "$root\dlss5\show-dlss5-settings.bat"
}
Get-ChildItem "$root\dlss5\templates" -File | ForEach-Object { $map["dlss5/templates/$($_.Name)"] = $_.FullName }
$z = [IO.Compression.ZipFile]::Open($azip, 'Create')
try {
  foreach ($k in $map.Keys) {
    $text = [IO.File]::ReadAllText($map[$k]) -replace "`r?`n", "`r`n"
    $e = $z.CreateEntry($k, 'Optimal')
    $w = New-Object IO.StreamWriter($e.Open(), (New-Object Text.UTF8Encoding($false)))
    $w.Write($text); $w.Dispose()
  }
} finally { $z.Dispose() }

"GameBuild: $GameBuild"
"ZIP:   $zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)  sha256 $((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower())"
"ADDON: $azip  ($([math]::Round((Get-Item $azip).Length / 1KB, 1)) KB)  sha256 $((Get-FileHash -Algorithm SHA256 $azip).Hash.ToLower())"
