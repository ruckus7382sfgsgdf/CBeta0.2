// Looking at Setup and driving it without a screen (nothing here runs for a player):
//   --snapshot <folder>            every page and state drawn to a PNG with sample values, then exit
//   TRIFORCE_UI_SCRIPT=<file>      a list of steps (wait for a page, click a button, type a path, take a picture or a run of
//                                  pictures to look at motion) run against the
//                                  real flows in an off-screen window; it carries on in the Setup that an install or a self-update
//                                  starts (the place in the list is kept beside the file). Pictures and script.log go to
//                                  TRIFORCE_UI_SHOTS. Used with TRIFORCE_TEST_ROOT (Setup.cs), which keeps the install in one folder.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace TriforceSetup;

sealed partial class Shell
{
    /// The window's picture as it is now
    RenderTargetBitmap Picture(double scale)
    {
        var rtb = new RenderTargetBitmap((int)(Root.ActualWidth * scale), (int)(Root.ActualHeight * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
        var v = new DrawingVisual();
        using (var dc = v.RenderOpen()) dc.DrawRectangle(Window.Background, null, new Rect(0, 0, Root.ActualWidth, Root.ActualHeight));
        rtb.Render(v);
        rtb.Render(Root);
        rtb.Freeze();
        return rtb;
    }

    static void SavePicture(BitmapSource picture, string dir, string name)
    {
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(picture));
        using var f = File.Create(System.IO.Path.Combine(dir, name + ".png"));
        enc.Save(f);
    }

    async Task Shot(string dir, string name, int settle = 150)
    {
        await Window.Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
        await Task.Delay(settle);
        SavePicture(Picture(1.5), dir, name);
    }

    /// --snapshot <folder>: every page and every state that looks different, with sample values (nothing of this PC in them)
    async void Snapshots(string dir)
    {
        try
        {
            Directory.CreateDirectory(dir);
            Part<Border>("WindowEdge").Visibility = Visibility.Visible;
            const string game = @"C:\Users\you\Games\Triforce";
            lang = Strings.All[0];
            LanguageBox.SelectedItem = lang;

            InstallPath.Text = @"C:\Users\you\AppData\Local\Programs\Triforce";
            InstallSpace.Text = @"Setup and its tools take about 2 GB. C:\ has 212 GB free.";
            Show(InstallPage);
            await Shot(dir, "01-install");

            Show(FetchPage);
            stage.All(false);
            FetchUnknown(true);
            FetchState("Gathering the stones", "Asking GitHub for the newest Triforce.", "Checking for updates");
            FetchCancel.Content = "Cancel";
            await Shot(dir, "02-gather-checking");

            FetchState("Gathering the stones", "Downloading the Triforce tools (264 MB), once. Updates come the same way.", "Downloading the tools");
            stage.Set(Art.Emerald, true); stage.Set(Art.Sapphire, true); stage.Charge(Art.Ruby, 0.62 * 0.7);
            FetchProgress(0.62, "164 / 264 MB");
            await Shot(dir, "03-gather-download");

            stage.All(false);
            FetchUnknown(false);
            FetchTitle.Text = "GitHub could not be reached";
            FetchNote.Text = "Check your internet connection, then try again.";
            FetchNote.Foreground = Brush("Bad");
            FetchStep.Text = "";
            FetchRetry.Visibility = FetchSkip.Visibility = Visibility.Visible;
            FetchCancel.Content = "Close";
            await Shot(dir, "04-gather-failed");
            FetchRetry.Visibility = FetchSkip.Visibility = Visibility.Collapsed;

            // welcome back: ready, an update, nothing built yet, no answer from GitHub
            HomeFill(true, "Triforce v1.1.0", game, "v1.1.0", "v1.1.0", false, null);
            SavesLineFill(true, "6 save slots", "Last played 8 Nov 2026, 02:46");
            Show(HomePage);
            await Shot(dir, "05-home-ready");
            // (the gleam held at seven places on its way across, in gleam\: the same page, so each differs from 05-home-ready
            // only by the band, which must lie on the crest's own pixels and nowhere else)
            Directory.CreateDirectory(System.IO.Path.Combine(dir, "gleam"));
            foreach (var at in new[] { 5, 20, 35, 50, 65, 80, 95 })
            {
                stage.GleamAt(at / 100.0);
                await Shot(System.IO.Path.Combine(dir, "gleam"), "home-gleam-" + at.ToString("00"));
            }
            stage.GleamAt(-1);
            // (and the ring of marks an eighth of a turn on: the fine ring inside it must not change at all)
            stage.TurnMarks(45);
            await Shot(System.IO.Path.Combine(dir, "gleam"), "arc-marks-turned-45");
            stage.TurnMarks(0);
            HomeFill(true, "Triforce v1.0.0", game, "v1.0.0", "v1.1.0", true, null);
            await Shot(dir, "06-home-update");
            HomeFill(false, null, null, null, "v1.1.0", false, null);
            SavesLineFill(null, "None yet", "They can come in from an emulator once your game is built.");
            await Shot(dir, "07-home-no-game");
            HomeFill(true, "Triforce v1.1.0", game, "v1.1.0", "v1.1.0", false, new System.Net.Http.HttpRequestException("offline"));
            SavesLineFill(false, "No saves yet", "Bring them in, or start a new game.");
            await Shot(dir, "08-home-offline");

            // the forge: nothing chosen yet, a checked file with a game to update, folders with the wrong version
            OutPath.Text = game;
            Show(SetupPage);
            checkDelay.Stop();
            Say(T(S.DropHint), "TextDim", null);
            SpaceText.Text = T(S.Needs) + " " + string.Format(T(S.Free), @"C:\", 212);
            await Shot(dir, "09-forge");
            UpdateBanner.Visibility = Visibility.Visible;
            UpdateText.Text = "Your game in " + game + " is Triforce v1.0.0. Version v1.1.0 is ready: updating keeps your saves and settings.";
            NupPath.Text = @"D:\Games\OcarinaOfTime.nup";
            checkDelay.Stop();
            Say(T(S.VersionOk), "Good", "Good");
            BuildButton.IsEnabled = true;
            SpaceText.Text = T(S.Needs) + " " + string.Format(T(S.Free), @"C:\", 212);
            await Shot(dir, "10-forge-checked");
            UpdateBanner.Visibility = Visibility.Collapsed;
            UseFolders.IsChecked = true;
            RomfsPath.Text = @"D:\Games\OcarinaOfTime\romfs"; ExefsPath.Text = @"D:\Games\OcarinaOfTime\exefs";
            checkDelay.Stop();
            Say(T(S.VersionBad), "Bad", "Bad");
            BuildButton.IsEnabled = false;
            SpaceText.Text = T(S.Needs) + " " + string.Format(T(S.Free), @"C:\", 212);
            await Shot(dir, "11-forge-folders");
            foreach (var code in new[] { "de", "ja" })
            {
                lang = Strings.All.First(l => l.Code == code);
                LanguageBox.SelectedItem = lang;
                checkDelay.Stop();
                Say(T(S.VersionBad), "Bad", "Bad");
                SpaceText.Text = T(S.Needs) + " " + string.Format(T(S.Free), @"C:\", 212);
                await Shot(dir, "12-forge-" + code);
            }
            lang = Strings.All[0];
            LanguageBox.SelectedItem = lang;
            checkDelay.Stop();
            UseNup.IsChecked = true;
            checkDelay.Stop();

            // building: a step running, its details, done, stopped, and the question when closing
            Show(BuildPage);
            stage.All(false);
            BuildTitle.Text = T(S.Building);
            BuildNote.Text = T(S.Patience) + "  " + T(S.Elapsed) + ": 14 min";
            rows["read"].Done(); rows["lift"].Done(); rows["compile"].Begin(); rows["compile"].Progress(0.41);
            SetButtons(cancel: true);
            await Shot(dir, "13-build");

            Log.Text = string.Join("\n", new[]
            {
                "@@stage read", "reading D:\\Games\\OcarinaOfTime.nup", "program 0400486024DEC000, update v131072, build e0f3ccfa", "romfs: 18,412 files, 11.6 GB",
                "@@stage lift", "lifting 14,134 functions in 96 pieces", "lift done in 222 s", "@@stage compile",
                "[ 312/ 760] clang -O2 -c sa/gen/code_038.c -o sa/build/code_038.o", "[ 313/ 760] clang -O2 -c sa/gen/code_039.c -o sa/build/code_039.o",
                "[ 314/ 760] clang -O2 -c sa/gen/code_040.c -o sa/build/code_040.o", "[ 315/ 760] clang -O2 -c sa/gen/code_041.c -o sa/build/code_041.o",
            });
            ShowLog.IsChecked = true;
            await Shot(dir, "14-build-details");
            ShowLog.IsChecked = false;

            Ask(T(S.CloseQuestion), T(S.StopClose), T(S.KeepBuilding), () => { });
            await Shot(dir, "15-build-ask-close");
            HideAsk();

            foreach (var r in rows.Values) r.Done();
            stage.All(true);
            BuildEyebrow.Label = "IV · Play";
            BuildTitle.Text = T(S.Ready);
            BuildNote.Text = game + "  ·  " + T(S.BuiltIn) + " 38 min";
            SetButtons(done: true);
            await Shot(dir, "16-build-done");

            foreach (var r in rows.Values) r.Reset();
            stage.All(false);
            BuildEyebrow.Label = "III · Forge";
            rows["read"].Done(); rows["lift"].Done(); rows["compile"].Begin(); rows["compile"].Stopped();
            BuildTitle.Text = T(S.Stopped);
            BuildNote.Text = "The compiler stopped at sa/gen/code_041.c (exit code 1). See the details.";
            BuildNote.Foreground = Brush("Bad");
            SetButtons(back: true);
            await Shot(dir, "17-build-stopped");
            BuildNote.Foreground = Brush("Text");

            // convert saves: what was found, and nothing found
            Show(SavesPage);
            found = new List<SaveSet>
            {
                new() { Name = "Eden", Folder = @"C:\Users\you\AppData\Roaming\eden\nand\user\save\0000000000000000\1401D3E59D96EFFB99D2488BA99C6EBE\" + Saves.TitleId, Slots = 6, Last = new DateTime(2026, 11, 9, 6, 19, 0) },
                new() { Name = "Ryujinx", Folder = @"C:\Users\you\AppData\Roaming\Ryujinx\bis\user\save\0000000000000003\0", Slots = 3, Last = new DateTime(2026, 10, 30, 21, 20, 0) },
                new() { Name = @"yuzu (D:\Emulators\yuzu)", Folder = @"D:\Emulators\yuzu\user\nand\user\save\0000000000000000\8A1C0F6E27B94D13A5E07C2B9F3D6E41\" + Saves.TitleId, Slots = 1, Last = new DateTime(2026, 6, 2, 18, 5, 0) },
            };
            SavesListLabel.Label = "Take the saves from";
            ShowSaveList();
            SavesInfo.Text = "Triforce's saves become Eden's (6 slots, last played 9 Nov 2026). Triforce's current saves (6 slots, last played 8 Nov 2026) are backed up first.";
            SavesInfo.Foreground = Brush("Text");
            SavesTarget.Text = @"Into " + game + @"\user\save\" + Saves.TriforceUid;
            SavesGo.IsEnabled = true;
            await Shot(dir, "18-saves");
            found = new List<SaveSet>();
            ShowSaveList();
            SavesOut.IsChecked = true;
            await Window.Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
            SaveList.ItemsSource = null;
            SavesListLabel.Label = "Put Triforce's saves into";
            SavesInfo.Text = "No emulator saves of this game were found. Use Choose a folder to point at the emulator's save folder for this game (0400486024DEC000).";
            SavesInfo.Foreground = Brush("Text");
            SavesTarget.Text = "";
            SavesGo.IsEnabled = false;
            await Shot(dir, "19-saves-none");
        }
        catch (Exception e)
        {
            File.WriteAllText(System.IO.Path.Combine(dir, "error.txt"), e.ToString());
        }
        Window.Close();
    }

    // ---- TRIFORCE_UI_SCRIPT ----

    string? scriptFile, scriptShots;

    /// A script to follow (testing)? Then the window stays off the screen and the steps start once it is up
    bool Scripted()
    {
        scriptFile = Environment.GetEnvironmentVariable("TRIFORCE_UI_SCRIPT");
        if (string.IsNullOrEmpty(scriptFile) || !File.Exists(scriptFile)) return false;
        scriptShots = Environment.GetEnvironmentVariable("TRIFORCE_UI_SHOTS") is { Length: > 0 } s ? s : System.IO.Path.GetDirectoryName(scriptFile)!;
        Directory.CreateDirectory(scriptShots);
        Window.WindowStartupLocation = WindowStartupLocation.Manual;
        Window.Left = -20000;
        Window.ShowActivated = false;
        Window.ShowInTaskbar = false;
        return true;
    }

    void ScriptLog(string line)
    {
        try { File.AppendAllText(System.IO.Path.Combine(scriptShots!, "script.log"), DateTime.Now.ToString("HH:mm:ss.fff") + "  " + line + Environment.NewLine); } catch (Exception) { }
    }

    async void RunScript()
    {
        var steps = File.ReadAllLines(scriptFile!);
        var place = scriptFile + ".pos";
        var at = File.Exists(place) && int.TryParse(File.ReadAllText(place).Trim(), out var p) ? p : 0;
        ScriptLog("-- Setup " + Program.Version + " (" + Install.Self + ") from step " + at + "; window " + Window.ActualWidth + " x " + Window.ActualHeight + ", inside " + Part<Grid>("Host").ActualWidth + " x " + Part<Grid>("Host").ActualHeight + ", picture " + Root.ActualWidth + " x " + Root.ActualHeight + ", Windows' answer about the edge colour 0x" + edgeAnswer.ToString("X8") + " (" + edgeNote + ")");
        try
        {
            for (; at < steps.Length; at++)
            {
                var line = steps[at].Trim();
                var cut = line.Split(new[] { ' ' }, 3);
                var waits = cut[0] == "wait" || cut[0] == "enabled" || cut[0] == "gone";
                if (!waits) File.WriteAllText(place, (at + 1).ToString());
                if (line.Length == 0 || line.StartsWith("#")) continue;
                string a = cut.Length > 1 ? cut[1] : "", b = cut.Length > 2 ? cut[2] : "";
                switch (cut[0])
                {
                    case "wait":   // wait <page> [seconds]
                        await Until(() => Part<FrameworkElement>(a).Visibility == Visibility.Visible, b, line);
                        break;
                    case "enabled":   // enabled <control> [seconds]
                        await Until(() => Part<FrameworkElement>(a) is { IsEnabled: true, IsVisible: true }, b, line);
                        break;
                    case "gone":   // gone <page> [seconds]
                        await Until(() => Part<FrameworkElement>(a).Visibility != Visibility.Visible, b, line);
                        break;
                    case "click":
                        var c = Part<FrameworkElement>(a);
                        if (!c.IsVisible || !c.IsEnabled) throw new InvalidOperationException(a + " cannot be clicked (visible " + c.IsVisible + ", enabled " + c.IsEnabled + ")");
                        if (c is ToggleButton t && c is not RadioButton) t.IsChecked = t.IsChecked != true;
                        else if (c is RadioButton r) r.IsChecked = true;
                        if (c is ButtonBase) c.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                        break;
                    case "text": Part<TextBox>(a).Text = b; break;
                    case "pick": FolderPicker.Answers.Enqueue(a + (b.Length > 0 ? " " + b : "")); break;
                    case "select": SaveList.SelectedIndex = int.Parse(a); break;
                    case "sleep": await Task.Delay(int.Parse(a)); break;
                    case "shot": await Shot(scriptShots!, a, 700); break;
                    case "frames":   // frames <name> <count> <ms apart>: what is on screen now and at each step after (to look at motion)
                    case "crest":    // crest <name> <count> <ms apart>: the same, but only the crest and its surroundings, at full size
                        var f = b.Split(' ');
                        var run = new List<(double at, BitmapSource picture)>();
                        var watch = System.Diagnostics.Stopwatch.StartNew();
                        for (var n = 0; n < int.Parse(f[0]); n++)
                        {
                            if (n > 0) await Task.Delay(int.Parse(f[1]));
                            BitmapSource now = cut[0] == "frames" ? Picture(0.75) : new CroppedBitmap(Picture(1.5), new Int32Rect(960, 144, 615, 576));
                            now.Freeze();
                            run.Add((watch.Elapsed.TotalMilliseconds, now));
                        }
                        // (saved afterwards: writing a picture takes longer than the motion lasts)
                        for (var n = 0; n < run.Count; n++) SavePicture(run[n].picture, scriptShots!, a + "-" + n.ToString("00") + "-" + (int)run[n].at + "ms");
                        break;
                    case "sheen":   // sheen <main button> <x>: the light that crosses a gold button under the mouse, held at x (no mouse here)
                        var gold = Part<Button>(a);
                        gold.ApplyTemplate();
                        if (gold.Template.FindName("Sheen", gold) is not FrameworkElement light) throw new InvalidOperationException(a + " has no sheen");
                        light.Opacity = 1;
                        light.RenderTransform = new TranslateTransform(double.Parse(b, System.Globalization.CultureInfo.InvariantCulture), 0);
                        break;
                    case "expect":   // expect <part> <words it must contain>
                        var words = Words(Part<FrameworkElement>(a));
                        if (words.IndexOf(b, StringComparison.OrdinalIgnoreCase) < 0) throw new InvalidOperationException(a + " says \"" + words + "\", not \"" + b + "\"");
                        break;
                    case "exit":
                        ScriptLog("PASS  exit");
                        closing = true;
                        Window.Close();
                        return;
                    default: throw new InvalidOperationException("unknown step");
                }
                if (waits) File.WriteAllText(place, (at + 1).ToString());
                ScriptLog("ok    " + line);
            }
            ScriptLog("PASS  end of script");
        }
        catch (Exception e)
        {
            ScriptLog("FAIL  " + (at < steps.Length ? steps[at] : "") + ": " + e.Message + "  [page " + (shown?.Name ?? "none") + "]");
            try { await Shot(scriptShots!, "FAIL-step-" + at, 100); } catch (Exception) { }
            File.WriteAllText(place, steps.Length.ToString());
            runner?.Cancel();
            Environment.Exit(4);
        }
    }

    async Task Until(Func<bool> done, string seconds, string line)
    {
        var limit = DateTime.UtcNow.AddSeconds(double.TryParse(seconds, out var s) ? s : 30);
        while (!done())
        {
            if (DateTime.UtcNow > limit) throw new TimeoutException("waited too long");
            await Task.Delay(100);
        }
    }

    static string Words(FrameworkElement e) => e switch
    {
        TextBlock t => t.Text.Replace("\u200A", ""),
        TextBox t => t.Text,
        ContentControl c => c.Content as string ?? "",
        _ => "",
    };
}
