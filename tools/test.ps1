# Runs a co-op test scenario: a host and N clients on this machine, each playing its own script
# (tests\scenarios\<name>\host.txt, client.txt / client2.txt ...), then collects results, dumps,
# screenshots (as PNG) and TLMP logs into tests\runs\<name>-<time>\.
#
#   .\tools\test.ps1 smoke
#   .\tools\test.ps1 handover -Clients 1 -Timeout 400
#
# Uses the test profiles t1 (host), t2.. (clients) under <game>\tlmp_profiles, whose saves are reset
# from tlmp_profiles\_test_seed before every run. The real profiles (p1, p2) are never touched.
param(
  [Parameter(Mandatory)][string]$Scenario,
  [int]$Clients = 1,
  [int]$Timeout = 300,
  [string]$Game = 'D:\GAMES\Torchlight'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path $PSScriptRoot
$scen = Join-Path $root "tests\scenarios\$Scenario"
if (-not (Test-Path "$scen\host.txt")) { throw "no scenario at $scen" }
$profiles = Join-Path $Game 'tlmp_profiles'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = Join-Path $root "tests\runs\$Scenario-$stamp"
New-Item -ItemType Directory -Force $run | Out-Null

# Fresh saves for every test profile in play.
$names = @('t1') + (1..$Clients | ForEach-Object { "t$($_ + 1)" })
foreach ($n in $names) {
  $seed = Join-Path $profiles "_test_seed\$n\Runic Games\Torchlight\Save"
  $save = Join-Path $profiles "$n\Runic Games\Torchlight\Save"
  if (-not (Test-Path $seed)) { throw "no seed save for $n ($seed)" }
  Remove-Item $save -Recurse -Force -ErrorAction SilentlyContinue
  Copy-Item $seed $save -Recurse
}
Remove-Item (Join-Path $Game 'tlmp_test') -Recurse -Force -ErrorAction SilentlyContinue

$exe = Join-Path $Game 'Torchlight.exe'
$procs = @()
# A scenario folder with a file named "menu" starts the games without -tlmp-host / -tlmp-join:
# its scripts start the session through the title screen's co-op buttons.
$menu = Test-Path "$scen\menu"
$hostArgs = if ($menu) { @() } else { @('-tlmp-host') }
$procs += Start-Process -FilePath $exe -WorkingDirectory $Game -PassThru -ArgumentList (@(
  '-tlmp-profile', 't1') + $hostArgs + @('-tlmp-name', 'TestHost', '-tlmp-test', "`"$scen\host.txt`""))
Start-Sleep -Seconds 5
for ($i = 1; $i -le $Clients; $i++) {
  $script = if ($i -eq 1 -and -not (Test-Path "$scen\client1.txt")) { "$scen\client.txt" } else { "$scen\client$i.txt" }
  $joinArgs = if ($menu) { @() } else { @('-tlmp-join', '127.0.0.1') }
  $procs += Start-Process -FilePath $exe -WorkingDirectory $Game -PassThru -ArgumentList (@(
    '-tlmp-profile', "t$($i + 1)") + $joinArgs + @('-tlmp-name', "TestClient$i", '-tlmp-test', "`"$script`""))
  Start-Sleep -Seconds 2
}

$deadline = (Get-Date).AddSeconds($Timeout)
while ((Get-Date) -lt $deadline -and ($procs | Where-Object { -not $_.HasExited })) { Start-Sleep -Seconds 2 }
$timedOut = @($procs | Where-Object { -not $_.HasExited })
foreach ($p in $timedOut) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 1

# Collect.
$out = Join-Path $Game 'tlmp_test'
if (Test-Path $out) { Copy-Item "$out\*" $run -Recurse -Force }
foreach ($p in $procs) {
  $log = Join-Path $Game "TLMP_logs\TLMP_$($p.Id).log"
  if (Test-Path $log) { Copy-Item $log $run }
}
Get-ChildItem $run -Recurse -Filter *.bmp | ForEach-Object {
  $img = [System.Drawing.Image]::FromFile($_.FullName)
  $img.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
  $img.Dispose()
  Remove-Item $_.FullName
}

"run: $run"
if ($timedOut.Count) { "TIMED OUT: $($timedOut.Count) game(s) still running after $Timeout s were killed" }
# A script may hold Shift (`key shift down`); never leave it stuck if a game was killed mid-way.
Add-Type -Namespace TlmpTest -Name Keys -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
[TlmpTest.Keys]::keybd_event(0x10, 0, 2, [System.UIntPtr]::Zero)
Get-ChildItem $run -Directory | ForEach-Object {
  "== $($_.Name)"
  $r = Join-Path $_.FullName 'results.txt'
  if (Test-Path $r) { Get-Content $r } else { '(no results)' }
}
