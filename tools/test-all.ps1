# Runs every co-op scenario in turn and compares the machines' dumps; prints a summary.
#   .\tools\test-all.ps1                 (all)
#   .\tools\test-all.ps1 combat,loot     (some)
param([string[]]$Only)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
# scenario -> dump tags to compare between machines
$suite = [ordered]@{
  smoke    = @('start')
  combat   = @('afterclientkill', 'afterhostkill')
  loot     = @()
  travel   = @('arrived')
  deep     = @('f1', 'f2', 'f3')
  handover = @('back')
  death    = @()
  anim     = @()
  apart    = @()
  quest       = @('f1', 'f2', 'f3')
  questkill   = @('after')
  questclient = @()
  shiftstart  = @()
  event       = @()  # dumps are taken mid-fight (the ambush), so not compared
  explore     = @('explored')
  questlate   = @()
  hardcore    = @()
  party4      = @('start', 'arrived')
  skills      = @()
  summon      = @()
}
$summary = @()
foreach ($name in $suite.Keys) {
  if ($Only -and $Only -notcontains $name) { continue }
  "==== $name"
  # client.txt / client1.txt .. clientN.txt: as many clients as scripts
  $clients = [Math]::Max(1, @(Get-ChildItem (Join-Path $root "tests\scenarios\$name") -Filter 'client*.txt').Count)
  $out = & (Join-Path $PSScriptRoot 'test.ps1') $name -Clients $clients -Timeout 500
  $out | ForEach-Object { "  $_" }
  $run = ($out | Where-Object { $_ -like 'run: *' } | Select-Object -First 1) -replace '^run: ', ''
  $fails = @($out | Where-Object { $_ -like 'FAIL*' -or $_ -like 'TIMED OUT*' })
  if ($suite[$name].Count -and $run) {
    $cmp = & python (Join-Path $PSScriptRoot 'compare.py') $run @($suite[$name])
    $cmp | Where-Object { $_ -like 'FAIL*' } | ForEach-Object { "  $_" }
    $fails += @($cmp | Where-Object { $_ -like 'FAIL*' })
  }
  $summary += '{0,-12} {1}' -f $name, $(if ($fails.Count) { "$($fails.Count) FAILED" } else { 'passed' })
}
'==== summary'
$summary
