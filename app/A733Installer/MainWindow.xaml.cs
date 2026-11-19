using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace A733Installer
{
    public partial class MainWindow : Window
    {
        string lang;
        List<DiskInfo> disks = new List<DiskInfo>();
        List<Edition> editions = new List<Edition>();
        string source;
        string statusKey, statusArg;
        string phase = "setup";          // setup | run | done | fail
        int current = -1;
        double? pct;
        string error;
        readonly List<TextBlock> icons = new List<TextBlock>(), labels = new List<TextBlock>();
        double? chrome;
        bool running;
        CancellationTokenSource cts;
        readonly string logPath;
        readonly bool allowVirtual = App.Has("--allow-virtual");

        static readonly Brush Gray = Br("#9CA3AF"), Dark = Br("#1F2937"), Green = Br("#059669"),
                              Orange = Br("#EA580C"), Red = Br("#DC2626"), DarkRed = Br("#991B1B"), Accent = Br("#C2410C");
        static Brush Br(string hex) => (Brush)new BrushConverter().ConvertFromString(hex);

        string T(string key) => Strings.Text[lang][key];

        public MainWindow()
        {
            InitializeComponent();
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "A733Installer", "logs");
            Directory.CreateDirectory(dir);
            logPath = Path.Combine(dir, $"install-{DateTime.Now:yyyyMMdd-HHmmss}.log");

            lang = App.Value("--lang") ?? (new[] { "ru", "uk", "be" }.Contains(CultureInfo.CurrentUICulture.TwoLetterISOLanguageName) ? "ru" : "en");
            if (!Strings.Text.ContainsKey(lang)) lang = "en";

            LangEn.Click += (s, e) => { lang = "en"; ApplyLang(); };
            LangRu.Click += (s, e) => { lang = "ru"; ApplyLang(); };
            RefreshBtn.Click += (s, e) => Fill();
            DiskBox.SelectionChanged += (s, e) => UpdateStart();
            EditionBox.SelectionChanged += (s, e) => UpdateStart();
            AgreeBox.Click += (s, e) => UpdateStart();
            MsLink.RequestNavigate += (s, e) => Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
            IsoBtn.Click += (s, e) =>
            {
                var dlg = new Microsoft.Win32.OpenFileDialog { Filter = "ISO (*.iso)|*.iso" };
                if (dlg.ShowDialog(this) == true) LoadSource(dlg.FileName);
            };
            FolderBtn.Click += (s, e) =>
            {
                using (var dlg = new System.Windows.Forms.FolderBrowserDialog { Description = T("FolderDesc") })
                    if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK) LoadSource(dlg.SelectedPath);
            };
            LogBtn.Click += (s, e) => { if (File.Exists(logPath)) Process.Start("notepad.exe", "\"" + logPath + "\""); };
            CloseBtn.Click += (s, e) => Close();
            StartBtn.Click += (s, e) => Start();

            // SizeToContent only for the first layout: later growth (status line, progress page,
            // details) would leave an unpainted strip at the bottom. Follow the content explicitly.
            Root.SizeChanged += (s, e) => { if (chrome.HasValue) Height = Math.Ceiling(Root.ActualHeight + chrome.Value); };
            ContentRendered += (s, e) =>
            {
                chrome = ActualHeight - Root.ActualHeight;
                SizeToContent = SizeToContent.Manual;
                Height = Math.Ceiling(Root.ActualHeight + chrome.Value);
                Fill();
                if (App.Value("--screenshot") is string shot) Screenshot(shot);
            };
            Closing += OnClosing;
            ApplyLang();
            UpdateStart();
        }

        // ------------------------------------------------------------------ texts
        void ApplyLang()
        {
            Title = T("Title");
            HeadTitle.Text = T("Title");
            SubTitle.Text = string.Format(T("Sub"), App.Version);
            Badge.Text = T("Badge");
            LangEn.Background = lang == "en" ? Brushes.White : Brushes.Transparent;
            LangEn.Foreground = lang == "en" ? Accent : Brushes.White;
            LangRu.Background = lang == "ru" ? Brushes.White : Brushes.Transparent;
            LangRu.Foreground = lang == "ru" ? Accent : Brushes.White;
            WarnBold.Text = T("WarnBold"); WarnRest.Text = T("WarnRest");
            S1Title.Text = T("S1Title"); S1Hint.Text = T("S1Hint"); RefreshBtn.Content = T("Refresh");
            S2Title.Text = T("S2Title"); S2HintA.Text = T("S2HintA"); S2HintB.Text = T("S2HintB");
            IsoBtn.Content = T("Iso"); FolderBtn.Content = T("Folder");
            if (SourceText.Tag as string != "set") SourceText.Text = T("NotSelected");
            S3Title.Text = T("S3Title"); S3Hint.Text = T("S3Hint");
            AgreeText.Text = T("Agree"); StartBtn.Content = T("Start");
            if (statusKey != null) Status.Text = string.Format(T(statusKey), statusArg ?? "");
            DoneHead.Text = T("DoneHead"); DoneText.Text = T("DoneText"); FailHead.Text = T("FailHead");
            LogExpander.Header = T("Details"); LogBtn.Content = T("OpenLog"); CloseBtn.Content = T("Close");
            if (phase != "setup") RenderProgress();
        }

        void SetStatus(string key, string arg = null)
        {
            statusKey = key; statusArg = arg;
            if (key == null) { StatusBox.Visibility = Visibility.Collapsed; return; }
            Status.Text = string.Format(T(key), arg ?? "");
            StatusBox.Visibility = Visibility.Visible;
        }

        void UpdateStart()
        {
            StartBtn.IsEnabled = DiskBox.SelectedIndex >= 0 && EditionBox.SelectedIndex >= 0 && AgreeBox.IsChecked == true && source != null;
            StepBadge(Num1, Num1Text, "1", DiskBox.SelectedIndex >= 0);
            StepBadge(Num2, Num2Text, "2", source != null);
            StepBadge(Num3, Num3Text, "3", source != null && EditionBox.SelectedIndex >= 0);
        }

        // step number circle: orange number, or a green check once the step is done
        static void StepBadge(Border b, TextBlock t, string n, bool done)
        {
            b.Background = done ? Br("#D1FAE5") : Br("#FDE7D9");
            t.Text = done ? "\u2714" : n;
            t.Foreground = done ? Br("#059669") : Br("#C2410C");
        }

        // ------------------------------------------------------------------ setup page
        void Fill()
        {
            DiskBox.Items.Clear();
            try { disks = Disks.Candidates(allowVirtual); } catch { disks = new List<DiskInfo>(); }
            foreach (var d in disks) DiskBox.Items.Add(d.ToString());
            if (disks.Count == 1) DiskBox.SelectedIndex = 0;
            if (disks.Count > 0) { if (statusKey == "NoCard") SetStatus(null); }
            else SetStatus("NoCard");
            UpdateStart();
        }

        async void LoadSource(string path)
        {
            SourceText.Text = path; SourceText.Tag = "set"; SourceText.Foreground = Dark;
            EditionBox.Items.Clear(); editions.Clear(); source = null;
            S3Hint.Text = T("Reading");
            Cursor = Cursors.Wait;
            IsoBtn.IsEnabled = FolderBtn.IsEnabled = false;
            try
            {
                var list = await Task.Run(() =>
                {
                    using (var src = new WindowsSource(path))
                        return WindowsSource.Editions(src.InstallImage).Where(e => e.IsArm64).ToList();
                });
                if (list.Count == 0) { SetStatus("ErrNoArm"); }
                else
                {
                    editions = list;
                    int pick = 0;
                    for (int i = 0; i < list.Count; i++)
                    {
                        EditionBox.Items.Add(list[i].Name);
                        if (list[i].Name.EndsWith("Pro")) pick = i;
                    }
                    EditionBox.SelectedIndex = pick;
                    source = path;
                    SetStatus(disks.Count == 0 ? "NoCard" : null);
                }
            }
            catch (SourceException ex) { SetStatus(ex.Key, ex.Detail); }
            catch (Exception ex) { SetStatus("ImgErr", ex.Message); }
            finally
            {
                Cursor = null;
                IsoBtn.IsEnabled = FolderBtn.IsEnabled = true;
                S3Hint.Text = T("S3Hint");
                UpdateStart();
            }
        }

        // ------------------------------------------------------------------ progress page
        void BuildSteps()
        {
            StepPanel.Children.Clear(); icons.Clear(); labels.Clear();
            for (int i = 0; i < Installer.StepCount; i++)
            {
                var row = new DockPanel { Margin = new Thickness(0, 3, 0, 3) };
                var ic = new TextBlock { Width = 26, FontSize = 14 };
                var lb = new TextBlock();
                row.Children.Add(ic); row.Children.Add(lb);
                StepPanel.Children.Add(row);
                icons.Add(ic); labels.Add(lb);
            }
        }

        void RenderProgress()
        {
            for (int i = 0; i < labels.Count; i++)
            {
                labels[i].Text = T("Step" + i);
                bool done = phase == "done" || i < current;
                if (done) { icons[i].Text = "\u2714"; icons[i].Foreground = Green; labels[i].Foreground = Dark; labels[i].FontWeight = FontWeights.Normal; }
                else if (i == current && phase == "fail") { icons[i].Text = "\u2716"; icons[i].Foreground = Red; labels[i].Foreground = DarkRed; labels[i].FontWeight = FontWeights.SemiBold; }
                else if (i == current) { icons[i].Text = "\u25C9"; icons[i].Foreground = Orange; labels[i].Foreground = Dark; labels[i].FontWeight = FontWeights.SemiBold; }
                else { icons[i].Text = "\u25CB"; icons[i].Foreground = Gray; labels[i].Foreground = Gray; labels[i].FontWeight = FontWeights.Normal; }
            }
            switch (phase)
            {
                case "done": ProgTitle.Text = T("DoneTitle"); ProgSub.Text = T("DoneSub"); ProgText.Text = " "; break;
                case "fail": ProgTitle.Text = T("FailTitle"); ProgSub.Text = logPath; ProgText.Text = " "; FailText.Text = error; break;
                default:
                    ProgTitle.Text = T("ProgTitle"); ProgSub.Text = T("ProgSub");
                    ProgText.Text = current == 5 && pct.HasValue ? string.Format(T("Copying"), pct.Value)
                                  : current >= 0 ? T("Step" + current) + "..." : T("Starting");
                    break;
            }
        }

        void AppendLog(string line)
        {
            var stamped = $"{DateTime.Now:HH:mm:ss} {line}";
            try { File.AppendAllText(logPath, stamped + Environment.NewLine); } catch { }
            Dispatcher.BeginInvoke(new Action(() =>
            {
                LogBox.AppendText((LogBox.Text.Length > 0 ? Environment.NewLine : "") + stamped);
                LogBox.ScrollToEnd();
            }));
        }

        async void Start()
        {
            var d = disks[DiskBox.SelectedIndex];
            var ed = editions[EditionBox.SelectedIndex];
            if (MessageBox.Show(this, string.Format(T("Confirm"), ed.Name, d), T("ConfirmTitle"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                return;
            var o = new InstallOptions { Disk = d.Number, Source = source, Index = ed.Index, AllowVirtual = allowVirtual, SkipInstallImage = App.Has("--skip-image") };
            phase = "run"; current = -1; pct = null; running = true;
            SetupPage.Visibility = Visibility.Collapsed; ProgressPage.Visibility = Visibility.Visible;
            BuildSteps(); RenderProgress();
            Bar.IsIndeterminate = true;
            cts = new CancellationTokenSource();
            var inst = new Installer
            {
                Log = AppendLog,
                Step = i => Dispatcher.BeginInvoke(new Action(() => { current = i; pct = null; Bar.IsIndeterminate = true; RenderProgress(); })),
                Percent = p => Dispatcher.BeginInvoke(new Action(() => { pct = p; Bar.IsIndeterminate = false; Bar.Value = p; RenderProgress(); })),
            };
            AppendLog($"disk {d}, source {source}, edition {ed.Name} (index {ed.Index})");
            try
            {
                await Task.Run(() => inst.Run(o, cts.Token));
                phase = "done";
                Bar.IsIndeterminate = false; Bar.Value = 100;
                DonePanel.Visibility = Visibility.Visible;
            }
            catch (Exception ex)
            {
                AppendLog("ERROR: " + ex.Message);
                phase = "fail"; error = ex.Message; current = Math.Max(current, 0);
                Bar.IsIndeterminate = false; Bar.Value = 0;
                FailPanel.Visibility = Visibility.Visible; LogExpander.IsExpanded = true;
            }
            running = false;
            CloseBtn.IsEnabled = true;
            RenderProgress();
        }

        void OnClosing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (!running) return;
            if (MessageBox.Show(this, T("StopQ"), T("StopTitle"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                e.Cancel = true;
            else
                cts?.Cancel();
        }

        // ------------------------------------------------------------------ docs screenshots
        void Screenshot(string path)
        {
            if (App.Value("--variant") == "progress")
            {
                phase = "run"; current = 5; pct = 62;
                SetupPage.Visibility = Visibility.Collapsed; ProgressPage.Visibility = Visibility.Visible;
                BuildSteps(); RenderProgress(); Bar.Value = 62;
            }
            else if (App.Value("--variant") != "raw")
            {
                SetStatus(null);
                DiskBox.Items.Clear(); DiskBox.Items.Add("2: Generic STORAGE DEVICE - 61,9 GB (USB)"); DiskBox.SelectedIndex = 0;
                SourceText.Text = @"D:\Downloads\Win11_25H2_English_Arm64.iso"; SourceText.Tag = "set"; SourceText.Foreground = Dark;
                EditionBox.Items.Clear(); EditionBox.Items.Add("Windows 11 Pro"); EditionBox.SelectedIndex = 0;
                AgreeBox.IsChecked = true; source = SourceText.Text; UpdateStart();
            }
            Topmost = true;
            UpdateLayout();
            var m = PresentationSource.FromVisual(this).CompositionTarget.TransformToDevice;
            const int pad = 36;
            var back = new Window
            {
                WindowStyle = WindowStyle.None, ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false, Topmost = true,
                Background = Br("#E2E8F0"), Left = Left - pad, Top = Top - pad, Width = ActualWidth + 2 * pad, Height = ActualHeight + 2 * pad,
            };
            back.Show();
            Topmost = false; Topmost = true;
            Activate();
            for (int i = 0; i < 12; i++)
            {
                Dispatcher.Invoke(DispatcherPriority.Background, new Action(() => { }));
                Thread.Sleep(80);
            }
            int x = (int)((Left - pad) * m.M11), y = (int)((Top - pad) * m.M22);
            int w = (int)((ActualWidth + 2 * pad) * m.M11), h = (int)((ActualHeight + 2 * pad) * m.M22);
            using (var bmp = new System.Drawing.Bitmap(w, h))
            {
                using (var g = System.Drawing.Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, new System.Drawing.Size(w, h));
                bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
            }
            back.Close();
            Close();
        }
    }
}
