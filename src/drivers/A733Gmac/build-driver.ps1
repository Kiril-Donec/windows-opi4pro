param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 not found.' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild not found.' }
& $msbuild "$PSScriptRoot\A733Gmac.vcxproj" /m /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=ARM64 /p:Inf2CatUseLocalTime=true
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
Write-Host "Built A733Gmac ARM64 $Configuration driver." -ForegroundColor Green
