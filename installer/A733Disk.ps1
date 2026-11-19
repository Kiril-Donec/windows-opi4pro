# Raw disk helpers for the Orange Pi 4 Pro (A733) Windows installer:
#  - lock / dismount the volumes of a disk, raw sector writes;
#  - the A733 GPT layout (port of a733_sd_windows_layout.py):
#      sector 0          protective MBR
#      LBA 1             GPT header
#      LBA 16..495       boot0 (eGON.BT0)            <- must never be touched
#      LBA 1024..1055    GPT partition entries (gap between boot0 and toc1)
#      LBA 32800..65535  toc1 (U-Boot, BL31, ...)    <- must never be touched
#      LBA 65536..       partition 1: ext4 with the UEFI image (from the firmware)
#      FirstUsableLBA = 65536, so no partition can ever cover the boot area.
#    A standard GPT puts its entries at LBA 2..33, right over boot0: that is
#    why Windows Setup must never repartition this card.

$script:Sector = 512
$script:P1Start = 65536
$script:P1Sectors = 65536
$script:EntryLba = 1024
$script:Entries = 128
$script:EntrySize = 128
$script:EntrySectors = 32
$script:MiB = 2048
$script:GuidLinux = '0FC63DAF-8483-4772-8E79-3D69D8477DE4'
$script:GuidEsp = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
$script:GuidMsr = 'E3C9E316-0B5C-4DB8-817D-F92DF00215AE'
$script:GuidBasic = 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'

if (-not ('A733.RawDisk' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace A733 {
public static class RawDisk {
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
    const uint FSCTL_LOCK_VOLUME = 0x00090018, FSCTL_DISMOUNT_VOLUME = 0x00090020;
    const uint IOCTL_DISK_UPDATE_PROPERTIES = 0x00070140, IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa,
                                             uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize,
                                       out long outBuf, uint outSize, out uint returned, IntPtr ov);

    public static SafeFileHandle Open(string path) {
        SafeFileHandle h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("cannot open " + path + " (error " + Marshal.GetLastWin32Error() + ")");
        return h;
    }
    static bool Ioctl(SafeFileHandle h, uint code, out long value) {
        uint ret;
        return DeviceIoControl(h, code, IntPtr.Zero, 0, out value, 8, out ret, IntPtr.Zero);
    }
    // Lock and dismount a volume; the handle must stay open until the raw writes are done.
    public static SafeFileHandle LockVolume(string volumePath) {
        SafeFileHandle h = Open(volumePath);
        long dummy;
        bool locked = false;
        for (int i = 0; i < 20 && !locked; i++) {
            locked = Ioctl(h, FSCTL_LOCK_VOLUME, out dummy);
            if (!locked) System.Threading.Thread.Sleep(250);
        }
        if (!locked) { h.Dispose(); throw new IOException("cannot lock " + volumePath + ": close Explorer windows / programs using the card"); }
        Ioctl(h, FSCTL_DISMOUNT_VOLUME, out dummy);
        return h;
    }
    public static long Length(SafeFileHandle h) {
        long len;
        if (!Ioctl(h, IOCTL_DISK_GET_LENGTH_INFO, out len)) throw new IOException("IOCTL_DISK_GET_LENGTH_INFO failed");
        return len;
    }
    public static void UpdateProperties(SafeFileHandle h) {
        long dummy;
        Ioctl(h, IOCTL_DISK_UPDATE_PROPERTIES, out dummy);
    }
}
}
'@
}

function ConvertTo-GuidBytes([string]$Guid) { ([Guid]$Guid).ToByteArray() }   # = uuid.bytes_le

$script:Crc32Table = $null
function Get-Crc32([byte[]]$Data) {
    # Hex literals above 0x7FFFFFFF are negative Int32 in PowerShell 5.1: use decimals.
    [uint32]$poly = 3988292384      # 0xEDB88320
    [uint32]$ones = [uint32]::MaxValue
    if ($null -eq $script:Crc32Table) {
        $script:Crc32Table = New-Object 'uint32[]' 256
        for ($n = 0; $n -lt 256; $n++) {
            [uint32]$c = $n
            for ($k = 0; $k -lt 8; $k++) {
                if ($c -band 1) { $c = [uint32]($poly -bxor ($c -shr 1)) } else { $c = [uint32]($c -shr 1) }
            }
            $script:Crc32Table[$n] = $c
        }
    }
    [uint32]$crc = $ones
    foreach ($b in $Data) { $crc = [uint32]($script:Crc32Table[($crc -bxor $b) -band 255] -bxor ($crc -shr 8)) }
    return [uint32]($crc -bxor $ones)
}

function Get-A733Layout([long]$Sectors, [int]$EspMiB) {
    $lastUsable = $Sectors - 1 - $script:EntrySectors - 1
    $espFirst = $script:P1Start + $script:P1Sectors
    $espLast = $espFirst + $EspMiB * $script:MiB - 1
    $msrFirst = $espLast + 1
    $msrLast = $msrFirst + 16 * $script:MiB - 1
    $winFirst = $msrLast + 1
    $winLast = [long]([Math]::Floor(($lastUsable + 1) / $script:MiB)) * $script:MiB - 1
    if (($winLast - $winFirst + 1) -lt 16L * 1024 * $script:MiB) {
        throw "card too small: $([int](($winLast - $winFirst + 1) / $script:MiB)) MiB left for Windows (need 16 GiB, 32 GiB+ recommended)"
    }
    [pscustomobject]@{
        LastUsable = $lastUsable
        Parts = @(
            [pscustomobject]@{ Name = 'A733 UEFI boot'; Type = $script:GuidLinux; First = [long]$script:P1Start; Last = [long]($script:P1Start + $script:P1Sectors - 1); Attrs = [uint64]5 }
            [pscustomobject]@{ Name = 'EFI system partition'; Type = $script:GuidEsp; First = [long]$espFirst; Last = [long]$espLast; Attrs = [uint64]0 }
            [pscustomobject]@{ Name = 'Microsoft reserved partition'; Type = $script:GuidMsr; First = [long]$msrFirst; Last = [long]$msrLast; Attrs = [uint64]0 }
            [pscustomobject]@{ Name = 'Windows'; Type = $script:GuidBasic; First = [long]$winFirst; Last = [long]$winLast; Attrs = [uint64]0 }
        )
    }
}

function New-GptEntries($Parts) {
    $arr = New-Object byte[] ($script:Entries * $script:EntrySize)
    for ($i = 0; $i -lt $Parts.Count; $i++) {
        $p = $Parts[$i]
        $o = $i * $script:EntrySize
        [Array]::Copy((ConvertTo-GuidBytes $p.Type), 0, $arr, $o, 16)
        [Array]::Copy(([Guid]::NewGuid()).ToByteArray(), 0, $arr, $o + 16, 16)
        [Array]::Copy([BitConverter]::GetBytes([uint64]$p.First), 0, $arr, $o + 32, 8)
        [Array]::Copy([BitConverter]::GetBytes([uint64]$p.Last), 0, $arr, $o + 40, 8)
        [Array]::Copy([BitConverter]::GetBytes([uint64]$p.Attrs), 0, $arr, $o + 48, 8)
        $name = [Text.Encoding]::Unicode.GetBytes($p.Name)
        [Array]::Copy($name, 0, $arr, $o + 56, [Math]::Min($name.Length, 72))
    }
    , $arr
}

function New-GptHeader([long]$MyLba, [long]$AltLba, [long]$LastUsable, [byte[]]$DiskGuid, [long]$EntryLba, [uint32]$EntriesCrc) {
    $h = New-Object byte[] $script:Sector
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes('EFI PART'), 0, $h, 0, 8)
    [Array]::Copy([BitConverter]::GetBytes([uint32]0x00010000), 0, $h, 8, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32]92), 0, $h, 12, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint64]$MyLba), 0, $h, 24, 8)
    [Array]::Copy([BitConverter]::GetBytes([uint64]$AltLba), 0, $h, 32, 8)
    [Array]::Copy([BitConverter]::GetBytes([uint64]$script:P1Start), 0, $h, 40, 8)   # FirstUsableLBA
    [Array]::Copy([BitConverter]::GetBytes([uint64]$LastUsable), 0, $h, 48, 8)
    [Array]::Copy($DiskGuid, 0, $h, 56, 16)
    [Array]::Copy([BitConverter]::GetBytes([uint64]$EntryLba), 0, $h, 72, 8)
    [Array]::Copy([BitConverter]::GetBytes([uint32]$script:Entries), 0, $h, 80, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32]$script:EntrySize), 0, $h, 84, 4)
    [Array]::Copy([BitConverter]::GetBytes($EntriesCrc), 0, $h, 88, 4)
    $first92 = New-Object byte[] 92
    [Array]::Copy($h, 0, $first92, 0, 92)
    [Array]::Copy([BitConverter]::GetBytes((Get-Crc32 $first92)), 0, $h, 16, 4)
    , $h
}

function New-ProtectiveMbr([long]$Sectors, [byte[]]$OldMbr) {
    $m = New-Object byte[] $script:Sector
    [Array]::Copy($OldMbr, 0, $m, 0, 440)
    $m[446] = 0; $m[447] = 0; $m[448] = 2; $m[449] = 0
    $m[450] = 0xEE; $m[451] = 0xFF; $m[452] = 0xFF; $m[453] = 0xFF
    [Array]::Copy([BitConverter]::GetBytes([uint32]1), 0, $m, 454, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32][Math]::Min($Sectors - 1, [long][uint32]::MaxValue)), 0, $m, 458, 4)
    $m[510] = 0x55; $m[511] = 0xAA
    , $m
}

function Read-Sectors($Stream, [long]$Lba, [int]$Count) {
    $buf = New-Object byte[] ($Count * $script:Sector)
    $Stream.Position = $Lba * $script:Sector
    $got = 0
    while ($got -lt $buf.Length) {
        $r = $Stream.Read($buf, $got, $buf.Length - $got)
        if ($r -le 0) { throw "short read at LBA $Lba" }
        $got += $r
    }
    , $buf
}

function Write-Sectors($Stream, [long]$Lba, [byte[]]$Data) {
    if ($Data.Length % $script:Sector) { throw 'unaligned write' }
    $Stream.Position = $Lba * $script:Sector
    $Stream.Write($Data, 0, $Data.Length)
}

function Get-Sha256([byte[]]$Data) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($Data)).Replace('-', '') } finally { $sha.Dispose() }
}

# Check that a 64 MiB firmware image has boot0, toc1 and the ext4 UEFI partition.
function Test-A733Firmware([byte[]]$Image) {
    if ($Image.Length -ne 2 * $script:P1Start * $script:Sector) { throw "firmware image must be 64 MiB, got $($Image.Length) bytes" }
    if ([Text.Encoding]::ASCII.GetString($Image, 16 * 512 + 4, 8) -ne 'eGON.BT0') { throw 'firmware: boot0 (eGON.BT0) not found at sector 16' }
    if ([Text.Encoding]::ASCII.GetString($Image, 32800 * 512, 13) -ne 'sunxi-package') { throw 'firmware: toc1 (sunxi-package) not found at sector 32800' }
    if ([BitConverter]::ToUInt16($Image, $script:P1Start * 512 + 1024 + 56) -ne 0xEF53) { throw 'firmware: ext4 UEFI partition not found at sector 65536' }
}

# Write the firmware (sectors 0..131071) and the A733 GPT to a disk stream or
# image file stream of $Sectors sectors. Returns the layout.
function Write-A733Card($Stream, [long]$Sectors, [byte[]]$Firmware, [int]$EspMiB) {
    Test-A733Firmware $Firmware
    $layout = Get-A733Layout $Sectors $EspMiB
    $last = $Sectors - 1
    $zero1M = New-Object byte[] (1MB)

    # old partition tables / file system signatures that could confuse Windows
    Write-Sectors $Stream ($Sectors - $script:MiB) $zero1M
    foreach ($p in $layout.Parts[1..3]) { Write-Sectors $Stream $p.First $zero1M }

    # firmware: MBR, boot0, toc1, partition 1 (ext4 with the UEFI image)
    Write-Sectors $Stream 0 $Firmware

    $diskGuid = ([Guid]::NewGuid()).ToByteArray()
    $entries = New-GptEntries $layout.Parts
    $ecrc = Get-Crc32 $entries
    $primary = New-GptHeader 1 $last $layout.LastUsable $diskGuid $script:EntryLba $ecrc
    $backup = New-GptHeader $last 1 $layout.LastUsable $diskGuid ($last - $script:EntrySectors) $ecrc
    # backup first, then primary entries, header, protective MBR last (same order as the Python tool)
    Write-Sectors $Stream ($last - $script:EntrySectors) $entries
    Write-Sectors $Stream $last $backup
    Write-Sectors $Stream $script:EntryLba $entries
    Write-Sectors $Stream 1 $primary
    $oldMbr = New-Object byte[] 512
    [Array]::Copy($Firmware, 0, $oldMbr, 0, 512)
    Write-Sectors $Stream 0 (New-ProtectiveMbr $Sectors $oldMbr)
    $Stream.Flush()
    $layout
}

# Read back: boot0 / toc1 / partition 1 must be byte-identical to the firmware.
function Test-A733BootArea($Stream, [byte[]]$Firmware) {
    foreach ($r in @(@{ N = 'boot0'; L = 16; C = 480 }, @{ N = 'toc1'; L = 32800; C = 32736 }, @{ N = 'UEFI partition'; L = 65536; C = 65536 })) {
        $disk = Read-Sectors $Stream $r.L $r.C
        $want = New-Object byte[] ($r.C * 512)
        [Array]::Copy($Firmware, $r.L * 512, $want, 0, $want.Length)
        if ((Get-Sha256 $disk) -ne (Get-Sha256 $want)) { throw "$($r.N) on the card differs from the firmware image" }
    }
}
