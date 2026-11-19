# Test-sign ONLY. Requires Visual Studio, WDK and an existing cert with private key.
# The resulting driver is NOT production signed and may be rejected by Windows
# without user-configured test-signing. Never automatically install it.
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$CertificateSubject = 'CN=Orange Pi A733 SD Test',
    [string]$Inf2CatOs = '10_25H2_ARM64,10_GE_ARM64'
)
$ErrorActionPreference = 'Stop'
# Parse DriverVer instead of pinning a stale version in this script.
$inf = Join-Path $PSScriptRoot 'A733SdMmc.inf'
if (!(Test-Path -LiteralPath $inf)) { throw 'A733SdMmc.inf missing' }
$driverVer = (Select-String -LiteralPath $inf -Pattern '^DriverVer\s*=\s*\d{1,2}/\d{1,2}/\d{4}\s*,\s*(\d+\.\d+\.\d+\.\d+)\s*$' | Select-Object -First 1)
if (!$driverVer) { throw 'DriverVer in INF is missing or malformed' }
$version = $driverVer.Matches[0].Groups[1].Value
$package = Join-Path $PSScriptRoot "package-$version"
$certificateFile = Join-Path $PSScriptRoot 'certificate\A733SdMmc-Test.cer'

function Find-KitTool([string]$Name) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (!(Test-Path $kits)) { throw 'Windows Kits tools not installed.' }
    $result = Get-ChildItem -LiteralPath $kits -Directory |
        Sort-Object Name -Descending | ForEach-Object {
            foreach ($arch in @('x64','x86')) {
                $candidate = Join-Path $_.FullName "$arch\$Name"
                if (Test-Path -LiteralPath $candidate) { return $candidate }
            }
        } | Select-Object -First 1
    if (!$result) { throw "$Name not found under $kits" }
    return $result
}

$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $CertificateSubject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (!$certificate) { throw "Test-signing certificate with private key not found: $CertificateSubject" }

$sysCandidates = Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'A733SdMmc.sys' -Recurse -File |
    Where-Object { $_.FullName -match [regex]::Escape([IO.Path]::DirectorySeparatorChar + $Configuration + [IO.Path]::DirectorySeparatorChar) -and $_.FullName -notmatch 'package-' } |
    Sort-Object LastWriteTime -Descending
$sys = $sysCandidates | Select-Object -First 1
if (!$sys) { throw 'No actual WDK A733SdMmc.sys found. First run build-driver.ps1.' }
# The package version has already been parsed from the INF above.

if (Test-Path $package) { Remove-Item $package -Recurse -Force }
New-Item -Path $package -ItemType Directory -Force | Out-Null
Copy-Item -LiteralPath $inf -Destination $package -Force
Copy-Item -LiteralPath $sys.FullName -Destination $package -Force
New-Item -Path (Split-Path -Parent $certificateFile) -ItemType Directory -Force | Out-Null
Export-Certificate -Cert $certificate -FilePath $certificateFile -Force | Out-Null
Copy-Item -LiteralPath $certificateFile -Destination $package -Force

$signTool = Find-KitTool 'signtool.exe'
$inf2Cat = Find-KitTool 'inf2cat.exe'
$sysPath = Join-Path $package 'A733SdMmc.sys'
$catPath = Join-Path $package 'A733SdMmc.cat'
& $signTool sign /v /fd SHA256 /sha1 $certificate.Thumbprint /s My $sysPath
if ($LASTEXITCODE -ne 0) { throw 'SYS signing failed.' }
& $inf2Cat "/driver:$package" "/os:$Inf2CatOs" /uselocaltime
if ($LASTEXITCODE -ne 0 -or !(Test-Path $catPath)) { throw 'Inf2Cat failed or catalogue absent.' }
& $signTool sign /v /fd SHA256 /sha1 $certificate.Thumbprint /s My $catPath
if ($LASTEXITCODE -ne 0) { throw 'CAT signing failed.' }
& $signTool verify /pa /v $sysPath
if ($LASTEXITCODE -ne 0) { throw 'SYS test-signature verification failed.' }
& $signTool verify /pa /v $catPath
if ($LASTEXITCODE -ne 0) { throw 'CAT test-signature verification failed.' }
& $signTool verify /pa /v /c $catPath $sysPath
if ($LASTEXITCODE -ne 0) { throw 'CAT/SYS membership verification failed.' }
$archive = Join-Path $PSScriptRoot "A733SdMmc-$version-ARM64-test-signed.zip"
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $archive -Force
Get-FileHash -Algorithm SHA256 $sysPath, $catPath, $archive | Format-Table Path, Hash -AutoSize
Write-Host "TEST-SIGNED package only: $package" -ForegroundColor Yellow
Write-Host "Archive: $archive" -ForegroundColor Green
