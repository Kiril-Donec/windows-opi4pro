using System;
using System.Linq;
using System.Reflection;
using System.Windows;

namespace A733Installer
{
    public partial class App : Application
    {
        public static string Version => Assembly.GetExecutingAssembly().GetName().Version.ToString(3);

        // Developer switches: --allow-virtual (VHD instead of a card), --skip-image (do not copy install.wim),
        // --lang en|ru, --screenshot <file.png> [--variant setup|progress]
        public static string[] Args = new string[0];
        public static bool Has(string flag) => Args.Contains(flag, StringComparer.OrdinalIgnoreCase);
        public static string Value(string flag)
        {
            int i = Array.FindIndex(Args, a => string.Equals(a, flag, StringComparison.OrdinalIgnoreCase));
            return i >= 0 && i + 1 < Args.Length ? Args[i + 1] : null;
        }

        protected override void OnStartup(StartupEventArgs e)
        {
            Args = e.Args;
            // Without the window (tests / scripting):
            //   --run --disk N --source <iso|folder> --index I [--log file] [--allow-virtual] [--skip-image]
            if (Has("--run"))
            {
                var log = Value("--log") ?? System.IO.Path.Combine(System.IO.Path.GetTempPath(), "a733-installer-run.log");
                void L(string s) => System.IO.File.AppendAllText(log, $"{DateTime.Now:HH:mm:ss} {s}{Environment.NewLine}");
                int code = 0;
                try
                {
                    var inst = new Installer { Log = L, Step = i => L($"=== step {i}") };
                    inst.Run(new InstallOptions
                    {
                        Disk = uint.Parse(Value("--disk")), Source = Value("--source"), Index = int.Parse(Value("--index")),
                        AllowVirtual = Has("--allow-virtual"), SkipInstallImage = Has("--skip-image"),
                    }, System.Threading.CancellationToken.None);
                }
                catch (Exception ex) { L("ERROR: " + ex); code = 1; }
                Shutdown(code);
                return;
            }
            DispatcherUnhandledException += (s, x) =>
            {
                MessageBox.Show(x.Exception.Message, "A733 installer", MessageBoxButton.OK, MessageBoxImage.Error);
                x.Handled = true;
            };
            base.OnStartup(e);
            new MainWindow().Show();
        }
    }
}
