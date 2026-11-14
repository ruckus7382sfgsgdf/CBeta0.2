// Triforce Setup's window: install -> gather (update check, downloads) -> forge (choose the game, build) -> play.
// The pages are on the left; on the right the crest (Stage.cs), whose stones go out when work starts and light as it is done.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;

namespace TriforceSetup;

sealed partial class Shell
{
    public readonly Window Window;
    const long NeededBytes = 60L << 30;
    // (the build's steps, and the stone each one lights when it is done; the last one makes the whole crest gleam)
    static readonly (string Id, S Text, int Stone)[] Stages =
        { ("read", S.Read, Art.Emerald), ("lift", S.Lift, Art.Sapphire), ("compile", S.Compile, Art.Ruby), ("finish", S.Finish, -1) };

    // parts of Main.xaml
    readonly Grid Root, InstallPage, FetchPage, SetupPage, BuildPage, NupRow, AskLayer;
    readonly TextBox InstallPath, NupPath, RomfsPath, ExefsPath, OutPath, Log;
    readonly TextBlock InstallSpace, FetchTitle, FetchNote, FetchStep, FetchPercent, FetchNumber, Subtitle, UpdateText, CheckText,
        SpaceText, BuildTitle, BuildNote, VersionText, AskText;
    readonly Caps LanguageLabel, GameFilesLabel, SaveToLabel, BuildEyebrow;
    readonly CheckBox DesktopIcon, StartIcon;
    readonly Button InstallButton, BrowseInstall, FetchSkip, FetchCancel, FetchRetry, UpdateGameButton, BrowseA, BrowseB, BrowseC, BrowseD, BuildButton,
        OpenButton, BackButton, CancelButton, PlayButton, AskYes, AskNo;
    readonly Rectangle CheckMark;
    readonly ProgressLine FetchBar;
    readonly Border UpdateBanner;
    readonly StackPanel FolderRows, StageList, FetchNumberRow;
    readonly ColumnDefinition StageColumn;
    readonly RadioButton UseNup, UseFolders;
    readonly ComboBox LanguageBox;
    readonly ToggleButton ShowLog;
    readonly Stage stage;

    readonly string? updateFolder;
    readonly bool relaunched;
    bool merge;
    Release? latest;
    readonly string[]? autoBuild;   // (testing: --auto-build <exefs> <romfs> <out> builds with the installed tools, as the Build button does)
    Language lang;
    readonly Dictionary<string, StageRow> rows = new();
    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly DispatcherTimer checkDelay = new() { Interval = TimeSpan.FromMilliseconds(500) };
    readonly Stopwatch elapsed = new();
    Runner? runner;
    string? current;
    string finished = "";
    int checkSerial;
    bool sourceOk;
    CancellationTokenSource? fetch;
    Action? bannerAction, askYes;
    bool closing;
    Grid? shown;
    readonly string? snapshot;

    string T(S s) => (int)s < lang.Text.Length ? lang.Text[(int)s] : Strings.All[0].Text[(int)s];
    T Part<T>(string name) where T : class => (Window.FindName(name) as T) ?? throw new InvalidOperationException("Main.xaml has no " + name);
    Brush Brush(string key) => (Brush)Window.Resources[key];

    public Shell(string[] args)
    {
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--update" && i + 1 < args.Length) { updateFolder = args[++i]; merge = true; }
            else if (args[i] == "--merge") merge = true;
            else if (args[i] == "--relaunched") relaunched = true;
            else if (args[i] == "--snapshot" && i + 1 < args.Length) snapshot = args[++i];
            else if (args[i] == "--auto-build" && i + 3 < args.Length) autoBuild = new[] { args[++i], args[++i], args[++i] };
        }
        Fx.Still = snapshot != null;
        Art.Load();

        using (var s = Program.Resource("Main.xaml")) Window = (Window)XamlReader.Load(s);
        Root = Part<Grid>("Root"); AskLayer = Part<Grid>("AskLayer");
        InstallPage = Part<Grid>("InstallPage"); FetchPage = Part<Grid>("FetchPage"); SetupPage = Part<Grid>("SetupPage"); BuildPage = Part<Grid>("BuildPage");
        NupRow = Part<Grid>("NupRow");
        InstallPath = Part<TextBox>("InstallPath"); NupPath = Part<TextBox>("NupPath"); RomfsPath = Part<TextBox>("RomfsPath");
        ExefsPath = Part<TextBox>("ExefsPath"); OutPath = Part<TextBox>("OutPath"); Log = Part<TextBox>("Log");
        InstallSpace = Part<TextBlock>("InstallSpace"); FetchTitle = Part<TextBlock>("FetchTitle"); FetchNote = Part<TextBlock>("FetchNote");
        FetchStep = Part<TextBlock>("FetchStep"); FetchPercent = Part<TextBlock>("FetchPercent"); FetchNumber = Part<TextBlock>("FetchNumber");
        Subtitle = Part<TextBlock>("Subtitle"); UpdateText = Part<TextBlock>("UpdateText");
        CheckText = Part<TextBlock>("CheckText"); SpaceText = Part<TextBlock>("SpaceText");
        BuildTitle = Part<TextBlock>("BuildTitle"); BuildNote = Part<TextBlock>("BuildNote"); VersionText = Part<TextBlock>("VersionText");
        AskText = Part<TextBlock>("AskText");
        LanguageLabel = Part<Caps>("LanguageLabel"); GameFilesLabel = Part<Caps>("GameFilesLabel"); SaveToLabel = Part<Caps>("SaveToLabel");
        BuildEyebrow = Part<Caps>("BuildEyebrow");
        DesktopIcon = Part<CheckBox>("DesktopIcon"); StartIcon = Part<CheckBox>("StartIcon");
        InstallButton = Part<Button>("InstallButton"); BrowseInstall = Part<Button>("BrowseInstall"); FetchSkip = Part<Button>("FetchSkip");
        FetchCancel = Part<Button>("FetchCancel"); FetchRetry = Part<Button>("FetchRetry"); UpdateGameButton = Part<Button>("UpdateGameButton");
        BrowseA = Part<Button>("BrowseA"); BrowseB = Part<Button>("BrowseB"); BrowseC = Part<Button>("BrowseC"); BrowseD = Part<Button>("BrowseD");
        BuildButton = Part<Button>("BuildButton"); OpenButton = Part<Button>("OpenButton"); BackButton = Part<Button>("BackButton");
        CancelButton = Part<Button>("CancelButton"); PlayButton = Part<Button>("PlayButton");
        AskYes = Part<Button>("AskYes"); AskNo = Part<Button>("AskNo");
        CheckMark = Part<Rectangle>("CheckMark");
        FetchBar = Part<ProgressLine>("FetchBar"); UpdateBanner = Part<Border>("UpdateBanner");
        FolderRows = Part<StackPanel>("FolderRows"); StageList = Part<StackPanel>("StageList"); FetchNumberRow = Part<StackPanel>("FetchNumberRow");
        StageColumn = Part<ColumnDefinition>("StageColumn");
        UseNup = Part<RadioButton>("UseNup"); UseFolders = Part<RadioButton>("UseFolders");
        LanguageBox = Part<ComboBox>("LanguageBox"); ShowLog = Part<ToggleButton>("ShowLog");
        stage = new Stage();
        Part<Grid>("StageHost").Children.Add(stage.Root);
        InitHome();

        Window.Icon = Program.Image("icon.png");
        VersionText.Text = Program.Version;
        var saved = Install.Get("Language");
        lang = Strings.All.FirstOrDefault(l => l.Code == saved) ?? Strings.Default();

        Wire();
        for (var i = 0; i < Stages.Length; i++)
        {
            rows[Stages[i].Id] = new StageRow(this, T(Stages[i].Text), Stages[i].Stone, i == Stages.Length - 1);
            StageList.Children.Add(rows[Stages[i].Id].Root);
        }
        LanguageBox.ItemsSource = Strings.All;
        LanguageBox.SelectedItem = lang;
        ApplyLanguage();
        FitToScreen();
        // (asked once the window is up: an off-screen run showed a SourceInitialized handler added here is never called)
        Window.Loaded += (_, _) => Frame();
        if (snapshot != null)
        {
            Window.WindowStartupLocation = WindowStartupLocation.Manual;
            Window.Left = -20000;
            Window.ShowActivated = false;
            Window.ShowInTaskbar = false;
            Window.Loaded += (_, _) => Snapshots(snapshot);
        }
        else
        {
            var scripted = Scripted();
            Window.Loaded += (_, _) => { Begin(); if (scripted) RunScript(); };
        }
    }

    /// One composed picture, 1060 x 640: on a screen too small for it (a small laptop at a large text size) the whole of it is
    /// scaled down to fit rather than cut off
    void FitToScreen()
    {
        var area = SystemParameters.WorkArea;
        var k = Math.Min(1, Math.Min((area.Width - 24) / Stage.WindowWidth, (area.Height - 24) / Stage.WindowHeight));
        if (k < 1 && k > 0.4) Root.LayoutTransform = new ScaleTransform(k, k); else k = 1;
        Window.Width = Stage.WindowWidth * k;
        Window.Height = Stage.WindowHeight * k;
        // (should Windows give the window's inside another size than asked for, the window takes up the difference once)
        Window.Loaded += (_, _) =>
        {
            var host = Part<Grid>("Host");
            double dw = Stage.WindowWidth * k - host.ActualWidth, dh = Stage.WindowHeight * k - host.ActualHeight;
            if (Math.Abs(dw) > 0.5 && Math.Abs(dw) < 60) Window.Width += dw;
            if (Math.Abs(dh) > 0.5 && Math.Abs(dh) < 60) Window.Height += dh;
        };
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);
    int edgeAnswer = -1;   // (0: Windows draws the window's edge in our colour)
    string edgeNote = "not asked";

    /// The window's frame as the game's own (host/display.c): dark, with an edge of old gold. Windows 11 draws that edge itself
    /// (round corners and all); where it cannot (Windows 10), the window keeps the fine edge it draws itself
    void Frame()
    {
        try
        {
            var handle = new WindowInteropHelper(Window).Handle;
            edgeNote = "asked, window " + handle;
            var dark = 1;
            DwmSetWindowAttribute(handle, 20, ref dark, 4);
            var edge = 0x001E546E;   // (COLORREF 0x00BBGGRR: RGB 110, 84, 30)
            edgeAnswer = DwmSetWindowAttribute(handle, 34, ref edge, 4);
            if (edgeAnswer == 0) Part<Border>("WindowEdge").Visibility = Visibility.Collapsed;
        }
        catch (Exception e) { edgeNote = e.GetType().Name + ": " + e.Message; }
    }

    void Wire()
    {
        Part<Button>("MinButton").Click += (_, _) => Window.WindowState = WindowState.Minimized;
        Window.StateChanged += (_, _) => Fx.Paused = Window.WindowState == WindowState.Minimized;
        Part<Button>("CloseButton").Click += (_, _) => CloseWindow();
        Window.Closing += (_, e) =>
        {
            // (a build is running: asked first, in the window's own words)
            if (runner != null && !closing)
            {
                e.Cancel = true;
                Ask(T(S.CloseQuestion), T(S.StopClose), T(S.KeepBuilding), () => { closing = true; Window.Close(); });
                return;
            }
            runner?.Cancel();
            fetch?.Cancel();
        };
        AskYes.Click += (_, _) => { var yes = askYes; HideAsk(); yes?.Invoke(); };
        AskNo.Click += (_, _) => HideAsk();
        Window.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Escape && AskLayer.Visibility == Visibility.Visible) { HideAsk(); e.Handled = true; } };
        InstallButton.Click += (_, _) => DoInstall();
        BrowseInstall.Click += (_, _) => { var p = FolderPicker.Pick(Window, "Choose where to install Triforce", InstallPath.Text); if (p != null) InstallPath.Text = System.IO.Path.Combine(p, "Triforce"); };
        InstallPath.TextChanged += (_, _) => InstallSpaceText();
        FetchCancel.Click += (_, _) => { if (fetch != null) fetch.Cancel(); else Window.Close(); };
        FetchRetry.Click += (_, _) => Gather();
        FetchSkip.Click += (_, _) => { if (HasGame) ShowHome(latest == null ? fetchFailure : null); else ShowSetup(); };
        UpdateGameButton.Click += (_, _) => bannerAction?.Invoke();
        UseNup.Checked += (_, _) => SourceKindChanged();
        UseFolders.Checked += (_, _) => SourceKindChanged();
        foreach (var t in new[] { NupPath, RomfsPath, ExefsPath, OutPath }) t.TextChanged += (_, _) => PathsChanged();
        BrowseA.Click += (_, _) => BrowseNup();
        BrowseB.Click += (_, _) => RomfsPath.Text = FolderPicker.Pick(Window, "Choose the romfs folder", RomfsPath.Text) ?? RomfsPath.Text;
        BrowseC.Click += (_, _) => ExefsPath.Text = FolderPicker.Pick(Window, "Choose the exefs folder", ExefsPath.Text) ?? ExefsPath.Text;
        BrowseD.Click += (_, _) => OutPath.Text = FolderPicker.Pick(Window, "Choose where to save the game", OutPath.Text) ?? OutPath.Text;
        // (11-13, a player's note: "If you click Build game folder there is no option to go back", and a second build of a game
        // already built started at one click: 45 minutes and the same game again. Back goes home; Build asks first then)
        BuildButton.Click += (_, _) =>
        {
            if (HasGame) Ask("You have already built a game folder. Do you want to build it again? It takes about 45 minutes.", "Build again", T(S.Back), StartBuild);
            else StartBuild();
        };
        SetupBack = Part<Button>("SetupBack");
        // (11-13) NVIDIA DLSS in the game that is built: NVIDIA's files come under NVIDIA's own license, so the tick stays only
        // once that is accepted. Kept (Dlss=1): an update builds with it again
        DlssTick = Part<CheckBox>("DlssTick");
        DlssTick.IsChecked = DlssChosen;
        DlssTick.Click += (_, _) =>
        {
            if (DlssTick.IsChecked != true) { Install.Set("Dlss", "0"); return; }
            DlssTick.IsChecked = false;
            Ask("NVIDIA DLSS is NVIDIA's, under NVIDIA's own license (github.com/NVIDIA/DLSS, LICENSE.txt). Its files are downloaded to " +
                "this PC; a game folder built with them must not be shared. Do you accept NVIDIA's license?", "I accept", T(S.Back),
                () => { DlssTick.IsChecked = true; Install.Set("Dlss", "1"); });
        };
        SetupBack.Click += (_, _) => ShowHome(homeOffline);
        CancelButton.Click += (_, _) => { fetch?.Cancel(); runner?.Cancel(); };
        BackButton.Click += (_, _) => ShowSetup();
        OpenButton.Click += (_, _) => Process.Start(new ProcessStartInfo("explorer.exe", "\"" + finished + "\"") { UseShellExecute = true });
        PlayButton.Click += (_, _) => Play(finished);
        ShowLog.Checked += (_, _) => LogToggled();
        ShowLog.Unchecked += (_, _) => LogToggled();
        LanguageBox.SelectionChanged += (_, _) =>
        {
            if (LanguageBox.SelectedItem is Language l) { lang = l; Install.Set("Language", l.Code); ApplyLanguage(); }
        };
        Window.Drop += Dropped;
        clock.Tick += (_, _) => ShowClock();
        checkDelay.Tick += (_, _) => { checkDelay.Stop(); CheckSource(); };
    }

    void CloseWindow() => Window.Close();

    /// A question over the whole window: the gold button does it, the other one (or Esc) leaves things as they are
    void Ask(string question, string yes, string no, Action onYes)
    {
        AskText.Text = question;
        AskYes.Content = yes;
        AskNo.Content = no;
        askYes = onYes;
        AskLayer.Visibility = Visibility.Visible;
        Fx.Once(AskLayer, UIElement.OpacityProperty, 0, 1, Fx.Base);
        AskNo.Focus();
    }

    void HideAsk()
    {
        askYes = null;
        AskLayer.Visibility = Visibility.Collapsed;
    }

    void Show(Grid page)
    {
        foreach (var p in new[] { InstallPage, FetchPage, HomePage, SavesPage, SetupPage, BuildPage }) p.Visibility = p == page ? Visibility.Visible : Visibility.Collapsed;
        if (page != BuildPage && ShowLog.IsChecked == true) ShowLog.IsChecked = false;
        // (nothing to wait for on these pages: the crest whole, every stone alight)
        if (page == InstallPage || page == HomePage || page == SavesPage || page == SetupPage) stage.All(true);
        if (shown == page) return;
        shown = page;
        Fx.Arrive(page);
    }

    // ---- start: install first, or straight to gathering ----

    void Begin()
    {
        var dir = Install.InstalledDir;
        try { var old = System.IO.Path.Combine(dir ?? Install.SelfDir, "TriforceSetup.old.exe"); if (File.Exists(old)) File.Delete(old); } catch (Exception) { }
        if (autoBuild != null && Tools.Find() is { } auto)
        {
            Program.Log("auto-build (test) " + string.Join(" | ", autoBuild));
            Run(auto, new List<string> { "--romfs", autoBuild[1], "--exefs", autoBuild[0], "--out", autoBuild[2], "--language", lang.Code });
            return;
        }
        // (TRIFORCE_DEV=1: run in place, without installing, for testing)
        if (Install.RunningInstalled || Environment.GetEnvironmentVariable("TRIFORCE_DEV") == "1")
        {
            Gather();
            return;
        }
        if (dir != null)
        {
            // (installed already: this download becomes the installed Setup, which then carries on)
            try
            {
                var exe = Install.Run(dir, Install.Get("Desktop") != "0", Install.Get("StartMenu") != "0");
                Relaunch(exe, false);
                return;
            }
            catch (Exception) { Gather(); return; }
        }
        InstallPath.Text = Install.DefaultDir;
        Show(InstallPage);
        InstallSpaceText();
        stage.Gleam(500);
    }

    void InstallSpaceText()
    {
        try
        {
            var root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(InstallPath.Text))!;
            var free = new DriveInfo(root).AvailableFreeSpace;
            InstallSpace.Text = "Setup and its tools take about 2 GB. " + root + " has " + (free >> 30) + " GB free.";
            InstallSpace.Foreground = Brush(free > (3L << 30) ? "TextDim" : "Bad");
        }
        catch (Exception) { InstallSpace.Text = "Setup and its tools take about 2 GB."; }
    }

    void DoInstall()
    {
        try
        {
            var dir = System.IO.Path.GetFullPath(InstallPath.Text.Trim());
            var exe = Install.Run(dir, DesktopIcon.IsChecked == true, StartIcon.IsChecked == true);
            Relaunch(exe, false);
        }
        catch (Exception e)
        {
            InstallSpace.Text = "Triforce could not be installed there: " + e.Message;
            InstallSpace.Foreground = Brush("Bad");
        }
    }

    void Relaunch(string exe, bool afterSelfUpdate)
    {
        var a = new List<string>();
        if (updateFolder != null) { a.Add("--update"); a.Add("\"" + updateFolder + "\""); }
        if (afterSelfUpdate) a.Add("--relaunched");
        if (merge && updateFolder == null) a.Add("--merge");
        Process.Start(new ProcessStartInfo(exe, string.Join(" ", a)) { UseShellExecute = false, WorkingDirectory = System.IO.Path.GetDirectoryName(exe)! });
        fetch = null;
        Window.Close();
    }

    // ---- gather: the update check, Setup's own update, the tools ----

    void FetchState(string title, string note, string step)
    {
        FetchTitle.Text = title;
        FetchNote.Text = note;
        FetchStep.Text = step;
        FetchNote.Foreground = Brush("Text");
    }

    /// The line and the large number: how far a download or an unpacking is (detail: "164 / 264 MB")
    void FetchProgress(double f, string detail)
    {
        FetchBar.IsIndeterminate = false;
        FetchBar.Value = f;
        FetchNumber.Text = ((int)(f * 100)).ToString();
        FetchNumberRow.Visibility = Visibility.Visible;
        FetchPercent.Text = detail;
    }

    /// Work of unknown length (asking GitHub): light sweeps along the line, no number
    void FetchUnknown(bool on)
    {
        FetchBar.IsIndeterminate = on;
        if (!on) FetchBar.Value = 0;
        FetchNumberRow.Visibility = Visibility.Hidden;
        FetchPercent.Text = "";
    }

    /// Every start: ask GitHub what is newest (nothing downloaded yet), then the welcome-back page decides. The first start, with
    /// no tools at all, downloads them straight away; --merge (after Setup replaced itself, or the game's own update offer) carries on.
    async void Gather()
    {
        Show(FetchPage);
        FetchRetry.Visibility = FetchSkip.Visibility = Visibility.Collapsed;
        FetchCancel.Content = "Cancel";
        FetchCancel.IsEnabled = true;
        stage.All(false);
        FetchUnknown(true);
        FetchState("Gathering the stones", "Asking GitHub for the newest Triforce.", "Checking for updates");
        fetch = new CancellationTokenSource();
        var token = fetch.Token;
        try
        {
            using var http = Http();
            latest = await Release.Latest(http, token);
            Program.Log("latest release " + latest.Tag + " (setup " + (latest.SetupUrl != null) + ", tools " + (latest.ToolsUrl != null) + "); this Setup " + Program.Version + ", tools " + (Tools.InstalledVersion ?? "none"));
            stage.Set(Art.Emerald, true);
            fetch = null;
            // (the first start, with nothing to play yet: the tools come straight away. With a game here, the start page asks first)
            var hasGame = Install.Get("GameFolder") is { } gf && GameExe(gf) != null;
            if (Tools.InstalledVersion == null && !hasGame || merge) { await Merge(); return; }
            ShowHome(null);
        }
        catch (Exception e)
        {
            fetch = null;
            Program.Log("update check failed: " + e.GetType().Name + ": " + e.Message);
            var cancelled = e is OperationCanceledException && token.IsCancellationRequested;
            // (no answer, or no release yet: the start page all the same when there is something to play or tools to build with)
            var game = Install.Get("GameFolder");
            if (!cancelled && (Tools.InstalledVersion != null || game != null && GameExe(game) != null)) { ShowHome(e); return; }
            Failed(e, cancelled);
        }
    }

    static HttpClient Http()
    {
        var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("TriforceSetup/" + Program.Version);
        return http;
    }

    Exception? fetchFailure;

    void Failed(Exception e, bool cancelled)
    {
        fetchFailure = cancelled ? null : e;
        Show(FetchPage);
        FetchUnknown(false);
        FetchCancel.IsEnabled = true;
        var offline = e is HttpRequestException || e is TaskCanceledException && !cancelled;
        FetchTitle.Text = cancelled ? "Stopped" : offline ? "GitHub could not be reached" : "Something went wrong";
        FetchNote.Text = cancelled ? "Nothing was changed." : offline ? "Check your internet connection, then try again." : e.Message;
        FetchNote.Foreground = Brush(cancelled ? "Text" : "Bad");
        FetchStep.Text = "";
        FetchRetry.Visibility = Visibility.Visible;
        FetchSkip.Visibility = Tools.Find() != null ? Visibility.Visible : Visibility.Collapsed;
        FetchCancel.Content = "Close";
    }

    /// Update & merge: Setup itself first (it restarts and carries on), then the new tools, then the game folder built again with
    /// them from the game files it keeps (saves and settings stay)
    async Task Merge()
    {
        Show(FetchPage);
        FetchRetry.Visibility = FetchSkip.Visibility = Visibility.Collapsed;
        FetchCancel.Content = "Cancel";
        stage.Set(Art.Sapphire, false);
        stage.Set(Art.Ruby, false);
        stage.Set(Art.Emerald, true);
        fetch = new CancellationTokenSource();
        var token = fetch.Token;
        var release = latest!;
        try
        {
            using var http = Http();
            // 1. Setup itself
            if (!relaunched && Program.Version != "dev" && release.Tag != Program.Version && release.SetupUrl != null)
            {
                FetchState("A new Setup is here", "Triforce Setup " + release.Tag + " replaces " + Program.Version + ". It restarts by itself and carries on.", "Downloading Setup " + release.Tag);
                var dir = Install.InstalledDir ?? Install.SelfDir;
                var fresh = System.IO.Path.Combine(dir, "TriforceSetup.new.exe");
                FetchProgress(0, "");
                await Release.Download(http, release.SetupUrl, fresh, release.SetupSize, (f, have) => Ui(() => FetchProgress(f, (have >> 10) + " KB")), token);
                var self = System.IO.Path.Combine(dir, Program.SetupAsset);
                var old = System.IO.Path.Combine(dir, "TriforceSetup.old.exe");
                if (File.Exists(old)) File.Delete(old);
                File.Move(self, old);
                File.Move(fresh, self);
                Program.Log("Setup updated to " + release.Tag + ", restarting");
                merge = true;
                Relaunch(self, true);
                return;
            }
            stage.Set(Art.Sapphire, true);

            // 2. the tools
            if (Tools.InstalledVersion != release.Tag)
            {
                if (release.ToolsUrl == null) throw new InvalidOperationException("The release " + release.Tag + " has no " + Program.ToolsAsset + ".");
                var have = Tools.InstalledVersion;
                FetchState(have == null ? "Gathering the stones" : "Merging Triforce " + release.Tag,
                           have == null ? "Downloading the Triforce tools (" + (release.ToolsSize >> 20) + " MB), once. Updates come the same way."
                                        : "Triforce " + release.Tag + " replaces " + have + ". Downloading its tools (" + (release.ToolsSize >> 20) + " MB).",
                           "Downloading the tools");
                var cache = System.IO.Path.Combine(Install.InstalledDir ?? Install.SelfDir, "downloads");
                Directory.CreateDirectory(cache);
                var zip = System.IO.Path.Combine(cache, "Triforce-Tools-" + string.Concat(release.Tag.Where(c => char.IsLetterOrDigit(c) || c == '.' || c == '-')) + ".zip");
                FetchProgress(0, "");
                await Release.Download(http, release.ToolsUrl, zip, release.ToolsSize, (f, got) => Ui(() =>
                {
                    FetchProgress(f, (got >> 20) + " / " + (release.ToolsSize >> 20) + " MB");
                    stage.Charge(Art.Ruby, f * 0.7);
                }), token);
                FetchStep.Text = "Unpacking the tools";
                FetchCancel.IsEnabled = false;
                FetchProgress(0, "");
                await Task.Run(() => Tools.Unpack(zip, release.Tag, f => Ui(() => { FetchProgress(f, ""); stage.Charge(Art.Ruby, 0.7 + f * 0.3); })));
                FetchCancel.IsEnabled = true;
                try { File.Delete(zip); } catch (Exception) { }
                Program.Log("tools " + release.Tag + " unpacked into " + Tools.Folder);
            }
            stage.Set(Art.Ruby, true);
            fetch = null;
            merge = false;

            // 3. the game folder, built again (the game's offer names it; else the one Setup built last)
            var folder = updateFolder ?? Install.Get("GameFolder");
            if (folder != null && GameExe(folder) != null && BuiltVersion(folder) != Tools.InstalledVersion && Tools.Find() is { } tools)
            {
                OutPath.Text = folder;
                BeginUpdate();
                Run(tools, new List<string> { "--update", folder }, keepRows: true);
                return;
            }
            // (nothing to build again: the start page, which now says "up to date" and offers Play)
            if (folder != null && GameExe(folder) != null) ShowHome(null);
            else ShowSetup();
        }
        catch (Exception e)
        {
            fetch = null;
            Program.Log("update failed: " + e.GetType().Name + ": " + e.Message);
            Failed(e, e is OperationCanceledException && token.IsCancellationRequested);
        }
    }

    void Ui(Action a) => Window.Dispatcher.BeginInvoke(a);

    static string? BuiltVersion(string folder) =>
        ReadValue(System.IO.Path.Combine(folder, "triforce_version.txt"), "version") ?? ReadValue(System.IO.Path.Combine(folder, "hookshot_version.txt"), "version");

    static bool HasGame => Install.Get("GameFolder") is { } g && GameExe(g) != null;
    Button SetupBack = null!;   // (the build page's way back home)
    CheckBox DlssTick = null!;   // (NVIDIA DLSS, its license accepted)
    static bool DlssChosen => Install.Get("Dlss") == "1";
    /// Where NVIDIA's DLSS SDK is kept: beside Setup, not in the tools folder (an update replaces that one whole)
    static string NgxSdk => System.IO.Path.Combine(Install.InstalledDir ?? Install.SelfDir, "ngx-sdk");

    /// Play: the game starts when there is one (its settings are in its own options menu), else on to building one
    void AfterHome()
    {
        var folder = Install.Get("GameFolder");
        if (folder != null && GameExe(folder) != null) Play(folder);
        else ShowSetup();
    }

    // ---- forge: choose the game ----

    void ShowSetup()
    {
        if (OutPath.Text.Length == 0)
            OutPath.Text = Install.Get("GameFolder") ?? System.IO.Path.Combine(Install.TestRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "Triforce");
        SetupBack.Content = T(S.Back);
        SetupBack.Visibility = HasGame ? Visibility.Visible : Visibility.Collapsed;   // (no game yet: nothing to go back to)
        Show(SetupPage);
        Banner();
        Refresh();
    }

    /// The banner over the form: the game built before, out of date (Update) or ready (Play)
    void Banner()
    {
        UpdateBanner.Visibility = Visibility.Collapsed;
        var folder = Install.Get("GameFolder");
        if (folder == null || GameExe(folder) == null) return;
        var built = BuiltVersion(folder);
        var tools = Tools.InstalledVersion;
        UpdateBanner.Visibility = Visibility.Visible;
        if (tools != null && built != null && built != tools)
        {
            UpdateText.Text = "Your game in " + folder + " is Triforce " + built + ". Version " + tools + " is ready: updating keeps your saves and settings.";
            UpdateGameButton.Content = "Update game";
            bannerAction = () =>
            {
                if (Tools.Find() is not { } t) return;
                OutPath.Text = folder;
                BeginUpdate();
                Run(t, new List<string> { "--update", folder }, keepRows: true);
            };
        }
        else
        {
            UpdateText.Text = "Your game is ready in " + folder + ".";
            UpdateGameButton.Content = T(S.Play);
            bannerAction = () => Play(folder);
        }
    }

    static string? ReadValue(string file, string name)
    {
        try
        {
            foreach (var line in File.ReadAllLines(file))
                if (line.StartsWith(name + "=")) return line.Substring(name.Length + 1).Trim();
        }
        catch (Exception) { }
        return null;
    }

    /// (testing: --where) what Setup knows about the game, and the page a start would show
    public static string Where()
    {
        var folder = Install.Get("GameFolder");
        var exe = folder != null ? GameExe(folder) : null;
        return "where: GameFolder=[" + (folder ?? "null") + "] exe=[" + (exe ?? "null") + "] built=[" + (folder != null ? BuiltVersion(folder) ?? "null" : "-") +
               "] tools=[" + (Tools.InstalledVersion ?? "null") + "] toolsFolder=[" + Tools.Folder + "] installed=[" + (Install.InstalledDir ?? "null") +
               "] runningInstalled=" + Install.RunningInstalled + " self=[" + Install.Self + "]";
    }

    static string? GameExe(string folder)
    {
        foreach (var n in new[] { "Triforce.exe", "hookshot.exe" })
        {
            var p = System.IO.Path.Combine(folder, n);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    void Play(string folder)
    {
        var exe = GameExe(folder);
        if (exe == null) return;
        if (!Install.Allowed(exe)) { Program.Log("not started (a test run starts only what is in its own folder): " + exe); return; }
        // (Setup just asked about updates: the game's own offer, host/hos_update.c, would ask again)
        var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = folder };
        si.EnvironmentVariables["HOOKSHOT_NO_UPDATE_CHECK"] = "1";
        Process.Start(si);
        Program.Log("started the game: " + exe);
        Window.Close();
    }

    void ApplyLanguage()
    {
        Subtitle.Text = T(S.Subtitle);
        LanguageLabel.Label = T(S.LanguageLabel);
        GameFilesLabel.Label = T(S.GameFiles);
        UseNup.Content = T(S.Nup);
        UseFolders.Content = T(S.Folders);
        BrowseA.Content = BrowseB.Content = BrowseC.Content = BrowseD.Content = T(S.Browse);
        SaveToLabel.Label = T(S.SaveTo);
        BuildButton.Content = T(S.Build);
        ShowLog.Content = T(ShowLog.IsChecked == true ? S.HideDetails : S.ShowDetails);
        CancelButton.Content = T(S.Cancel);
        BackButton.Content = T(S.Back);
        OpenButton.Content = T(S.OpenFolder);
        PlayButton.Content = T(S.Play);
        foreach (var (id, text, _) in Stages) rows[id].SetText(T(text));
        if (!sourceOk && !SourceGiven) Say(T(S.DropHint), "TextDim", null);
        else CheckSource();
        if (SetupPage.Visibility == Visibility.Visible) Banner();
        Refresh();
    }

    void SourceKindChanged()
    {
        NupRow.Visibility = UseNup.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        FolderRows.Visibility = UseNup.IsChecked == true ? Visibility.Collapsed : Visibility.Visible;
        PathsChanged();
    }

    void PathsChanged()
    {
        sourceOk = false;
        checkDelay.Stop();
        checkDelay.Start();
        Refresh();
    }

    bool SourceGiven => UseNup.IsChecked == true ? File.Exists(NupPath.Text)
        : Directory.Exists(RomfsPath.Text) && File.Exists(System.IO.Path.Combine(ExefsPath.Text, "main"));

    void Refresh()
    {
        BuildButton.IsEnabled = sourceOk && OutPath.Text.Length > 2;
        try
        {
            var root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(OutPath.Text))!;
            var free = new DriveInfo(root).AvailableFreeSpace;
            SpaceText.Text = T(S.Needs) + " " + string.Format(T(S.Free), root, free >> 30);
            SpaceText.Foreground = Brush(free >= NeededBytes ? "TextDim" : "Bad");
        }
        catch (Exception) { SpaceText.Text = T(S.Needs); }
    }

    async void CheckSource()
    {
        if (!SourceGiven) { Say(T(S.DropHint), "TextDim", null); return; }
        var serial = ++checkSerial;
        Say(T(S.Checking), "TextDim", "TextFaint");
        var tools = Tools.Find();
        if (tools == null) { Say(T(S.ToolsMissing), "Bad", "Bad"); return; }
        var args = new List<string> { "--check" };
        args.AddRange(UseNup.IsChecked == true ? new[] { "--nup", NupPath.Text } : new[] { "--exefs", ExefsPath.Text });
        var (ok, text) = await Runner.Check(tools, args);
        if (serial != checkSerial) return;
        sourceOk = ok;
        Say(ok ? T(S.VersionOk) : text.StartsWith("This is build") ? T(S.VersionBad) : text, ok ? "Good" : "Bad", ok ? "Good" : "Bad");
        Refresh();
    }

    /// The line under the game files: what the check found, with a diamond in its colour (none for the plain hint)
    void Say(string text, string brush, string? mark)
    {
        CheckText.Text = text;
        CheckText.Foreground = Brush(brush);
        CheckMark.Visibility = mark == null ? Visibility.Collapsed : Visibility.Visible;
        if (mark != null) CheckMark.Fill = Brush(mark);
    }

    void Dropped(object sender, DragEventArgs e)
    {
        if (SetupPage.Visibility != Visibility.Visible || e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return;
        foreach (var p in paths)
        {
            var name = System.IO.Path.GetFileName(p).ToLowerInvariant();
            if (name.EndsWith(".nup")) { UseNup.IsChecked = true; NupPath.Text = p; }
            else if (name == "romfs") { UseFolders.IsChecked = true; RomfsPath.Text = p; }
            else if (name == "exefs") { UseFolders.IsChecked = true; ExefsPath.Text = p; }
            else if (Directory.Exists(System.IO.Path.Combine(p, "romfs")) && Directory.Exists(System.IO.Path.Combine(p, "exefs")))
            {
                UseFolders.IsChecked = true;
                RomfsPath.Text = System.IO.Path.Combine(p, "romfs");
                ExefsPath.Text = System.IO.Path.Combine(p, "exefs");
            }
        }
    }

    void BrowseNup()
    {
        var d = new OpenFileDialog { Filter = "Game file (*.nup)|*.nup", Title = "Choose your game file" };
        if (d.ShowDialog(Window) == true) NupPath.Text = d.FileName;
    }

    // ---- the build ----

    void StartBuild()
    {
        var tools = Tools.Find();
        if (tools == null) return;
        var args = new List<string>();
        if (UseNup.IsChecked == true) args.AddRange(new[] { "--nup", NupPath.Text });
        else args.AddRange(new[] { "--romfs", RomfsPath.Text, "--exefs", ExefsPath.Text });
        args.AddRange(new[] { "--out", OutPath.Text, "--language", lang.Code });
        Run(tools, args);
    }

    void BeginUpdate()
    {
        foreach (var r in rows.Values) r.Reset();
        Log.Clear();
        BuildTitle.Text = T(S.Updating);
        BuildTitle.Foreground = Brush("TextHi");
        BuildNote.Text = "";
        Show(BuildPage);
        SetButtons(cancel: true);
    }

    void Run(Tools tools, List<string> args, bool keepRows = false)
    {
        if (!keepRows) BuildTitle.Text = T(S.Building);
        BuildTitle.Foreground = Brush("TextHi");
        foreach (var r in rows.Values) r.Reset();
        if (!keepRows) Log.Clear();
        current = null;
        finished = "";
        BuildEyebrow.Label = "III · Forge";
        BuildNote.Foreground = Brush("Text");
        Show(BuildPage);
        stage.All(false);
        SetButtons(cancel: true);
        elapsed.Restart();
        clock.Start();
        // (NVIDIA DLSS chosen: its SDK fetched first, only what is missing, and built in; a build and an update alike)
        var dlss = DlssChosen;
        if (dlss && !args.Contains("--fetch-ngx-sdk")) args.AddRange(new[] { "--fetch-ngx-sdk", "--accept-nvidia-license" });
        Program.Log("build: make_game.py " + string.Join(" ", args) + " (tools " + tools.Root + ")");
        runner = new Runner(tools, args, dlss ? NgxSdk : null);
        ShowClock();
        runner.Line += l => Ui(() => Line(l));
        runner.Finished += failure => Ui(() => Ended(failure));
        runner.Start();
    }

    void Line(string line)
    {
        if (line.StartsWith("@@stage "))
        {
            var id = line.Split(' ')[1];
            if (current != null && rows.TryGetValue(current, out var was)) was.Done();
            current = id;
            if (rows.TryGetValue(id, out var row)) row.Begin();
        }
        else if (line.StartsWith("@@progress "))
        {
            if (current != null && rows.TryGetValue(current, out var row) &&
                double.TryParse(line.Substring(11), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f))
                row.Progress(f);
        }
        else if (line.StartsWith("@@done ")) finished = line.Substring(7);
        if (!line.StartsWith("@@progress"))
        {
            Log.AppendText(line + "\n");
            if (Log.LineCount > 4000) Log.Text = Log.Text.Substring(Log.Text.Length / 2);
            if (ShowLog.IsChecked == true) Log.ScrollToEnd();
        }
    }

    void Ended(string? failure)
    {
        clock.Stop();
        elapsed.Stop();
        runner = null;
        Program.Log("build ended: " + (finished.Length > 0 ? "done, " + finished : failure ?? "failed") + " after " + Span(elapsed.Elapsed));
        if (AskLayer.Visibility == Visibility.Visible) HideAsk();   // (the question was about a build that is over now)
        if (finished.Length > 0)
        {
            foreach (var r in rows.Values) r.Done();
            stage.All(true);
            stage.Gleam(500);
            BuildEyebrow.Label = "IV · Play";
            BuildTitle.Text = T(S.Ready);
            BuildNote.Text = finished + "  ·  " + T(S.BuiltIn) + " " + Span(elapsed.Elapsed);
            Install.Set("GameFolder", finished);
            SetButtons(done: true);
            return;
        }
        BuildTitle.Text = T(failure == "cancelled" ? S.Cancelled : S.Stopped);
        BuildNote.Text = failure == "cancelled" ? T(S.CancelledNote) : failure ?? "";
        BuildNote.Foreground = Brush(failure == "cancelled" ? "Text" : "Bad");
        if (current != null && rows.TryGetValue(current, out var row)) row.Stopped();
        SetButtons(back: true);
    }

    void ShowClock()
    {
        if (runner != null) BuildNote.Text = T(S.Patience) + "  " + T(S.Elapsed) + ": " + Span(elapsed.Elapsed);
    }

    static string Span(TimeSpan t) => t.TotalHours >= 1 ? (int)t.TotalHours + " h " + t.Minutes + " min" : t.TotalMinutes >= 1 ? t.Minutes + " min" : t.Seconds + " s";

    void SetButtons(bool cancel = false, bool done = false, bool back = false)
    {
        CancelButton.Visibility = cancel ? Visibility.Visible : Visibility.Collapsed;
        PlayButton.Visibility = OpenButton.Visibility = done ? Visibility.Visible : Visibility.Collapsed;
        BackButton.Visibility = back ? Visibility.Visible : Visibility.Collapsed;
    }

    /// The build's own words, for those who want them: they take the crest's side of the window while shown
    void LogToggled()
    {
        var show = ShowLog.IsChecked == true;
        Log.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        ShowLog.Content = T(show ? S.HideDetails : S.ShowDetails);
        Grid.SetColumnSpan(BuildPage, show ? 2 : 1);
        StageColumn.Width = new GridLength(show ? 390 : 540);
        stage.Away(show);
        if (show) Log.ScrollToEnd();
    }

    /// One step of the build, on a rail down the page: a diamond (hollow until it starts, bright while it runs, gold when done),
    /// its name, and the progress line while it has progress. Its stone on the crest fills with light as it runs and lights when done.
    sealed class StageRow
    {
        public readonly Grid Root = new() { MinHeight = 46 };
        readonly Shell shell;
        readonly int stone;
        readonly Rectangle mark = new() { Width = 9, Height = 9, StrokeThickness = 1.2, RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new RotateTransform(45) };
        readonly Rectangle ring = new() { Width = 9, Height = 9, StrokeThickness = 1, RenderTransformOrigin = new Point(0.5, 0.5), Opacity = 0, IsHitTestVisible = false };
        readonly ScaleTransform ringGrow = new(1, 1);
        readonly TextBlock name = new() { VerticalAlignment = VerticalAlignment.Top, FontSize = 14.5, Margin = new Thickness(0, 0, 0, 0) };
        readonly TextBlock percent = new() { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, FontSize = 12.5, Margin = new Thickness(0, 1, 0, 0) };
        readonly ProgressLine bar = new() { Margin = new Thickness(0, 2, 0, 0), Visibility = Visibility.Collapsed, VerticalAlignment = VerticalAlignment.Top };

        public StageRow(Shell shell, string text, int stone, bool last)
        {
            this.shell = shell;
            this.stone = stone;
            Root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(30) });
            Root.ColumnDefinitions.Add(new ColumnDefinition());
            Root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(21) });
            Root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            if (!last)
            {
                // (the rail to the next step)
                var rail = new Rectangle { Width = 1, Fill = shell.Brush("Line"), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(5, 19, 0, -3) };
                Grid.SetRowSpan(rail, 2);
                Root.Children.Add(rail);
            }
            ring.RenderTransform = new TransformGroup { Children = { new RotateTransform(45), ringGrow } };
            ring.Stroke = shell.Brush("Gold300");
            foreach (var m in new[] { ring, mark })
            {
                m.HorizontalAlignment = HorizontalAlignment.Left;
                m.VerticalAlignment = VerticalAlignment.Top;
                m.Margin = new Thickness(1, 6, 0, 0);
                Root.Children.Add(m);
            }
            name.Text = text;
            percent.Foreground = shell.Brush("Gold");
            Grid.SetColumn(name, 1);
            Grid.SetColumn(percent, 1);
            Grid.SetRow(bar, 1);
            Grid.SetColumn(bar, 1);
            Root.Children.Add(name);
            Root.Children.Add(percent);
            Root.Children.Add(bar);
            Reset();
        }

        public void SetText(string text) => name.Text = text;

        void Mark(string? fill, string stroke, bool glow)
        {
            mark.Fill = fill == null ? Brushes.Transparent : shell.Brush(fill);
            mark.Stroke = shell.Brush(stroke);
            mark.Effect = glow ? new System.Windows.Media.Effects.DropShadowEffect { Color = Fx.Rgb(Fx.Gold500), BlurRadius = 10, ShadowDepth = 0, Opacity = 0.9 } : null;
        }

        void Pulse(bool on)
        {
            ring.BeginAnimation(UIElement.OpacityProperty, null);
            ringGrow.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            ringGrow.BeginAnimation(ScaleTransform.ScaleYProperty, null);
            ring.Opacity = 0;
            if (!on || Fx.Still) return;
            var time = TimeSpan.FromMilliseconds(1800);
            var grow = new DoubleAnimation(1, 2.6, time) { RepeatBehavior = RepeatBehavior.Forever, EasingFunction = Fx.Out };
            ringGrow.BeginAnimation(ScaleTransform.ScaleXProperty, grow);
            ringGrow.BeginAnimation(ScaleTransform.ScaleYProperty, grow);
            ring.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0.7, 0, time) { RepeatBehavior = RepeatBehavior.Forever });
        }

        public void Reset()
        {
            Mark(null, "TextFaint", false);
            Pulse(false);
            name.Foreground = shell.Brush("TextFaint");
            percent.Text = "";
            bar.Visibility = Visibility.Collapsed;
            bar.Value = 0;
        }

        public void Begin()
        {
            Mark("Gold300", "Gold300", true);
            Pulse(true);
            name.Foreground = shell.Brush("TextHi");
            if (stone >= 0) shell.stage.Charge(stone, 0);
        }

        public void Progress(double f)
        {
            bar.Visibility = Visibility.Visible;
            bar.Value = f;
            percent.Text = (int)(f * 100) + "%";
            if (stone >= 0) shell.stage.Charge(stone, f);
        }

        public void Done()
        {
            Mark("Gold", "Gold", false);
            Pulse(false);
            name.Foreground = shell.Brush("TextDim");
            percent.Text = "";
            bar.Visibility = Visibility.Collapsed;
            if (stone >= 0) shell.stage.Set(stone, true);
        }

        public void Stopped()
        {
            Mark(null, "Bad", false);
            Pulse(false);
            name.Foreground = shell.Brush("Bad");
            bar.Visibility = Visibility.Collapsed;
            percent.Text = "";
            if (stone >= 0) shell.stage.Set(stone, false);
        }
    }
}
