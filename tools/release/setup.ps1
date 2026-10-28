# TLMP (Torchlight co-op) installer, run by Install.bat / Uninstall.bat from the release zip.
#   setup.ps1 [-GameDir <Torchlight folder>] [-Uninstall]
# Finds the Torchlight folder (next to this file, GOG, Steam, or asks), checks that Torchlight.exe is
# the supported build, puts TLMP's two DLLs in and registers the plugin. Uninstall puts the game's
# own files back; TLMP.ini, TLMP_logs and tlmp_profiles are left.
param(
  [string]$GameDir,
  [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$expectedStamp = 0x4FFC6815 # Torchlight.exe 1.7.8.70 (GOG); TLMP's hooks are address-based

function Fail([string]$msg) {
  Write-Host ''
  Write-Host $msg -ForegroundColor Red
  exit 1
}

function Test-GameDir([string]$dir) {
  return $dir -and (Test-Path (Join-Path $dir 'Torchlight.exe'))
}

function Find-GameDir {
  foreach ($d in @($here, (Split-Path $here))) {
    if (Test-GameDir $d) { return $d }
  }
  foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1207659069', 'HKLM:\SOFTWARE\GOG.com\Games\1207659069') {
    try {
      $p = (Get-ItemProperty $key -ErrorAction Stop).path
      if (Test-GameDir $p) { return $p }
    } catch {}
  }
  try {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
    $p = Join-Path $steam 'steamapps\common\Torchlight'
    if (Test-GameDir $p) { return $p }
  } catch {}
  foreach ($p in 'C:\GOG Games\Torchlight', 'C:\Program Files (x86)\GOG Galaxy\Games\Torchlight',
                 'C:\Program Files (x86)\Steam\steamapps\common\Torchlight') {
    if (Test-GameDir $p) { return $p }
  }
  Write-Host 'Torchlight was not found automatically; please pick its folder.'
  Add-Type -AssemblyName System.Windows.Forms
  $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
  $dlg.Description = 'Select your Torchlight folder (the one with Torchlight.exe in it)'
  if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK -and (Test-GameDir $dlg.SelectedPath)) {
    return $dlg.SelectedPath
  }
  return $null
}

function Get-BuildStamp([string]$exe) {
  $fs = [IO.File]::OpenRead($exe)
  try {
    $buf = New-Object byte[] 4096
    [void]$fs.Read($buf, 0, $buf.Length)
  } finally { $fs.Close() }
  $pe = [BitConverter]::ToInt32($buf, 0x3C)
  return [BitConverter]::ToUInt32($buf, $pe + 8)
}

# Our proxy OgreGUIRenderer.dll imports TLMP.dll; the game's own doesn't.
function Test-OurProxy([string]$dll) {
  if (-not (Test-Path $dll)) { return $false }
  $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
  return $text.Contains('TLMP.dll')
}

function Test-Writable([string]$dir) {
  $probe = Join-Path $dir 'tlmp-write-test.tmp'
  try {
    [IO.File]::WriteAllText($probe, 'x')
    Remove-Item $probe -Force
    return $true
  } catch { return $false }
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not (Test-GameDir $GameDir)) { Fail 'No Torchlight folder was chosen; nothing was changed.' }
$GameDir = (Resolve-Path $GameDir).Path
Write-Host "Torchlight folder: $GameDir"

$running = Get-Process -Name 'Torchlight' -ErrorAction SilentlyContinue
if ($running) { Fail 'Torchlight is running. Close the game, then run this again.' }

# Folders under Program Files need administrator rights: ask for them (once) and start over.
if (-not (Test-Writable $GameDir)) {
  $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
  if ($isAdmin) { Fail "Can't write to $GameDir." }
  Write-Host 'The Torchlight folder needs administrator rights to change; asking for them...'
  $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -GameDir `"$GameDir`""
  if ($Uninstall) { $argList += ' -Uninstall' }
  Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait
  exit 0
}

$cfg = Join-Path $GameDir 'Plugins.cfg'
$line = 'Plugin=TLMP'
$proxy = Join-Path $GameDir 'OgreGUIRenderer.dll'
$orig = Join-Path $GameDir 'OgreGUIRenderer_orig.dll'

if ($Uninstall) {
  if (Test-Path $cfg) {
    $lines = Get-Content $cfg | Where-Object { $_.Trim() -ne $line }
    Set-Content -Path $cfg -Value $lines -Encoding ascii
  }
  if ((Test-Path $orig) -and -not (Test-OurProxy $orig)) { Move-Item $orig $proxy -Force }
  foreach ($f in 'TLMP.dll', 'TLMP.pdb', 'OgreGUIRenderer.pdb') {
    Remove-Item (Join-Path $GameDir $f) -ErrorAction SilentlyContinue
  }
  Write-Host ''
  Write-Host 'TLMP removed. (TLMP.ini, TLMP_logs and tlmp_profiles were left in the Torchlight folder.)' -ForegroundColor Green
  exit 0
}

$stamp = Get-BuildStamp (Join-Path $GameDir 'Torchlight.exe')
if ($stamp -ne $expectedStamp) {
  $msg = ("This Torchlight.exe is a different version (build {0:X8}). TLMP works only with Torchlight " +
          "1.7.8.70 as sold by GOG (build {1:X8}). Nothing was changed.") -f $stamp, $expectedStamp
  Fail $msg
}
foreach ($f in 'TLMP.dll', 'OgreGUIRenderer.dll', 'TLMP.ini') {
  if (-not (Test-Path (Join-Path $here $f))) { Fail "$f is missing next to this installer; extract the whole zip first." }
}

# Keep the game's own OgreGUIRenderer.dll as OgreGUIRenderer_orig.dll (our proxy loads it), once.
if (-not (Test-Path $orig)) {
  if (Test-OurProxy $proxy) {
    Fail ('The game''s own OgreGUIRenderer.dll is missing (an earlier install was damaged). Repair or ' +
          'verify the game''s files in GOG Galaxy / Steam, then run this again.')
  }
  Move-Item $proxy $orig
} elseif (Test-OurProxy $orig) {
  Fail ('OgreGUIRenderer_orig.dll is not the game''s own file (an earlier install was damaged). Repair or ' +
        'verify the game''s files, delete OgreGUIRenderer_orig.dll, then run this again.')
}
Copy-Item (Join-Path $here 'OgreGUIRenderer.dll') $proxy -Force
Copy-Item (Join-Path $here 'TLMP.dll') $GameDir -Force

if (-not (Test-Path "$cfg.tlmp-backup")) { Copy-Item $cfg "$cfg.tlmp-backup" }
if (-not (Get-Content $cfg | Where-Object { $_.Trim() -eq $line })) {
  Add-Content -Path $cfg -Value $line -Encoding ascii
}

$ini = Join-Path $GameDir 'TLMP.ini'
if (-not (Test-Path $ini)) {
  Copy-Item (Join-Path $here 'TLMP.ini') $ini
} else {
  Write-Host 'Kept your TLMP.ini.'
}

Write-Host ''
Write-Host 'TLMP is installed. Start Torchlight: the title screen now has Host Co-op and Join Co-op.' -ForegroundColor Green
