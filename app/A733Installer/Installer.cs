using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace A733Installer
{
    class Edition
    {
        public int Index;
        public string Name;
        public string Arch;
        public bool IsArm64 => string.Equals(Arch, "ARM64", System.StringComparison.OrdinalIgnoreCase);
        public override string ToString() => Name;
    }

    /// <summary>The Windows source: an .iso (mounted while in use) or a folder / USB drive.</summary>
    class WindowsSource : IDisposable
    {
        public string Path { get; }
        public string Root { get; private set; }
        bool mounted;

        public WindowsSource(string path)
        {
            Path = path;
            if (Directory.Exists(path)) { Root = path.TrimEnd('\\'); return; }
            if (!File.Exists(path)) throw new FileNotFoundException(path);
            var q = Proc.Quote(path);
            var o = Proc.PowerShell($"$i = Get-DiskImage -ImagePath {q}; if (-not $i.Attached) {{ $i = Mount-DiskImage -ImagePath {q} -PassThru }}; " +
                                    "for ($n = 0; $n -lt 20; $n++) { $l = ($i | Get-Volume).DriveLetter; if ($l) { 'LETTER=' + $l; break }; Start-Sleep -Milliseconds 500 }",
                                    out _);
            var m = Regex.Match(o, @"LETTER=([A-Za-z])");
            if (!m.Success) throw new IOException("cannot mount the ISO: " + o.Trim());
            Root = m.Groups[1].Value + ":";
            mounted = true;
        }

        public string InstallImage
        {
            get
            {
                foreach (var n in new[] { "install.wim", "install.esd" })
                {
                    var p = System.IO.Path.Combine(Root, "sources", n);
                    if (File.Exists(p)) return p;
                }
                if (File.Exists(System.IO.Path.Combine(Root, @"sources\install.swm")))
                    throw new IOException("split install.swm images are not supported: use the official Microsoft ISO");
                throw new IOException(@"no sources\install.wim or install.esd here");
            }
        }

        public static List<Edition> Editions(string image)
        {
            Proc.Run(Proc.Sys("dism.exe"), $"/English /Get-WimInfo /WimFile:\"{image}\"", null, out var o);
            var list = new List<Edition>();
            foreach (Match m in Regex.Matches(o, @"Index : (\d+)\s*\r?\nName : ([^\r\n]+)"))
                list.Add(new Edition { Index = int.Parse(m.Groups[1].Value), Name = m.Groups[2].Value.Trim() });
            if (list.Count == 0) throw new IOException("cannot read the image list: " + o.Trim());
            foreach (var e in list)
            {
                Proc.Run(Proc.Sys("dism.exe"), $"/English /Get-WimInfo /WimFile:\"{image}\" /Index:{e.Index}", null, out var d);
                var a = Regex.Match(d, @"Architecture : (\S+)");
                e.Arch = a.Success ? a.Groups[1].Value : "?";
            }
            return list;
        }

        public void Dispose()
        {
            if (!mounted) return;
            Proc.PowerShell($"Dismount-DiskImage -ImagePath {Proc.Quote(Path)} | Out-Null", out _);
            mounted = false;
        }
    }

    class InstallOptions
    {
        public uint Disk;
        public string Source;
        public int Index;
        public int EspMiB = 1024;
        public bool AllowVirtual;
        public bool SkipInstallImage;   // developer test
    }

    /// <summary>Stage 1 on the PC: the same steps as installer/Install-A733Windows.ps1.</summary>
    class Installer
    {
        public const int StepCount = 7;
        public Action<int> Step = _ => { };          // 0..6
        public Action<string> Log = _ => { };
        public Action<double> Percent = _ => { };    // copy of the Windows image

        void Say(string s) => Log(s);

        public void Run(InstallOptions o, CancellationToken cancel)
        {
            Say($"Orange Pi 4 Pro Windows SD installer {App.Version}");
            var fw = Payload.Firmware();
            Gpt.CheckFirmware(fw);
            Say("firmware OK (64 MiB: boot0, U-Boot, UEFI)");
            var disk = Disks.Get(o.Disk, o.AllowVirtual)
                       ?? throw new InvalidOperationException($"disk {o.Disk} is not a removable card between 15 GB and 1 TB: refusing");
            Say("card: " + disk);

            Step(0);
            var work = Path.Combine(Path.GetTempPath(), "a733-installer-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(work);
            var letters = new List<char>();
            using (var src = new WindowsSource(o.Source))
            {
                try
                {
                    var install = src.InstallImage;
                    var ed = WindowsSource.Editions(install).FirstOrDefault(e => e.Index == o.Index)
                             ?? throw new InvalidOperationException($"image index {o.Index} not found");
                    if (!ed.IsArm64) throw new InvalidOperationException($"{ed.Name} is {ed.Arch}, not ARM64");
                    Say($"edition: {ed.Name} (index {o.Index}, {new FileInfo(install).Length >> 20} MiB)");
                    cancel.ThrowIfCancellationRequested();

                    Step(1);
                    var bootWim = PrepareBootWim(src.Root, work);
                    cancel.ThrowIfCancellationRequested();

                    Step(2);
                    WriteFirmware(o, fw);
                    Say("boot0 / U-Boot / UEFI written and verified");

                    Step(3);
                    var parts = WaitPartitions(o.Disk);
                    var free = FreeLetters(2);
                    char E = free[0], W = free[1];
                    FormatVolume(parts.First(p => p.PartitionNumber == 2), E, "FAT32", "SYSTEM"); letters.Add(E);
                    FormatVolume(parts.First(p => p.PartitionNumber == 4), W, "NTFS", "Windows"); letters.Add(W);
                    Say($"boot partition {E}:  Windows partition {W}:");

                    Step(4);
                    foreach (var d in new[] { "efi", "boot" }) CopyDir(Path.Combine(src.Root, d), $@"{E}:\{d}");
                    Directory.CreateDirectory($@"{E}:\sources");
                    File.Copy(bootWim, $@"{E}:\sources\boot.wim", true);
                    foreach (var f in Directory.GetFiles($@"{E}:\", "*", SearchOption.AllDirectories)) File.SetAttributes(f, FileAttributes.Normal);
                    var bcd = $@"{E}:\efi\microsoft\boot\bcd";
                    if (!File.Exists(bcd)) throw new IOException(@"the Windows image has no efi\microsoft\boot\bcd");
                    foreach (var a in new[] { "testsigning on", "nointegritychecks on" })
                        if (Proc.Run(Proc.Sys("bcdedit.exe"), $"/store \"{bcd}\" /set {{default}} {a}", Say) != 0)
                            throw new IOException("bcdedit /set " + a + " failed");
                    File.WriteAllText($@"{E}:\a733-esp.tag", "A733 installer boot partition");

                    Step(5);
                    Directory.CreateDirectory($@"{W}:\a733install");
                    if (o.SkipInstallImage) Say("TEST: install image NOT copied");
                    else CopyWithProgress(install, $@"{W}:\a733install\{Path.GetFileName(install)}", cancel);
                    File.WriteAllText($@"{W}:\a733install\config.cmd", $"set IMG={Path.GetFileName(install)}\r\nset IDX={o.Index}\r\n");
                    File.WriteAllText($@"{W}:\a733install\a733.tag", "A733 installer Windows partition");

                    Step(6);
                    RawDisk.FlushVolume(E);
                    RawDisk.FlushVolume(W);
                    using (var h = RawDisk.Open($@"\\.\PhysicalDrive{o.Disk}", false))
                    using (var fs = new FileStream(h, FileAccess.Read, 1 << 20))
                        Gpt.VerifyBootArea(fs, fw);
                    Say("boot area intact");
                }
                finally
                {
                    foreach (var l in letters) Proc.Run(Proc.Sys("mountvol.exe"), $"{l}: /D", null);
                    try { Directory.Delete(work, true); } catch { }
                }
            }
            Say("DONE.");
        }

        string PrepareBootWim(string root, string work)
        {
            var wim = Path.Combine(work, "boot.wim");
            File.Copy(Path.Combine(root, @"sources\boot.wim"), wim, true);
            File.SetAttributes(wim, FileAttributes.Normal);
            Proc.Run(Proc.Sys("dism.exe"), $"/English /Get-WimInfo /WimFile:\"{wim}\" /Index:2", null, out var info);
            if (!Regex.IsMatch(info, @"Architecture : ARM64", RegexOptions.IgnoreCase)) throw new IOException("boot.wim index 2 (Windows Setup) is missing or not ARM64");
            Proc.Run(Proc.Sys("dism.exe"), "/English /Cleanup-Wim", null);
            var mnt = Path.Combine(work, "mount");
            Directory.CreateDirectory(mnt);
            if (Proc.Run(Proc.Sys("dism.exe"), $"/English /Mount-Wim /WimFile:\"{wim}\" /Index:2 /MountDir:\"{mnt}\"", Say) != 0)
                throw new IOException("cannot mount boot.wim");
            bool saved = false;
            try
            {
                var a = Path.Combine(mnt, "a733");
                Payload.ExtractDir("drivers/sd", Path.Combine(a, @"drivers\sd"));
                Payload.ExtractDir("drivers/ethernet", Path.Combine(a, @"drivers\ethernet"));
                Payload.ExtractFile("winpe/install.cmd", Path.Combine(a, "install.cmd"));
                Payload.ExtractFile("winpe/winpeshl.ini", Path.Combine(mnt, @"Windows\System32\winpeshl.ini"));
                if (Proc.Run(Proc.Sys("dism.exe"), $"/English /Unmount-Wim /MountDir:\"{mnt}\" /Commit", Say) != 0)
                    throw new IOException("cannot save boot.wim");
                saved = true;
            }
            finally
            {
                if (!saved) Proc.Run(Proc.Sys("dism.exe"), $"/English /Unmount-Wim /MountDir:\"{mnt}\" /Discard", null);
            }
            return wim;
        }

        void WriteFirmware(InstallOptions o, byte[] fw)
        {
            var disk = Disks.Get(o.Disk, o.AllowVirtual) ?? throw new InvalidOperationException("the card disappeared");
            var locks = new List<SafeFileHandle>();
            try
            {
                foreach (var p in Disks.Partitions(o.Disk))
                    if (p.VolumePath != null) locks.Add(RawDisk.LockVolume(p.VolumePath));
                using (var h = RawDisk.Open($@"\\.\PhysicalDrive{o.Disk}"))
                {
                    long len = RawDisk.Length(h);
                    if ((ulong)len != disk.Size) throw new IOException($"disk {o.Disk} size changed");
                    using (var fs = new FileStream(h, FileAccess.ReadWrite, 1 << 20))
                    {
                        Gpt.WriteCard(fs, len / Gpt.Sector, fw, o.EspMiB);
                        Gpt.VerifyBootArea(fs, fw);
                        fs.Flush();
                        RawDisk.UpdateProperties(h);
                    }
                }
            }
            finally { foreach (var l in locks) l.Dispose(); }
        }

        List<PartitionInfo> WaitPartitions(uint disk)
        {
            for (int i = 0; i < 30; i++)
            {
                Disks.Update(disk);
                var p = Disks.Partitions(disk);
                if (p.Count >= 4 && p.First(x => x.PartitionNumber == 2).VolumePath != null && p.First(x => x.PartitionNumber == 4).VolumePath != null)
                    return p;
                Thread.Sleep(1000);
            }
            throw new IOException("Windows does not see the new partitions: unplug and re-insert the card reader, then try again");
        }

        static List<char> FreeLetters(int count)
        {
            var used = DriveInfo.GetDrives().Select(d => char.ToUpper(d.Name[0])).ToList();
            var free = "SWRTUVQPONMLK".Where(c => !used.Contains(c)).Take(count).ToList();
            if (free.Count < count) throw new IOException("not enough free drive letters");
            return free;
        }

        void FormatVolume(PartitionInfo p, char letter, string fs, string label)
        {
            foreach (var l in p.Letters) Proc.Run(Proc.Sys("mountvol.exe"), $"{l.Substring(0, 2)} /D", null);
            if (Proc.Run(Proc.Sys("mountvol.exe"), $"{letter}: {p.VolumePath}", Say) != 0)
                throw new IOException($"cannot mount partition {p.PartitionNumber} as {letter}:");
            // format.com, not diskpart / VDS: VDS refuses some removable card readers
            if (Proc.Run(Proc.Sys("format.com"), $"{letter}: /FS:{fs} /Q /V:{label} /Y", null, out var o, "Y\r\n") != 0)
                throw new IOException($"format {letter}: failed: {o.Trim()}");
            for (int i = 0; i < 10 && !Directory.Exists($@"{letter}:\"); i++) Thread.Sleep(500);
            if (!Directory.Exists($@"{letter}:\")) throw new IOException($"{letter}: not accessible after format");
        }

        static void CopyDir(string src, string dst)
        {
            if (!Directory.Exists(src)) throw new IOException("missing folder in the Windows image: " + src);
            Directory.CreateDirectory(dst);
            foreach (var f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
            foreach (var d in Directory.GetDirectories(src)) CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
        }

        void CopyWithProgress(string src, string dst, CancellationToken cancel)
        {
            var buf = new byte[4 << 20];
            using (var i = new FileStream(src, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
            using (var o = new FileStream(dst, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.WriteThrough))
            {
                long total = i.Length, done = 0;
                int r, lastPct = -1;
                while ((r = i.Read(buf, 0, buf.Length)) > 0)
                {
                    cancel.ThrowIfCancellationRequested();
                    o.Write(buf, 0, r);
                    done += r;
                    int pct = (int)(done * 1000 / Math.Max(1, total));
                    if (pct != lastPct) { lastPct = pct; Percent(pct / 10.0); }
                }
            }
            Say($"copied {Path.GetFileName(src)} ({new FileInfo(dst).Length >> 20} MiB)");
        }
    }
}
