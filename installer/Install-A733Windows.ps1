#requires -Version 5.1
<#
.SYNOPSIS
  Prepare a microSD card that installs Windows 11 ARM64 on an Orange Pi 4 Pro (Allwinner A733).

.DESCRIPTION
  Stage 1 (this script, on a Windows PC):
    1. erases the chosen SD card and writes the A733 boot firmware (boot0, U-Boot, UEFI)
       with a GPT that keeps the boot area safe;
    2. puts a WinPE (from your Windows 11 ARM64 ISO) with the A733 SD driver on the card,
       plus install.wim/esd from the same ISO and the A733 drivers.
  Stage 2 (automatic, on the board): WinPE applies Windows, adds the drivers, writes the
  boot files and reboots into the normal Windows setup screens.

  Nothing from Microsoft is shipped with this project: you provide the ISO.

.EXAMPLE
  .\Install-A733Windows.ps1                       # window
.EXAMPLE
  .\Install-A733Windows.ps1 -NoGui -DiskNumber 2 -IsoPath D:\Win11_ARM64.iso -ImageIndex 6
#>
[CmdletBinding()]
param(
    [int]$DiskNumber = -1,
    [Alias('Source')][string]$IsoPath,   # an .iso file, or a folder / USB drive with the extracted ISO
    [int]$ImageIndex = 0,
    [int]$EspMiB = 1024,
    [switch]$Yes,                # do not ask for confirmation (with -NoGui)
    [switch]$NoGui,
    [switch]$AllowVirtualDisk,   # developer testing with an attached VHD(X)
    [switch]$TestSkipInstallImage, # developer testing: do not copy install.wim/esd
    [string]$ScreenshotPath,     # docs: save a picture of the window and exit
    [string]$LogPath
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Repo = Split-Path -Parent $PSScriptRoot
$Version = '0.1.0'

if ($ScreenshotPath) {
    . (Join-Path $PSScriptRoot 'A733Disk.ps1')
    $AllowVirtualDisk = $false
}

# ------------------------------------------------------------------ elevation
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $ScreenshotPath -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    foreach ($k in $PSBoundParameters.Keys) {
        $v = $PSBoundParameters[$k]
        if ($v -is [switch]) { if ($v) { $argList += "-$k" } } else { $argList += "-$k"; $argList += "`"$v`"" }
    }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    return
}

. (Join-Path $PSScriptRoot 'A733Disk.ps1')

if (-not $LogPath) {
    $logDir = Join-Path $Repo 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $LogPath = Join-Path $logDir ("install-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
}

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }
function Step([string]$Text) { Write-Host ''; Write-Host "=== $(Get-Date -Format HH:mm:ss) $Text" -ForegroundColor Cyan }

# ------------------------------------------------------------------ payload
$Payload = [ordered]@{
    Firmware   = Join-Path $Repo 'firmware\opi4pro-a733-uefi-v60.img.gz'
    SdDriver   = Join-Path $Repo 'drivers\sd\A733SdMmc.inf'
    EthDriver  = Join-Path $Repo 'drivers\ethernet\A733Gmac.inf'
    InstallCmd = Join-Path $PSScriptRoot 'winpe\install.cmd'
    WinPeShl   = Join-Path $PSScriptRoot 'winpe\winpeshl.ini'
}

function Get-Firmware {
    $in = [IO.File]::OpenRead($Payload.Firmware)
    try {
        $gz = New-Object IO.Compression.GZipStream($in, [IO.Compression.CompressionMode]::Decompress)
        $ms = New-Object IO.MemoryStream
        $gz.CopyTo($ms)
        $gz.Dispose()
        $fw = $ms.ToArray()
    } finally { $in.Dispose() }
    Test-A733Firmware $fw
    , $fw
}

# ------------------------------------------------------------------ disks / ISO
function Get-CandidateDisks {
    $bus = @('USB', 'SD', 'MMC')
    if ($AllowVirtualDisk) { $bus += 'File Backed Virtual' }
    @(Get-Disk -ErrorAction SilentlyContinue | Where-Object {
        ($bus -contains [string]$_.BusType) -and -not $_.IsBoot -and -not $_.IsSystem -and
        $_.Size -ge 15GB -and $_.Size -le 1TB
    } | Sort-Object Number)
}

function Format-Disk($d) { '{0}: {1} - {2:N1} GB ({3})' -f $d.Number, $d.FriendlyName, ($d.Size / 1e9), $d.BusType }

function Mount-Iso([string]$Path) {
    if (Test-Path $Path -PathType Container) { return $Path.TrimEnd('\') }   # extracted ISO / USB installer
    $img = Get-DiskImage -ImagePath $Path
    if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $Path -PassThru }
    for ($i = 0; $i -lt 20; $i++) {
        $letter = ($img | Get-Volume -ErrorAction SilentlyContinue).DriveLetter
        if ($letter) { return "$($letter):" }
        Start-Sleep -Milliseconds 500
    }
    throw "ISO mounted but has no drive letter: $Path"
}

function Dismount-Iso([string]$Path) {
    if ($Path -and (Test-Path $Path -PathType Leaf)) { Dismount-DiskImage -ImagePath $Path -ErrorAction SilentlyContinue | Out-Null }
}

function Find-InstallImage([string]$IsoRoot) {
    foreach ($n in 'install.wim', 'install.esd') {
        $p = Join-Path $IsoRoot "sources\$n"
        if (Test-Path $p) { return $p }
    }
    if (Test-Path (Join-Path $IsoRoot 'sources\install.swm')) { throw 'split install.swm images are not supported: use the official Microsoft ISO' }
    throw "no sources\install.wim or install.esd in $IsoRoot"
}

function Get-Editions([string]$InstallImage) {
    @(Get-WindowsImage -ImagePath $InstallImage | ForEach-Object {
        $info = Get-WindowsImage -ImagePath $InstallImage -Index $_.ImageIndex
        [pscustomobject]@{ Index = $_.ImageIndex; Name = $_.ImageName; Arch = [int]$info.Architecture }
    })
}

# ------------------------------------------------------------------ GUI
function Show-Gui {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    $f = New-Object Windows.Forms.Form
    $f.Text = "Orange Pi 4 Pro - Windows SD installer $Version"
    $f.Size = New-Object Drawing.Size(620, 430)
    $f.StartPosition = 'CenterScreen'
    $f.FormBorderStyle = 'FixedDialog'
    $f.MaximizeBox = $false
    $font = New-Object Drawing.Font('Segoe UI', 9)
    $f.Font = $font
    $y = 12
    function Add-Label($text, $yy, $h = 20, $bold = $false) {
        $l = New-Object Windows.Forms.Label
        $l.Text = $text; $l.Location = New-Object Drawing.Point(14, $yy); $l.Size = New-Object Drawing.Size(580, $h)
        if ($bold) { $l.Font = New-Object Drawing.Font('Segoe UI', 9, [Drawing.FontStyle]::Bold) }
        $f.Controls.Add($l); $l
    }
    $warn = Add-Label ("EXPERIMENTAL. Everything on the selected card will be ERASED.`r`n" +
                       "ЭКСПЕРИМЕНТАЛЬНО. Всё на выбранной карте будет СТЁРТО.") $y 36 $true
    $warn.ForeColor = [Drawing.Color]::DarkRed
    $y += 46
    Add-Label '1. SD card / SD-карта:' $y | Out-Null
    $y += 22
    $disks = New-Object Windows.Forms.ComboBox
    $disks.DropDownStyle = 'DropDownList'; $disks.Location = New-Object Drawing.Point(14, $y); $disks.Size = New-Object Drawing.Size(470, 24)
    $f.Controls.Add($disks)
    $refresh = New-Object Windows.Forms.Button
    $refresh.Text = 'Refresh'; $refresh.Location = New-Object Drawing.Point(494, ($y - 1)); $refresh.Size = New-Object Drawing.Size(96, 26)
    $f.Controls.Add($refresh)
    $y += 38
    Add-Label '2. Windows 11 ARM64: ISO file, or a folder / USB drive with the extracted ISO:' $y | Out-Null
    $y += 22
    $iso = New-Object Windows.Forms.TextBox
    $iso.Location = New-Object Drawing.Point(14, $y); $iso.Size = New-Object Drawing.Size(370, 24); $iso.ReadOnly = $true
    $f.Controls.Add($iso)
    $browse = New-Object Windows.Forms.Button
    $browse.Text = 'ISO...'; $browse.Location = New-Object Drawing.Point(394, ($y - 1)); $browse.Size = New-Object Drawing.Size(96, 26)
    $f.Controls.Add($browse)
    $folder = New-Object Windows.Forms.Button
    $folder.Text = 'Folder...'; $folder.Location = New-Object Drawing.Point(494, ($y - 1)); $folder.Size = New-Object Drawing.Size(96, 26)
    $f.Controls.Add($folder)
    $y += 38
    Add-Label '3. Edition / Редакция:' $y | Out-Null
    $y += 22
    $eds = New-Object Windows.Forms.ComboBox
    $eds.DropDownStyle = 'DropDownList'; $eds.Location = New-Object Drawing.Point(14, $y); $eds.Size = New-Object Drawing.Size(576, 24)
    $f.Controls.Add($eds)
    $y += 40
    $ok = New-Object Windows.Forms.CheckBox
    $ok.Text = 'I understand the card will be erased / Я понимаю, что карта будет стёрта'
    $ok.Location = New-Object Drawing.Point(14, $y); $ok.Size = New-Object Drawing.Size(576, 24)
    $f.Controls.Add($ok)
    $y += 34
    $status = Add-Label '' $y 40
    $y += 46
    $start = New-Object Windows.Forms.Button
    $start.Text = 'Write card / Записать карту'; $start.Location = New-Object Drawing.Point(380, $y); $start.Size = New-Object Drawing.Size(210, 32)
    $start.Enabled = $false
    $f.Controls.Add($start)

    $state = @{ Disks = @(); Editions = @(); Result = $null }
    $update = { $start.Enabled = ($disks.SelectedIndex -ge 0 -and $eds.SelectedIndex -ge 0 -and $ok.Checked) }
    $fill = {
        $disks.Items.Clear()
        $state.Disks = @(Get-CandidateDisks)
        foreach ($d in $state.Disks) { [void]$disks.Items.Add((Format-Disk $d)) }
        $status.Text = if ($state.Disks.Count) { '' } else { 'No SD card found (15 GB .. 1 TB, card reader / USB). Insert the card and press Refresh.' }
        & $update
    }
    $refresh.Add_Click($fill)
    $disks.Add_SelectedIndexChanged($update)
    $eds.Add_SelectedIndexChanged($update)
    $ok.Add_CheckedChanged($update)
    $load = {
        param([string]$path)
        $iso.Text = $path
        $eds.Items.Clear(); $state.Editions = @()
        $status.Text = 'Reading the Windows image...'; $f.Refresh()
        try {
            $root = Mount-Iso $path
            try {
                $state.Editions = @(Get-Editions (Find-InstallImage $root) | Where-Object { $_.Arch -eq 12 })
            } finally { Dismount-Iso $path }
            if (-not $state.Editions.Count) { throw 'no ARM64 image here: download "Windows 11 (Arm64)" from microsoft.com' }
            $pick = 0
            for ($i = 0; $i -lt $state.Editions.Count; $i++) {
                [void]$eds.Items.Add("$($state.Editions[$i].Index): $($state.Editions[$i].Name)")
                if ($state.Editions[$i].Name -match 'Pro$') { $pick = $i }
            }
            $eds.SelectedIndex = $pick
            $status.Text = ''
        } catch { $status.Text = "Windows image: $($_.Exception.Message)" }
        & $update
    }
    $browse.Add_Click({
        $dlg = New-Object Windows.Forms.OpenFileDialog
        $dlg.Filter = 'ISO image (*.iso)|*.iso'
        if ($dlg.ShowDialog() -eq 'OK') { & $load $dlg.FileName }
    })
    $folder.Add_Click({
        $dlg = New-Object Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Folder or USB drive with the extracted Windows 11 ARM64 ISO (has sources\install.wim)'
        if ($dlg.ShowDialog() -eq 'OK') { & $load $dlg.SelectedPath }
    })
    $start.Add_Click({
        $d = $state.Disks[$disks.SelectedIndex]
        $msg = "ERASE this card?`r`n`r`n$(Format-Disk $d)`r`n`r`nСТЕРЕТЬ эту карту?"
        if ([Windows.Forms.MessageBox]::Show($msg, 'Confirm', 'YesNo', 'Warning') -eq 'Yes') {
            $state.Result = @{ Disk = $d.Number; Iso = $iso.Text; Index = $state.Editions[$eds.SelectedIndex].Index }
            $f.Close()
        }
    })
    $f.Add_Shown({
        & $fill
        if ($ScreenshotPath) {
            # copy from the screen so the Windows 11 frame is captured as users see it
            $f.TopMost = $true
            $f.Activate()
            $f.Refresh()
            [Windows.Forms.Application]::DoEvents()
            Start-Sleep -Milliseconds 700
            $b = $f.Bounds
            $bmp = New-Object Drawing.Bitmap($b.Width, $b.Height)
            $g = [Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
            $g.Dispose()
            $bmp.Save($ScreenshotPath, [Drawing.Imaging.ImageFormat]::Png)
            $bmp.Dispose()
            $f.Close()
        }
    })
    [void]$f.ShowDialog()
    $state.Result
}

# ------------------------------------------------------------------ steps
function Get-FreeLetters([int]$Count) {
    $used = @([IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpper() })
    $free = @(@('S', 'W', 'R', 'T', 'U', 'V', 'Q', 'P', 'O', 'N', 'M', 'L', 'K') | Where-Object { $used -notcontains $_ })
    if ($free.Count -lt $Count) { throw 'not enough free drive letters' }
    $free[0..($Count - 1)]
}

function Invoke-RawWrite([int]$Number, [byte[]]$Firmware) {
    $disk = Get-Disk -Number $Number
    $locks = @()
    try {
        foreach ($p in @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue)) {
            foreach ($ap in @($p.AccessPaths | Where-Object { $_ -like '\\?\Volume*' })) {
                $locks += [A733.RawDisk]::LockVolume($ap.TrimEnd('\'))
            }
        }
        $h = [A733.RawDisk]::Open("\\.\PhysicalDrive$Number")
        try {
            $len = [A733.RawDisk]::Length($h)
            if ($len -ne $disk.Size) { throw "disk $Number size changed ($len vs $($disk.Size))" }
            $fs = New-Object IO.FileStream($h, [IO.FileAccess]::ReadWrite, 1MB)
            $layout = Write-A733Card $fs ($len / 512) $Firmware $EspMiB
            Test-A733BootArea $fs $Firmware
            $fs.Flush()
            [A733.RawDisk]::UpdateProperties($h)
            $fs.Dispose()
        } finally { $h.Dispose() }
    } finally { foreach ($l in $locks) { $l.Dispose() } }
    $layout
}

function Wait-Partitions([int]$Number) {
    for ($i = 0; $i -lt 30; $i++) {
        Update-Disk -Number $Number -ErrorAction SilentlyContinue
        $parts = @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue)
        if ($parts.Count -ge 4) { return $parts }
        Start-Sleep 1
    }
    throw 'Windows does not see the 4 new partitions: unplug and re-insert the card reader, then run the installer again'
}

function Get-VolumePath($Partition) {
    for ($i = 0; $i -lt 20; $i++) {
        $p = Get-Partition -DiskNumber $Partition.DiskNumber -PartitionNumber $Partition.PartitionNumber
        $v = @($p.AccessPaths | Where-Object { $_ -like '\\?\Volume*' })
        if ($v.Count) { return $v[0] }
        Start-Sleep 1
    }
    throw "partition $($Partition.PartitionNumber) has no volume"
}

function Format-CardVolume($Partition, [string]$Letter, [string]$Fs, [string]$Label) {
    $vol = Get-VolumePath $Partition
    foreach ($l in @($Partition.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' })) { mountvol $l /D | Out-Null }
    mountvol "$($Letter):" $vol
    if ($LASTEXITCODE -ne 0) { throw "cannot mount $vol as $($Letter):" }
    # format.com, not diskpart / Format-Volume: VDS refuses some removable card readers
    $out = 'Y' | format.com "$($Letter):" /FS:$Fs /Q /V:$Label /Y 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "format $($Letter): failed: $out" }
    if (-not (Test-Path "$($Letter):\")) { throw "$($Letter): not accessible after format" }
}

function Copy-WithProgress([string]$Source, [string]$DestDir, [switch]$Quiet) {
    $name = Split-Path $Source -Leaf
    if ($Quiet) {
        robocopy (Split-Path $Source) $DestDir $name /J /NJH /NJS /NDL /NP /R:2 /W:2 | Out-Null
    } else {
        # robocopy prints the percentage on the console only (not into the log)
        robocopy (Split-Path $Source) $DestDir $name /J /NJH /NJS /NDL /R:2 /W:2 | Out-Host
    }
    if ($LASTEXITCODE -ge 8) { throw "copy of $name failed (robocopy $LASTEXITCODE)" }
    attrib -r (Join-Path $DestDir $name)
}

function New-InstallerBootWim([string]$IsoRoot, [string]$Work) {
    $src = Join-Path $IsoRoot 'sources\boot.wim'
    $wim = Join-Path $Work 'boot.wim'
    Copy-Item $src $wim -Force
    attrib -r $wim
    $setupIndex = (Get-WindowsImage -ImagePath $wim | Where-Object { $_.ImageIndex -eq 2 })
    if (-not $setupIndex) { throw 'boot.wim has no index 2 (Windows Setup)' }
    if ((Get-WindowsImage -ImagePath $wim -Index 2).Architecture -ne 12) { throw 'boot.wim is not ARM64' }
    $mnt = Join-Path $Work 'mount'
    New-Item -ItemType Directory -Force $mnt | Out-Null
    Mount-WindowsImage -ImagePath $wim -Index 2 -Path $mnt | Out-Null
    $saved = $false
    try {
        $a = Join-Path $mnt 'a733'
        New-Item -ItemType Directory -Force "$a\drivers\sd", "$a\drivers\ethernet" | Out-Null
        Copy-Item (Join-Path (Split-Path $Payload.SdDriver) '*') "$a\drivers\sd\" -Force
        Copy-Item (Join-Path (Split-Path $Payload.EthDriver) '*') "$a\drivers\ethernet\" -Force
        Copy-Item $Payload.InstallCmd "$a\install.cmd" -Force
        Copy-Item $Payload.WinPeShl (Join-Path $mnt 'Windows\System32\winpeshl.ini') -Force
        Dismount-WindowsImage -Path $mnt -Save | Out-Null
        $saved = $true
    } finally {
        if (-not $saved) { Dismount-WindowsImage -Path $mnt -Discard -ErrorAction SilentlyContinue | Out-Null }
    }
    $wim
}

function Invoke-Install([int]$Number, [string]$Iso, [int]$Index) {
    Step "Orange Pi 4 Pro Windows SD installer $Version"
    foreach ($k in $Payload.Keys) { if (-not (Test-Path $Payload[$k])) { throw "missing file: $($Payload[$k]) (download the whole repository / release)" } }
    $fw = Get-Firmware
    Say "firmware OK ($($fw.Length / 1MB) MiB: boot0, U-Boot, UEFI)"

    $disk = Get-Disk -Number $Number
    if (-not (@(Get-CandidateDisks) | Where-Object { $_.Number -eq $Number })) {
        throw "disk $Number ($($disk.FriendlyName), $($disk.BusType)) is not a removable card between 15 GB and 1 TB: refusing"
    }
    Say "card: $(Format-Disk $disk)" Yellow

    Step 'Reading the ISO'
    $root = Mount-Iso $Iso
    $work = Join-Path $env:TEMP ("a733-installer-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $work | Out-Null
    $letters = @()
    try {
        $install = Find-InstallImage $root
        $ed = Get-Editions $install | Where-Object { $_.Index -eq $Index }
        if (-not $ed) { throw "image index $Index not found in $install" }
        if ($ed.Arch -ne 12) { throw "index $Index ($($ed.Name)) is not ARM64" }
        Say "edition: $($ed.Name) (index $Index, $([int]((Get-Item $install).Length / 1MB)) MiB)"

        if (-not $NoGui -or $Yes) { } else {
            $answer = Read-Host "Type ERASE to wipe disk $Number ($($disk.FriendlyName))"
            if ($answer -cne 'ERASE') { throw 'cancelled' }
        }

        Step 'Preparing the installer WinPE (boot.wim from the ISO + A733 SD driver)'
        Clear-WindowsCorruptMountPoint -ErrorAction SilentlyContinue | Out-Null
        $bootWim = New-InstallerBootWim $root $work

        Step "Writing the boot firmware and partition table to disk $Number"
        $layout = Invoke-RawWrite $Number $fw
        Say 'boot0 / U-Boot / UEFI written and verified' Green

        Step 'Formatting the card partitions'
        $parts = Wait-Partitions $Number
        $esp = $parts | Where-Object { $_.PartitionNumber -eq 2 }
        $win = $parts | Where-Object { $_.PartitionNumber -eq 4 }
        $letters = Get-FreeLetters 2
        $E = $letters[0]; $W = $letters[1]
        Format-CardVolume $esp $E 'FAT32' 'SYSTEM'
        Format-CardVolume $win $W 'NTFS' 'Windows'
        Say "boot partition $($E):  Windows partition $($W):"

        Step 'Copying the installer boot files'
        foreach ($d in 'efi', 'boot') {
            robocopy (Join-Path $root $d) "$($E):\$d" /E /NJH /NJS /NDL /NFL /R:2 /W:2 | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "copy of $d failed" }
        }
        New-Item -ItemType Directory -Force "$($E):\sources" | Out-Null
        Copy-WithProgress $bootWim "$($E):\sources" -Quiet
        attrib -r -s -h "$($E):\*" /s /d | Out-Null
        $bcd = "$($E):\efi\microsoft\boot\bcd"
        if (-not (Test-Path $bcd)) { throw 'the ISO has no efi\microsoft\boot\bcd' }
        foreach ($a in @(@('{default}', 'testsigning', 'on'), @('{default}', 'nointegritychecks', 'on'))) {
            bcdedit /store $bcd /set @a | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "bcdedit /set $($a -join ' ') failed" }
        }
        Set-Content -Path "$($E):\a733-esp.tag" -Value 'A733 installer boot partition' -Encoding ascii

        Step 'Copying the Windows image (several minutes)'
        New-Item -ItemType Directory -Force "$($W):\a733install" | Out-Null
        if ($TestSkipInstallImage) { Say 'TEST: install image NOT copied' Yellow } else { Copy-WithProgress $install "$($W):\a733install" }
        $imgName = Split-Path $install -Leaf
        Set-Content -Path "$($W):\a733install\config.cmd" -Encoding ascii -Value @("set IMG=$imgName", "set IDX=$Index")
        Set-Content -Path "$($W):\a733install\a733.tag" -Value 'A733 installer Windows partition' -Encoding ascii

        Step 'Final check'
        Write-VolumeCache $E
        Write-VolumeCache $W
        $h = [A733.RawDisk]::Open("\\.\PhysicalDrive$Number")
        try { $fs = New-Object IO.FileStream($h, [IO.FileAccess]::Read, 1MB); Test-A733BootArea $fs $fw; $fs.Dispose() } finally { $h.Dispose() }
        Say 'boot area intact' Green
    } finally {
        foreach ($l in $letters) { if (Test-Path "$($l):\") { mountvol "$($l):" /D | Out-Null } }
        Dismount-Iso $Iso
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host ''
    Say 'DONE. Put the card into the Orange Pi 4 Pro, connect HDMI, keyboard/mouse (white USB port),' Green
    Say 'optionally Ethernet, and power it on. The board installs Windows by itself (20-60 min) and reboots' Green
    Say 'into the Windows setup screens. / ГОТОВО. Вставьте карту в плату и включите её.' Green
}

# ------------------------------------------------------------------ main
if ($ScreenshotPath) { Show-Gui | Out-Null; return }
Start-Transcript -Path $LogPath -Force | Out-Null
try {
    if ($DiskNumber -lt 0 -or -not $IsoPath -or $ImageIndex -le 0) {
        if ($NoGui) { throw 'with -NoGui give -DiskNumber, -IsoPath and -ImageIndex' }
        $sel = Show-Gui
        if (-not $sel) { Say 'cancelled'; return }
        $DiskNumber = $sel.Disk; $IsoPath = $sel.Iso; $ImageIndex = $sel.Index
        $Yes = $true
    }
    Invoke-Install $DiskNumber $IsoPath $ImageIndex
    $code = 0
} catch {
    Write-Host ''
    Say "ERROR: $($_.Exception.Message)" Red
    Say "log: $LogPath" Red
    $code = 1
} finally {
    Stop-Transcript | Out-Null
}
if (-not $NoGui) { Read-Host 'Press Enter to close' | Out-Null }
exit $code
