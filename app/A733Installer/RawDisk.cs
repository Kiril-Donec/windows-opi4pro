using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace A733Installer
{
    /// <summary>Raw access to \\.\PhysicalDriveN and volume locking.</summary>
    static class RawDisk
    {
        const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
        const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
        const uint FSCTL_LOCK_VOLUME = 0x00090018, FSCTL_DISMOUNT_VOLUME = 0x00090020;
        const uint IOCTL_DISK_UPDATE_PROPERTIES = 0x00070140, IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize, out long outBuf, uint outSize, out uint returned, IntPtr ov);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool FlushFileBuffers(SafeFileHandle h);

        public static SafeFileHandle Open(string path, bool write = true)
        {
            var h = CreateFileW(path, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
            if (h.IsInvalid) throw new IOException($"cannot open {path}: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");
            return h;
        }

        static bool Ioctl(SafeFileHandle h, uint code, out long value) =>
            DeviceIoControl(h, code, IntPtr.Zero, 0, out value, 8, out _, IntPtr.Zero);

        /// <summary>Lock + dismount a volume; keep the handle open while writing raw sectors.</summary>
        public static SafeFileHandle LockVolume(string volumePath)
        {
            var h = Open(volumePath.TrimEnd('\\'));
            bool locked = false;
            for (int i = 0; i < 20 && !locked; i++)
            {
                locked = Ioctl(h, FSCTL_LOCK_VOLUME, out _);
                if (!locked) Thread.Sleep(250);
            }
            if (!locked) { h.Dispose(); throw new IOException("cannot lock a volume on the card: close Explorer windows and programs that use it"); }
            Ioctl(h, FSCTL_DISMOUNT_VOLUME, out _);
            return h;
        }

        public static long Length(SafeFileHandle h)
        {
            if (!Ioctl(h, IOCTL_DISK_GET_LENGTH_INFO, out long len)) throw new IOException("IOCTL_DISK_GET_LENGTH_INFO failed");
            return len;
        }

        public static void UpdateProperties(SafeFileHandle h) => Ioctl(h, IOCTL_DISK_UPDATE_PROPERTIES, out _);

        /// <summary>Flush a drive letter's file system cache (like Write-VolumeCache).</summary>
        public static void FlushVolume(char letter)
        {
            using (var h = Open($@"\\.\{letter}:"))
                FlushFileBuffers(h);
        }
    }
}
