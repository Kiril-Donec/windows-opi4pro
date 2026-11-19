using System;
using System.Collections.Generic;
using System.Linq;
using System.Management;

namespace A733Installer
{
    class DiskInfo
    {
        public uint Number;
        public string Name;
        public ulong Size;
        public ushort BusType;
        public string BusName => BusType switch { 7 => "USB", 12 => "SD", 13 => "MMC", 15 => "virtual", _ => "bus " + BusType };
        public override string ToString() => $"{Number}: {Name} - {Size / 1e9:N1} GB ({BusName})";
    }

    class PartitionInfo
    {
        public uint DiskNumber, PartitionNumber;
        public ulong Offset, Size;
        public string[] AccessPaths;
        public string VolumePath => AccessPaths?.FirstOrDefault(p => p.StartsWith(@"\\?\Volume", StringComparison.OrdinalIgnoreCase));
        public IEnumerable<string> Letters => (AccessPaths ?? new string[0]).Where(p => p.Length == 3 && p[1] == ':' && p[2] == '\\');
    }

    /// <summary>Storage management (the WMI classes behind Get-Disk / Get-Partition).</summary>
    static class Disks
    {
        const ushort BusUsb = 7, BusSd = 12, BusMmc = 13, BusVirtual = 15;
        static readonly ManagementScope Scope = new ManagementScope(@"\\.\root\Microsoft\Windows\Storage");

        static IEnumerable<ManagementObject> Query(string wql)
        {
            using (var s = new ManagementObjectSearcher(Scope, new ObjectQuery(wql)))
                foreach (ManagementObject o in s.Get()) yield return o;
        }

        public static List<DiskInfo> Candidates(bool allowVirtual)
        {
            var list = new List<DiskInfo>();
            foreach (var o in Query("SELECT Number, FriendlyName, Size, BusType, IsBoot, IsSystem FROM MSFT_Disk"))
            {
                var d = new DiskInfo
                {
                    Number = (uint)o["Number"],
                    Name = ((string)o["FriendlyName"] ?? "disk").Trim(),
                    Size = (ulong)o["Size"],
                    BusType = (ushort)o["BusType"],
                };
                bool boot = (bool)o["IsBoot"], system = (bool)o["IsSystem"];
                bool removable = d.BusType == BusUsb || d.BusType == BusSd || d.BusType == BusMmc || (allowVirtual && d.BusType == BusVirtual);
                if (removable && !boot && !system && d.Size >= 15UL * 1024 * 1024 * 1024 && d.Size <= 1UL << 40)
                    list.Add(d);
            }
            return list.OrderBy(d => d.Number).ToList();
        }

        public static DiskInfo Get(uint number, bool allowVirtual) => Candidates(allowVirtual).FirstOrDefault(d => d.Number == number);

        public static List<PartitionInfo> Partitions(uint disk) =>
            Query($"SELECT DiskNumber, PartitionNumber, Offset, Size, AccessPaths FROM MSFT_Partition WHERE DiskNumber = {disk}")
                .Select(o => new PartitionInfo
                {
                    DiskNumber = (uint)o["DiskNumber"],
                    PartitionNumber = (uint)o["PartitionNumber"],
                    Offset = (ulong)o["Offset"],
                    Size = (ulong)o["Size"],
                    AccessPaths = o["AccessPaths"] as string[],
                })
                .OrderBy(p => p.PartitionNumber).ToList();

        /// <summary>Ask the storage service to re-read the disk (Update-Disk).</summary>
        public static void Update(uint disk)
        {
            try
            {
                foreach (var o in Query($"SELECT * FROM MSFT_Disk WHERE Number = {disk}"))
                    o.InvokeMethod("Refresh", null);
            }
            catch { }
        }
    }
}
