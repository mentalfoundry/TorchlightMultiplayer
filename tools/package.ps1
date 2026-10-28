# Builds the release zip players install from: dist\TLMP-<date>-<rev>.zip with the two DLLs, a
# default TLMP.ini, Install.bat / Uninstall.bat (tools\release\setup.ps1) and README.txt. The PDBs go
# to dist\symbols\<name>\ (not in the zip), for reading crash logs from that build.
#   .\tools\package.ps1            (builds first)
#   .\tools\package.ps1 -NoBuild   (packages what build\RelWithDebInfo has)
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$config = 'RelWithDebInfo' # the configuration the test rig runs

if (-not $NoBuild) { & (Join-Path $root 'build.ps1') -Config $config }
$bin = Join-Path $root "build\$config"

$versionHeader = Get-Content (Join-Path $root 'build\generated\version.h') -Raw
if ($versionHeader -notmatch '#define TLMP_VERSION "([^"]+)"') { throw 'No TLMP_VERSION in build\generated\version.h' }
$version = $Matches[1]
if ($version.EndsWith('+')) {
  Write-Warning "Built from uncommitted changes ($version): commit first for a build others can match."
}
$name = 'TLMP-' + ($version -replace ' ', '-' -replace '\+$', '-dev')

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $bin 'TLMP.dll'), (Join-Path $bin 'OgreGUIRenderer.dll') $stage
Copy-Item (Join-Path $root 'TLMP.ini') $stage
foreach ($f in 'setup.ps1', 'Install.bat', 'Uninstall.bat') {
  Copy-Item (Join-Path $PSScriptRoot "release\$f") $stage
}
# The readme with this build's id, with Windows line endings for Notepad.
$readme = (Get-Content (Join-Path $PSScriptRoot 'release\README.txt') -Raw -Encoding UTF8).Replace('{VERSION}', $version)
$readme = $readme -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme, (New-Object Text.UTF8Encoding $false))
$ini = (Get-Content (Join-Path $stage 'TLMP.ini') -Raw) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'TLMP.ini'), $ini, (New-Object Text.ASCIIEncoding))

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

$symbols = Join-Path $dist "symbols\$name"
New-Item -ItemType Directory -Force $symbols | Out-Null
foreach ($pdb in 'TLMP.pdb', 'OgreGUIRenderer.pdb') {
  if (Test-Path (Join-Path $bin $pdb)) { Copy-Item (Join-Path $bin $pdb) $symbols -Force }
}

Write-Host "Package: $zip"
Write-Host "Symbols: $symbols"
