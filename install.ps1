param(
  [string]$GameDir = 'D:\GAMES\Torchlight',
  [string]$Config = 'RelWithDebInfo',
  [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$cfg = Join-Path $GameDir 'Plugins.cfg'
$backup = "$cfg.tlmp-backup"
$line = 'Plugin=TLMP'
$proxy = Join-Path $GameDir 'OgreGUIRenderer.dll'
$orig = Join-Path $GameDir 'OgreGUIRenderer_orig.dll'

if (-not (Test-Path (Join-Path $GameDir 'Torchlight.exe'))) { throw "Torchlight.exe not found in $GameDir" }

if ($Uninstall) {
  $lines = Get-Content $cfg | Where-Object { $_.Trim() -ne $line }
  Set-Content -Path $cfg -Value $lines -Encoding ascii
  if (Test-Path $orig) { Move-Item $orig $proxy -Force }
  foreach ($f in 'TLMP.dll', 'TLMP.pdb', 'OgreGUIRenderer.pdb') {
    Remove-Item (Join-Path $GameDir $f) -ErrorAction SilentlyContinue
  }
  Write-Host 'TLMP removed (TLMP.ini, tlmp_profiles and logs left in place).'
  return
}

$src = Join-Path $PSScriptRoot "build\$Config"

# Rename the real OgreGUIRenderer.dll exactly once; after that the file in place is ours.
if (-not (Test-Path $orig)) { Move-Item $proxy $orig }
Copy-Item (Join-Path $src 'OgreGUIRenderer.dll') $proxy -Force
Copy-Item (Join-Path $src 'TLMP.dll') $GameDir -Force
foreach ($pdb in 'TLMP.pdb', 'OgreGUIRenderer.pdb') {
  if (Test-Path (Join-Path $src $pdb)) { Copy-Item (Join-Path $src $pdb) $GameDir -Force }
}
Remove-Item (Join-Path $GameDir 'tlmp-launch.exe') -ErrorAction SilentlyContinue

if (-not (Test-Path $backup)) { Copy-Item $cfg $backup }
if (-not (Get-Content $cfg | Where-Object { $_.Trim() -eq $line })) {
  Add-Content -Path $cfg -Value $line -Encoding ascii
}

$ini = Join-Path $GameDir 'TLMP.ini'
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $PSScriptRoot 'TLMP.ini') $ini }

Write-Host "TLMP installed into $GameDir"
