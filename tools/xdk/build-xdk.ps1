<#
.SYNOPSIS
  Build Butterscotch (or the smoke test) for Xbox 360 with an installed XDK.

.DESCRIPTION
  Validates the XDK + Visual Studio 2010 MSBuild integration first and stops
  with a specific message if anything is missing, then runs MSBuild on the
  checked-in .vcxproj (the single source of truth for flags and libraries).
  Nothing from the XDK is copied into the repository.

  XDK location, first match wins: -XdkDir, $env:XEDK (set by the XDK
  installer), then the default 21256.3 install folder.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -CheckOnly
  powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -Target smoke
  powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -Configuration Release -Package -DataWin D:\games\undertale\data.win
#>
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [ValidateSet('butterscotch', 'smoke')] [string] $Target = 'butterscotch',
    [string] $XdkDir = '',
    [string] $MSBuild = '',
    [switch] $CheckOnly,
    [switch] $Package,       # stage dist\<Target>\default.xex for FTP to Aurora
    [string] $DataWin = ''   # optional data.win to stage next to default.xex
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$problems = New-Object System.Collections.Generic.List[string]

function Need([bool] $ok, [string] $what, [string] $hint) {
    if ($ok) { Write-Host "  ok    $what" }
    else { Write-Host "  MISSING $what" -ForegroundColor Red; $problems.Add("$what -- $hint") }
}

# ---- XDK ----------------------------------------------------------------
if (-not $XdkDir) { $XdkDir = $env:XEDK }
if (-not $XdkDir) {
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if (-not $pf86) { $pf86 = $env:ProgramFiles }
    $XdkDir = Join-Path $pf86 'Microsoft Xbox 360 SDK'
}
$XdkDir = $XdkDir.TrimEnd('\')
Write-Host "XDK root: $XdkDir"
Need (Test-Path $XdkDir) 'XDK root folder' 'install XDK 21256.3, or pass -XdkDir / set XEDK'
Need (Test-Path "$XdkDir\bin\win32\cl.exe") 'bin\win32\cl.exe (Xenon compiler)' 'XDK install is incomplete'
Need (Test-Path "$XdkDir\bin\win32\link.exe") 'bin\win32\link.exe' 'XDK install is incomplete'
Need (Test-Path "$XdkDir\bin\win32\imagexex.exe") 'bin\win32\imagexex.exe (XEX packager)' 'XDK install is incomplete'
foreach ($h in 'xtl.h', 'd3d9.h', 'xaudio2.h', 'xgraphics.h') {
    Need (Test-Path "$XdkDir\include\xbox\$h") "include\xbox\$h" 'XDK headers missing'
}
$libs = if ($Configuration -eq 'Debug') {
    'd3d9d', 'd3dx9d', 'xgraphicsd', 'xapilibd', 'xaudiod2', 'xmcored', 'xboxkrnl', 'xbdm'
} else {
    'd3d9', 'd3dx9', 'xgraphics', 'xapilib', 'xaudio2', 'xmcore', 'xboxkrnl'
}
foreach ($l in $libs) {
    Need (Test-Path "$XdkDir\lib\xbox\$l.lib") "lib\xbox\$l.lib" "XDK $Configuration libraries missing"
}

# ---- MSBuild + Visual Studio 2010 Xbox 360 platform ----------------------
if (-not $MSBuild) { $MSBuild = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\MSBuild.exe' }
Need (Test-Path $MSBuild) "MSBuild 4.0 ($MSBuild)" 'install .NET Framework 4 / pass -MSBuild'
Need ([bool]$env:VS100COMNTOOLS) 'Visual Studio 2010 (VS100COMNTOOLS)' 'install VS2010 (with C++) before the XDK'
$pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
if (-not $pf86) { $pf86 = $env:ProgramFiles }
$platformDir = Join-Path $pf86 'MSBuild\Microsoft.Cpp\v4.0\Platforms\Xbox 360'
Need (Test-Path $platformDir) "MSBuild 'Xbox 360' platform ($platformDir)" 're-run the XDK installer after VS2010 and select Visual Studio integration'

if ($problems.Count -gt 0) {
    Write-Host "`nXDK environment is not ready ($($problems.Count) problem(s)):" -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  - $_" }
    exit 2
}
Write-Host "XDK environment looks complete." -ForegroundColor Green
if ($CheckOnly) { exit 0 }

# ---- Build ---------------------------------------------------------------
$project = if ($Target -eq 'smoke') { "$repo\tools\xdk\smoke\XdkSmoke.vcxproj" } else { "$repo\Butterscotch360.sln" }
$outDir = if ($Target -eq 'smoke') { "$repo\build-xbox360-xdk\smoke\$Configuration" } else { "$repo\build-xbox360-xdk\$Configuration" }
$log = "$repo\build-xbox360-xdk\$Target-$Configuration.log"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null

$env:XEDK = $XdkDir  # the XDK platform props resolve include/lib paths from XEDK
Write-Host "`nBuilding $Target ($Configuration|Xbox 360) -> log: $log"
& $MSBuild $project /nologo /m /t:Build "/p:Configuration=$Configuration" "/p:Platform=Xbox 360" `
    "/p:SolutionDir=$repo\" /fl "/flp:logfile=$log;verbosity=normal"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBuild FAILED (exit $LASTEXITCODE). First errors:" -ForegroundColor Red
    Select-String -Path $log -Pattern ': (fatal )?error ' | Select-Object -First 15 | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    exit $LASTEXITCODE
}

$xex = Get-ChildItem -Path $outDir -Filter *.xex -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $xex) {
    Write-Host "Build reported success but no .xex was found in $outDir (check the ImageXex step in $log)." -ForegroundColor Red
    exit 3
}
Write-Host "XEX: $($xex.FullName) ($([math]::Round($xex.Length / 1MB, 2)) MB)" -ForegroundColor Green

if ($Package) {
    $dist = "$repo\dist\$Target"
    New-Item -ItemType Directory -Force $dist | Out-Null
    Copy-Item $xex.FullName "$dist\default.xex" -Force   # Aurora launches default.xex
    if ($DataWin) { Copy-Item $DataWin "$dist\data.win" -Force }
    Write-Host "Staged for FTP: $dist"
}
