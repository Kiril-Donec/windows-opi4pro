$ErrorActionPreference = 'Stop'

$package = Join-Path $PSScriptRoot 'package-0.2.3.0'
$build = Join-Path $PSScriptRoot 'ARM64\Release\A733Gmac'
$certificateFile = Join-Path $PSScriptRoot 'A733Gmac-Test.cer'
$subject = 'CN=Orange Pi A733 SD Test'

function Find-KitTool([string]$Name, [string]$Architecture) {
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $result = Get-ChildItem -LiteralPath $kits -Directory |
        Sort-Object Name -Descending |
        ForEach-Object {
            $candidate = Join-Path $_.FullName "$Architecture\$Name"
            if (Test-Path -LiteralPath $candidate) { $candidate }
        } | Select-Object -First 1
    if (!$result) { throw "$Name not found under $kits" }
    return $result
}

$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (!$certificate) { throw "Code-signing certificate not found: $subject" }

New-Item -Path $package -ItemType Directory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'A733Gmac.inf') -Destination $package -Force
Copy-Item -LiteralPath (Join-Path $build 'A733Gmac.sys') -Destination $package -Force
Export-Certificate -Cert $certificate -FilePath $certificateFile -Force | Out-Null
Copy-Item -LiteralPath $certificateFile -Destination $package -Force

$signTool = Find-KitTool 'signtool.exe' 'x64'
$inf2Cat = Find-KitTool 'inf2cat.exe' 'x86'
$sys = Join-Path $package 'A733Gmac.sys'
$cat = Join-Path $package 'A733Gmac.cat'

& $signTool sign /v /fd SHA256 /sha1 $certificate.Thumbprint /s My $sys
if ($LASTEXITCODE -ne 0) { throw 'SYS signing failed.' }
& $inf2Cat "/driver:$package" /os:Server10_ARM64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed.' }
& $signTool sign /v /fd SHA256 /sha1 $certificate.Thumbprint /s My $cat
if ($LASTEXITCODE -ne 0) { throw 'CAT signing failed.' }
& $signTool verify /pa /v $sys
if ($LASTEXITCODE -ne 0) { throw 'SYS signature verification failed.' }
& $signTool verify /pa /v $cat
if ($LASTEXITCODE -ne 0) { throw 'CAT signature verification failed.' }
& $signTool verify /pa /v /c $cat $sys
if ($LASTEXITCODE -ne 0) { throw 'Catalog membership verification failed.' }

$archive = Join-Path $PSScriptRoot 'A733Gmac-0.2.3.0-ARM64-signed.zip'
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $archive -Force
Get-FileHash -Algorithm SHA256 $sys, $cat, $archive | Format-Table Path, Hash -AutoSize
Write-Host "Signed package: $package" -ForegroundColor Green
Write-Host "Archive: $archive" -ForegroundColor Green
