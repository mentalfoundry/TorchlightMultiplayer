# Builds libtailcat.dll (32-bit) for TLMP's join codes: Tailscale's Tailcat (BSD-3-Clause,
# github.com/tailscale/tailcat) at a pinned commit, with one patch to a dependency (below).
#   .\tools\tailcat\build.ps1
# Needs Go (winget GoLang.Go) and LLVM-MinGW for i686 (github.com/mstorsjo/llvm-mingw, UCRT build;
# unzip anywhere and pass -MinGW <its bin folder>). Output: build\tailcat\libtailcat.dll and
# LICENSE.tailcat.txt, which package.ps1 and install.ps1 pick up. Only needed when Tailcat changes.
param(
  [string]$Go = 'C:\Program Files\Go\bin\go.exe',
  [string]$MinGW = 'D:\CODE\_tools\llvm-mingw-20261020-ucrt-x86_64\bin'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$commit = 'c14b171db436062b86e95aa09e5792d1d1fd90f1' # tailcat main, 2026-11-20
$out = Join-Path $root 'build\tailcat'
$src = Join-Path $out 'src'

foreach ($tool in $Go, (Join-Path $MinGW 'i686-w64-mingw32-clang.exe')) {
  if (-not (Test-Path $tool)) { throw "Not found: $tool (see the top of this script)" }
}
New-Item -ItemType Directory -Force $out | Out-Null
if (-not (Test-Path (Join-Path $src '.git'))) {
  git clone --quiet https://github.com/tailscale/tailcat.git $src
  if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
}
git -C $src fetch --quiet origin
git -C $src checkout --quiet --force $commit
if ($LASTEXITCODE -ne 0) { throw "git checkout $commit failed" }

$env:PATH = "$MinGW;$(Split-Path $Go);$env:PATH"
Push-Location $src
try {
  # The patch: github.com/dblohm7/wingoes reads the Windows version with GetVersionEx, which reports
  # 6.2 to processes without a Windows 10 manifest -- Torchlight.exe has none -- and then panics
  # ("incoherent Windows version"), taking the game down on the first connection. The patched copy
  # takes the version numbers from RtlGetNtVersionNumbers, which ignores manifests.
  & $Go mod download github.com/dblohm7/wingoes
  $wingoes = (& $Go list -m -f '{{.Dir}}' github.com/dblohm7/wingoes).Trim()
  $patched = Join-Path $out 'wingoes'
  if (Test-Path $patched) { Remove-Item $patched -Recurse -Force }
  Copy-Item $wingoes $patched -Recurse
  Get-ChildItem $patched -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }
  $file = Join-Path $patched 'osversion.go'
  $text = [IO.File]::ReadAllText($file)
  $find = "	if err := getVersionEx(&osv); err != nil {`n		return nil, err`n	}`n	return &osv, nil"
  if (-not $text.Contains($find)) { throw 'wingoes changed: review the patch in tools\tailcat\build.ps1' }
  $text = $text.Replace($find, "	if err := getVersionEx(&osv); err != nil {`n		return nil, err`n	}`n" +
    "	// TLMP: the version numbers proper, whatever the host process's manifest says.`n" +
    "	osv.MajorVersion, osv.MinorVersion, osv.BuildNumber = windows.RtlGetNtVersionNumbers()`n	return &osv, nil")
  [IO.File]::WriteAllText($file, $text)

  # Build against the patched copy without touching Tailcat's own go.mod.
  Copy-Item go.mod go.tlmp.mod -Force
  Copy-Item go.sum go.tlmp.sum -Force
  # (Arguments quoted: Windows PowerShell splits an unquoted -flag=value at its first dot.)
  & $Go mod edit "-replace=github.com/dblohm7/wingoes=$patched" 'go.tlmp.mod'
  if ($LASTEXITCODE -ne 0) { throw 'go mod edit failed' }
  $env:GOOS = 'windows'
  $env:GOARCH = '386'
  $env:CGO_ENABLED = '1'
  $env:CC = 'i686-w64-mingw32-clang'
  $tags = (Get-Content build-tags.txt -Raw).Trim()
  & $Go build '-modfile=go.tlmp.mod' '-tags' $tags '-trimpath' '-ldflags=-s -w' '-buildmode=c-shared' `
    '-o' (Join-Path $out 'libtailcat.dll') './cmd/libtailcat'
  if ($LASTEXITCODE -ne 0) { throw 'go build failed' }
  Copy-Item LICENSE (Join-Path $out 'LICENSE.tailcat.txt') -Force
} finally {
  Pop-Location
}
Write-Host "Built: $out\libtailcat.dll (tailcat $($commit.Substring(0, 9)), wingoes patched)"
