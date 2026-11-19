using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Text;

namespace A733Installer
{
    static class Proc
    {
        /// <summary>Run a console tool, return exit code; output lines go to the log.</summary>
        public static int Run(string file, string args, Action<string> log, out string output, string stdin = null)
        {
            var psi = new ProcessStartInfo(file, args)
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = stdin != null,
                StandardOutputEncoding = Encoding.GetEncoding(System.Globalization.CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
            };
            var sb = new StringBuilder();
            using (var p = new Process { StartInfo = psi })
            {
                DataReceivedEventHandler h = (s, e) =>
                {
                    if (e.Data == null) return;
                    lock (sb) sb.AppendLine(e.Data);
                    var line = e.Data.TrimEnd();
                    // DISM progress bars ("[====  50.0%  ]") are noise in the log
                    if (log != null && line.Length > 0 && !line.TrimStart().StartsWith("[")) log(line);
                };
                p.OutputDataReceived += h;
                p.ErrorDataReceived += h;
                p.Start();
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                if (stdin != null) { p.StandardInput.Write(stdin); p.StandardInput.Close(); }
                p.WaitForExit();
                output = sb.ToString();
                return p.ExitCode;
            }
        }

        public static int Run(string file, string args, Action<string> log) => Run(file, args, log, out _);

        public static string Sys(string exe) => Path.Combine(Environment.SystemDirectory, exe);

        public static string PowerShell(string script, out int exit)
        {
            exit = Run(Sys(@"WindowsPowerShell\v1.0\powershell.exe"),
                       "-NoProfile -ExecutionPolicy Bypass -Command \"" + script.Replace("\"", "\\\"") + "\"", null, out var o);
            return o;
        }

        public static string Quote(string s) => "'" + s.Replace("'", "''") + "'";
    }

    /// <summary>Files embedded in the exe (firmware, drivers, WinPE script).</summary>
    static class Payload
    {
        static readonly Assembly Asm = Assembly.GetExecutingAssembly();

        public static Stream Open(string name) =>
            Asm.GetManifestResourceStream("payload/" + name) ?? throw new FileNotFoundException("embedded file missing: " + name);

        public static byte[] Firmware()
        {
            using (var gz = new GZipStream(Open("firmware.img.gz"), CompressionMode.Decompress))
            using (var ms = new MemoryStream())
            {
                gz.CopyTo(ms);
                return ms.ToArray();
            }
        }

        public static void ExtractDir(string prefix, string dest)
        {
            Directory.CreateDirectory(dest);
            foreach (var n in Asm.GetManifestResourceNames().Where(n => n.StartsWith("payload/" + prefix + "/")))
            {
                using (var src = Asm.GetManifestResourceStream(n))
                using (var dst = File.Create(Path.Combine(dest, n.Substring(("payload/" + prefix + "/").Length))))
                    src.CopyTo(dst);
            }
        }

        public static void ExtractFile(string name, string dest)
        {
            using (var src = Open(name))
            using (var dst = File.Create(dest))
                src.CopyTo(dst);
        }
    }
}
