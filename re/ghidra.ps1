# Runs TLExport.java against the analyzed Torchlight project (no re-analysis).
#   .\ghidra.ps1 symbols
#   .\ghidra.ps1 decomp -Addresses 4dcba0,5fe5d0   (bare hex: PowerShell converts 0x literals to decimal)
param([Parameter(Mandatory)][ValidateSet('symbols', 'decomp')][string]$Mode, [string[]]$Addresses)

$env:JAVA_HOME = 'D:\APPS\jdk-21.0.12.1+1'
$env:GHIDRA_HEADLESS_MAXMEM = '6G'
$re = $PSScriptRoot
$scriptArgs = if ($Mode -eq 'symbols') { @('symbols', "$re\functions.txt") } else { @('decomp', "$re\decomp") + $Addresses }

& D:\APPS\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat "$re\ghidra_proj" Torchlight `
  -process Torchlight.exe -noanalysis -readOnly `
  -scriptPath "$re\ghidra_scripts" -postScript TLExport.java @scriptArgs 2>&1 |
  Where-Object { $_ -match 'TLExport|ERROR|decompiled|wrote|no function' }
