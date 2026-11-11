// The Convert saves page: an emulator's saves into Triforce, or Triforce's out to an emulator (Saves.cs).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;

namespace TriforceSetup;

sealed partial class Shell
{
    Grid SavesPage = null!;
    RadioButton SavesIn = null!, SavesOut = null!;
    Caps SavesListLabel = null!;
    TextBlock SavesInfo = null!, SavesTarget = null!, SavesLine = null!, SavesDetail = null!;
    Button SavesRescan = null!, SavesBrowse = null!, SavesBack = null!, SavesGo = null!, ConvertSavesButton = null!;
    ListBox SaveList = null!;
    List<SaveSet> found = new();
    SaveSet? chosenFolder;   // (one picked with Choose a folder)
    bool savesBusy;

    void InitSaves()
    {
        SavesPage = Part<Grid>("SavesPage");
        SavesIn = Part<RadioButton>("SavesIn"); SavesOut = Part<RadioButton>("SavesOut");
        SavesListLabel = Part<Caps>("SavesListLabel"); SavesInfo = Part<TextBlock>("SavesInfo"); SavesTarget = Part<TextBlock>("SavesTarget");
        SavesLine = Part<TextBlock>("SavesLine"); SavesDetail = Part<TextBlock>("SavesDetail");
        SavesRescan = Part<Button>("SavesRescan"); SavesBrowse = Part<Button>("SavesBrowse"); SavesBack = Part<Button>("SavesBack");
        SavesGo = Part<Button>("SavesGo"); ConvertSavesButton = Part<Button>("ConvertSavesButton");
        SaveList = Part<ListBox>("SaveList");

        ConvertSavesButton.Click += (_, _) => ShowSaves();
        SavesIn.Checked += (_, _) => SavesWayChanged();
        SavesOut.Checked += (_, _) => SavesWayChanged();
        SavesRescan.Click += (_, _) => { chosenFolder = null; _ = ScanSaves(); };
        SavesBrowse.Click += (_, _) => BrowseSaves();
        SavesBack.Click += (_, _) => ShowHome(homeOffline);
        SavesGo.Click += (_, _) => ConvertNow();
        SaveList.SelectionChanged += (_, _) => SavesRefresh();
    }

    static string? GameFolder() => Install.Get("GameFolder") is { } g && Directory.Exists(g) ? g : null;

    /// The line on the welcome-back page
    void ShowSavesLine()
    {
        var game = GameFolder();
        if (game == null) { SavesLineFill(null, "None yet", "They can come in from an emulator once your game is built."); return; }
        var t = Saves.Describe("Triforce", Saves.TriforceFolder(game));
        if (t.Empty) SavesLineFill(false, "No saves yet", "Bring them in, or start a new game.");
        else SavesLineFill(true, t.Slots + " save slot" + (t.Slots == 1 ? "" : "s"), "Last played " + t.Last.ToString("d MMM yyyy, HH:mm"));
    }

    void SavesLineFill(bool? have, string text, string detail)
    {
        SavesLine.Text = text;
        SavesDetail.Text = detail;
        SavesLine.Foreground = Brush(have == true ? "TextHi" : "Text");
        SavesMark.Fill = Brush(have == true ? "Gold" : "TextFaint");
        ConvertSavesButton.Visibility = have == null ? Visibility.Collapsed : Visibility.Visible;
    }

    void ShowSaves()
    {
        Show(SavesPage);
        chosenFolder = null;
        SavesWayChanged();
    }

    void SavesWayChanged()
    {
        if (SavesPage.Visibility != Visibility.Visible) return;
        chosenFolder = null;
        SavesListLabel.Label = SavesIn.IsChecked == true ? "Take the saves from" : "Put Triforce's saves into";
        _ = ScanSaves();
    }

    async Task ScanSaves()
    {
        SaveList.ItemsSource = null;
        SavesInfo.Text = "Looking for saves of Ocarina of Time on this PC...";
        SavesInfo.Foreground = Brush("Text");
        SavesGo.IsEnabled = false;
        found = await Task.Run(() => Saves.FindEmulatorSaves());
        ShowSaveList();
    }

    void ShowSaveList()
    {
        var items = new List<SaveSet>(found);
        if (chosenFolder != null && items.All(s => !string.Equals(s.Folder, chosenFolder.Folder, StringComparison.OrdinalIgnoreCase))) items.Insert(0, chosenFolder);
        SaveList.ItemsSource = items.Select(Item).ToList();
        if (items.Count > 0) SaveList.SelectedIndex = 0;
        SavesRefresh();
    }

    /// One place saves were found: its name, what it holds on the right, its folder under it
    FrameworkElement Item(SaveSet s)
    {
        var g = new Grid { Tag = s };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.RowDefinitions.Add(new RowDefinition());
        g.RowDefinitions.Add(new RowDefinition());
        var name = new TextBlock { Text = s.Name, Foreground = Brush("TextHi"), FontSize = 14, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 0, 16, 0) };
        var what = new TextBlock
        {
            Text = s.Empty ? "No saves here yet" : s.Slots + " slot" + (s.Slots == 1 ? "" : "s") + "  ·  last played " + s.Last.ToString("d MMM yyyy, HH:mm"),
            Foreground = Brush(s.Empty ? "TextDim" : "Gold"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center,
        };
        var where = new TextBlock { Text = s.Folder, Foreground = Brush("TextFaint"), FontSize = 11.5, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 3, 0, 0) };
        Grid.SetColumn(what, 1);
        Grid.SetRow(where, 1);
        Grid.SetColumnSpan(where, 2);
        g.Children.Add(name);
        g.Children.Add(what);
        g.Children.Add(where);
        return g;
    }

    SaveSet? Picked => (SaveList.SelectedItem as FrameworkElement)?.Tag as SaveSet;

    /// What Convert will do, said before it is done
    void SavesRefresh()
    {
        if (savesBusy) return;
        var game = GameFolder();
        var into = SavesIn.IsChecked == true;
        var pick = Picked;
        SavesGo.IsEnabled = false;
        SavesInfo.Foreground = Brush("Text");
        SavesTarget.Text = "";
        if (game == null)
        {
            SavesInfo.Text = "Build your game first: Triforce's saves live in its game folder.";
            return;
        }
        var tri = Saves.Describe("Triforce", Saves.TriforceFolder(game));
        if (!into && tri.Empty)
        {
            SavesInfo.Text = "Triforce has no saves yet to take out.";
            return;
        }
        if (pick == null)
        {
            SavesInfo.Text = (found.Count == 0 ? "No emulator saves of this game were found. " : "") +
                             (into ? "Use Choose a folder to point at the saves (the folder with option.sav and slot_00 to slot_05)."
                                   : "Use Choose a folder to point at the emulator's save folder for this game (0400486024DEC000).");
            return;
        }
        if (into)
        {
            if (pick.Empty) { SavesInfo.Text = "That folder has no saves to bring in."; return; }
            SavesInfo.Text = "Triforce's saves become " + pick.Name + "'s (" + pick.Slots + " slots, last played " + pick.Last.ToString("d MMM yyyy") + ")." +
                             (tri.Empty ? "" : " Triforce's current saves (" + tri.Slots + " slots, last played " + tri.Last.ToString("d MMM yyyy") + ") are backed up first.");
            SavesTarget.Text = "Into " + tri.Folder;
        }
        else
        {
            SavesInfo.Text = pick.Name + "'s saves become Triforce's (" + tri.Slots + " slots, last played " + tri.Last.ToString("d MMM yyyy") + ")." +
                             (pick.Empty ? "" : " Its current saves (" + pick.Slots + " slots, last played " + pick.Last.ToString("d MMM yyyy") + ") are backed up first.");
            SavesTarget.Text = "Into " + pick.Folder;
        }
        SavesGo.IsEnabled = true;
    }

    void BrowseSaves()
    {
        var into = SavesIn.IsChecked == true;
        var dir = FolderPicker.Pick(Window, into ? "Choose the folder with the saves" : "Choose the emulator's save folder for Ocarina of Time", null);
        if (dir == null) return;
        var set = Saves.FindIn(dir);
        if (set == null && into)
        {
            SavesInfo.Text = "No saves of this game were found in " + dir + ". Choose the folder that holds option.sav and slot_00 to slot_05.";
            SavesInfo.Foreground = Brush("Bad");
            return;
        }
        // (out of Triforce: a folder with no saves yet is fine, the saves go straight into it)
        chosenFolder = Saves.Describe(System.IO.Path.GetFileName(dir.TrimEnd('\\')) + " (chosen)", set ?? dir);
        ShowSaveList();
    }

    async void ConvertNow()
    {
        var game = GameFolder();
        var pick = Picked;
        if (game == null || pick == null) return;
        if (Saves.RunningGame() is { } running)
        {
            SavesInfo.Text = "Close " + running + " first: it would write its own saves back over the copy when it closes.";
            SavesInfo.Foreground = Brush("Bad");
            return;
        }
        var into = SavesIn.IsChecked == true;
        var tri = Saves.TriforceFolder(game);
        var backups = System.IO.Path.Combine(Install.InstalledDir ?? Install.SelfDir, "save-backups");
        savesBusy = true;
        SavesGo.IsEnabled = false;
        SavesInfo.Text = "Copying...";
        try
        {
            var from = into ? pick.Folder : tri;
            var to = into ? tri : pick.Folder;
            var label = into ? "Triforce before " + pick.Name : pick.Name + " before Triforce";
            var backup = await Task.Run(() => Saves.Copy(from, to, backups, label));
            var now = Saves.Describe("", to);
            Program.Log("saves: " + from + " -> " + to + (backup != null ? " (backup " + backup + ")" : ""));
            savesBusy = false;
            await ScanSaves();
            SavesInfo.Text = "Done: " + now.Slots + " slot" + (now.Slots == 1 ? "" : "s") + " copied " + (into ? "into Triforce" : "into " + pick.Name) + "." +
                             (backup != null ? " The saves that were there are kept in " + backup + "." : "");
            SavesInfo.Foreground = Brush("Good");
            SavesGo.IsEnabled = false;
        }
        catch (Exception e)
        {
            savesBusy = false;
            Program.Log("saves failed: " + e.Message);
            SavesInfo.Text = "The saves could not be copied: " + e.Message;
            SavesInfo.Foreground = Brush("Bad");
        }
        finally { savesBusy = false; }
    }
}
