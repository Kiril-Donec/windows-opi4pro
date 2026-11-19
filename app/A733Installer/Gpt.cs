using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace A733Installer
{
    /// <summary>
    /// The A733 card layout (same as installer/A733Disk.ps1 and a733_sd_windows_layout.py):
    ///   sector 0 protective MBR, LBA 1 GPT header, LBA 16..495 boot0 (never touched),
    ///   LBA 1024..1055 GPT entries, LBA 32800..65535 toc1/U-Boot (never touched),
    ///   LBA 65536.. partition 1 = ext4 with the UEFI (from the firmware image).
    ///   FirstUsableLBA = 65536, so no partition can cover the boot area.
    /// </summary>
    static class Gpt
    {
        public const int Sector = 512;
        public const long P1Start = 65536, P1Sectors = 65536;
        const long EntryLba = 1024;
        const int Entries = 128, EntrySize = 128, EntrySectors = 32;
        const long MiB = 2048;
        static readonly Guid Linux = new Guid("0FC63DAF-8483-4772-8E79-3D69D8477DE4");
        static readonly Guid Esp = new Guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
        static readonly Guid Msr = new Guid("E3C9E316-0B5C-4DB8-817D-F92DF00215AE");
        static readonly Guid Basic = new Guid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");

        public class Part { public string Name; public Guid Type; public long First, Last; public ulong Attrs; }

        static readonly uint[] crcTable = BuildCrcTable();
        static uint[] BuildCrcTable()
        {
            var t = new uint[256];
            for (uint n = 0; n < 256; n++)
            {
                uint c = n;
                for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[n] = c;
            }
            return t;
        }

        public static uint Crc32(byte[] data, int offset, int count)
        {
            uint crc = 0xFFFFFFFFu;
            for (int i = offset; i < offset + count; i++) crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        public static Part[] Layout(long sectors, int espMiB, out long lastUsable)
        {
            lastUsable = sectors - 1 - EntrySectors - 1;
            long espFirst = P1Start + P1Sectors;
            long espLast = espFirst + espMiB * MiB - 1;
            long msrFirst = espLast + 1, msrLast = msrFirst + 16 * MiB - 1;
            long winFirst = msrLast + 1;
            long winLast = (lastUsable + 1) / MiB * MiB - 1;
            if (winLast - winFirst + 1 < 16L * 1024 * MiB)
                throw new InvalidOperationException($"card too small: {(winLast - winFirst + 1) / MiB} MiB left for Windows (need 16 GiB, 32 GiB+ recommended)");
            return new[]
            {
                new Part { Name = "A733 UEFI boot", Type = Linux, First = P1Start, Last = P1Start + P1Sectors - 1, Attrs = 5 },
                new Part { Name = "EFI system partition", Type = Esp, First = espFirst, Last = espLast },
                new Part { Name = "Microsoft reserved partition", Type = Msr, First = msrFirst, Last = msrLast },
                new Part { Name = "Windows", Type = Basic, First = winFirst, Last = winLast },
            };
        }

        static byte[] BuildEntries(Part[] parts)
        {
            var arr = new byte[Entries * EntrySize];
            for (int i = 0; i < parts.Length; i++)
            {
                int o = i * EntrySize;
                parts[i].Type.ToByteArray().CopyTo(arr, o);
                Guid.NewGuid().ToByteArray().CopyTo(arr, o + 16);
                BitConverter.GetBytes((ulong)parts[i].First).CopyTo(arr, o + 32);
                BitConverter.GetBytes((ulong)parts[i].Last).CopyTo(arr, o + 40);
                BitConverter.GetBytes(parts[i].Attrs).CopyTo(arr, o + 48);
                var name = Encoding.Unicode.GetBytes(parts[i].Name);
                Array.Copy(name, 0, arr, o + 56, Math.Min(name.Length, 72));
            }
            return arr;
        }

        static byte[] BuildHeader(long myLba, long altLba, long lastUsable, byte[] diskGuid, long entryLba, uint entriesCrc)
        {
            var h = new byte[Sector];
            Encoding.ASCII.GetBytes("EFI PART").CopyTo(h, 0);
            BitConverter.GetBytes(0x00010000u).CopyTo(h, 8);
            BitConverter.GetBytes(92u).CopyTo(h, 12);
            BitConverter.GetBytes((ulong)myLba).CopyTo(h, 24);
            BitConverter.GetBytes((ulong)altLba).CopyTo(h, 32);
            BitConverter.GetBytes((ulong)P1Start).CopyTo(h, 40);     // FirstUsableLBA
            BitConverter.GetBytes((ulong)lastUsable).CopyTo(h, 48);
            diskGuid.CopyTo(h, 56);
            BitConverter.GetBytes((ulong)entryLba).CopyTo(h, 72);
            BitConverter.GetBytes((uint)Entries).CopyTo(h, 80);
            BitConverter.GetBytes((uint)EntrySize).CopyTo(h, 84);
            BitConverter.GetBytes(entriesCrc).CopyTo(h, 88);
            BitConverter.GetBytes(Crc32(h, 0, 92)).CopyTo(h, 16);
            return h;
        }

        static byte[] ProtectiveMbr(long sectors, byte[] firmware)
        {
            var m = new byte[Sector];
            Array.Copy(firmware, 0, m, 0, 440);
            m[448] = 2; m[450] = 0xEE; m[451] = 0xFF; m[452] = 0xFF; m[453] = 0xFF;
            BitConverter.GetBytes(1u).CopyTo(m, 454);
            BitConverter.GetBytes((uint)Math.Min(sectors - 1, uint.MaxValue)).CopyTo(m, 458);
            m[510] = 0x55; m[511] = 0xAA;
            return m;
        }

        static void Write(Stream s, long lba, byte[] data)
        {
            s.Position = lba * Sector;
            s.Write(data, 0, data.Length);
        }

        public static byte[] Read(Stream s, long lba, int count)
        {
            var buf = new byte[count * Sector];
            s.Position = lba * Sector;
            int got = 0;
            while (got < buf.Length)
            {
                int r = s.Read(buf, got, buf.Length - got);
                if (r <= 0) throw new IOException($"short read at LBA {lba}");
                got += r;
            }
            return buf;
        }

        public static void CheckFirmware(byte[] fw)
        {
            if (fw.Length != 2 * P1Start * Sector) throw new InvalidDataException($"firmware image must be 64 MiB, got {fw.Length} bytes");
            if (Encoding.ASCII.GetString(fw, 16 * Sector + 4, 8) != "eGON.BT0") throw new InvalidDataException("firmware: boot0 not found at sector 16");
            if (Encoding.ASCII.GetString(fw, 32800 * Sector, 13) != "sunxi-package") throw new InvalidDataException("firmware: toc1 not found at sector 32800");
            if (BitConverter.ToUInt16(fw, (int)(P1Start * Sector) + 1024 + 56) != 0xEF53) throw new InvalidDataException("firmware: ext4 UEFI partition not found");
        }

        /// <summary>Write the firmware (sectors 0..131071) and the A733 GPT.</summary>
        public static Part[] WriteCard(Stream s, long sectors, byte[] fw, int espMiB)
        {
            CheckFirmware(fw);
            var parts = Layout(sectors, espMiB, out long lastUsable);
            long last = sectors - 1;
            var zero1M = new byte[1024 * 1024];
            Write(s, sectors - MiB, zero1M);                                   // old backup GPT
            for (int i = 1; i < parts.Length; i++) Write(s, parts[i].First, zero1M);   // old file system signatures
            Write(s, 0, fw);                                                   // MBR, boot0, toc1, UEFI partition
            var diskGuid = Guid.NewGuid().ToByteArray();
            var entries = BuildEntries(parts);
            uint ecrc = Crc32(entries, 0, entries.Length);
            // backup first, then primary entries, header, protective MBR last
            Write(s, last - EntrySectors, entries);
            Write(s, last, BuildHeader(last, 1, lastUsable, diskGuid, last - EntrySectors, ecrc));
            Write(s, EntryLba, entries);
            Write(s, 1, BuildHeader(1, last, lastUsable, diskGuid, EntryLba, ecrc));
            Write(s, 0, ProtectiveMbr(sectors, fw));
            s.Flush();
            return parts;
        }

        /// <summary>boot0, toc1 and the UEFI partition on the card must equal the firmware.</summary>
        public static void VerifyBootArea(Stream s, byte[] fw)
        {
            foreach (var (name, lba, count) in new[] { ("boot0", 16L, 480), ("toc1", 32800L, 32736), ("UEFI partition", 65536L, 65536) })
            {
                var disk = Read(s, lba, count);
                using (var sha = SHA256.Create())
                {
                    var a = Convert.ToBase64String(sha.ComputeHash(disk));
                    var b = Convert.ToBase64String(sha.ComputeHash(fw, (int)(lba * Sector), count * Sector));
                    if (a != b) throw new IOException($"{name} on the card differs from the firmware image");
                }
            }
        }
    }
}
