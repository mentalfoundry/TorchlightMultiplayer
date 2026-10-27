param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
  [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'

$vs = 'C:\Program Files\Microsoft Visual Studio\18\Professional'
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }

& $cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -G 'Visual Studio 18 2026' -A Win32 `
  "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x86-windows-static
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

& $cmake --build "$PSScriptRoot\build" --config $Config
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

Write-Host "Built: $PSScriptRoot\build\$Config\TLMP.dll"
