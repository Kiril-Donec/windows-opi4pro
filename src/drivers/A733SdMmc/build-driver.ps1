# Build on a Windows x64 developer PC using MSVC + matching SDK/WDK.
# This script NEVER installs a test driver or changes the device firmware.
# 2.5.0 (XNU port): CCU/PIO are only read; no SDHF required by default.
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$NoTrace,
    [switch]$HighSpeed,
    [switch]$RequireSdhf,
    [string]$WindowsTargetPlatformVersion = ''
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Visual Studio Installer/vswhere not found; install VS with ARM64 tools and WDK.' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild not found in installed Visual Studio.' }
$msbuild64 = Join-Path (Split-Path -Parent $msbuild) 'amd64\MSBuild.exe'
if (Test-Path -LiteralPath $msbuild64) { $msbuild = $msbuild64 }
$kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
if (!(Test-Path (Join-Path $kit 'Include'))) { throw 'Windows SDK/WDK headers are missing.' }
$evidence = Join-Path $PSScriptRoot '..\WDK-EVIDENCE'
New-Item -Path $evidence -ItemType Directory -Force | Out-Null
$log = Join-Path $evidence ('wdk-msbuild-{0}.log' -f $Configuration)
$binlog = Join-Path $evidence ('wdk-msbuild-{0}.binlog' -f $Configuration)
$traceFlag = if ($NoTrace) { 0 } else { 1 }
$hsFlag = if ($HighSpeed) { 1 } else { 0 }
$sdhfFlag = if ($RequireSdhf) { 1 } else { 0 }
$argsList = @(
    (Join-Path $PSScriptRoot 'A733SdMmc.vcxproj'), '/m', '/t:Rebuild',
    "/p:Configuration=$Configuration", '/p:Platform=ARM64',
    "/p:A733TraceBringup=$traceFlag", "/p:A733EnableHighSpeed=$hsFlag", "/p:A733RequireSdhf=$sdhfFlag",
    '/p:Inf2CatUseLocalTime=true', "/bl:$binlog", '/v:minimal', '/nologo'
)
if ($WindowsTargetPlatformVersion -ne '') {
    $argsList += "/p:WindowsTargetPlatformVersion=$WindowsTargetPlatformVersion"
}
Write-Host ('Building with {0} (trace={1} highspeed={2} requireSdhf={3})' -f $msbuild, $traceFlag, $hsFlag, $sdhfFlag)
& $msbuild @argsList *>&1 | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0) { throw "WDK build failed: $LASTEXITCODE; inspect $log and $binlog" }
$sys = Get-ChildItem -Path $PSScriptRoot -Filter 'A733SdMmc.sys' -File -Recurse |
    Where-Object { $_.FullName -match ('(?i)[\\/]' + $Configuration + '[\\/]') } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$sys) { throw "MSBuild exited successfully but ARM64 SYS was not found; inspect $log" }
Write-Host ("Built ARM64 driver: {0}" -f $sys.FullName) -ForegroundColor Green
