// The welcome-back page (every start: play, or the update first, merged into the game).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Shapes;

namespace TriforceSetup;

sealed partial class Shell
{
    Grid HomePage = null!;
    TextBlock HomeTitle = null!, HomeNote = null!, GameLine = null!, GamePath = null!, NewsLine = null!, NewsDetail = null!, MergeNote = null!;
    Caps HomeEyebrow = null!;
    Rectangle GameMark = null!, SavesMark = null!, NewsMark = null!;
    Button NotNowButton = null!, HomeGoButton = null!;
    ToggleButton NewFolderLink = null!;
    Action? homeGo;

    void InitHome()
    {
        HomePage = Part<Grid>("HomePage");
        HomeTitle = Part<TextBlock>("HomeTitle"); HomeNote = Part<TextBlock>("HomeNote"); GameLine = Part<TextBlock>("GameLine");
        GamePath = Part<TextBlock>("GamePath"); NewsLine = Part<TextBlock>("NewsLine"); NewsDetail = Part<TextBlock>("NewsDetail");
        MergeNote = Part<TextBlock>("MergeNote");
        HomeEyebrow = Part<Caps>("HomeEyebrow");
        GameMark = Part<Rectangle>("GameMark"); SavesMark = Part<Rectangle>("SavesMark"); NewsMark = Part<Rectangle>("NewsMark");
        NotNowButton = Part<Button>("NotNowButton"); HomeGoButton = Part<Button>("HomeGoButton");
        NewFolderLink = Part<ToggleButton>("NewFolderLink");

        NotNowButton.Click += (_, _) => { Program.Log("update: not now"); AfterHome(); };
        HomeGoButton.Click += (_, _) => homeGo?.Invoke();
        NewFolderLink.Click += (_, _) => { NewFolderLink.IsChecked = false; ShowSetup(); };
        InitSaves();
    }

    // ---- welcome back ----

    Exception? homeOffline;

    /// The game folder, the saves and the newest release, and the choice: Play, or Update & merge first
    void ShowHome(Exception? offline)
    {
        homeOffline = offline;
        var folder = Install.Get("GameFolder");
        var hasGame = folder != null && GameExe(folder) != null;
        var tools = Tools.InstalledVersion;
        var built = hasGame ? BuiltVersion(folder!) : null;
        Program.Log("home: GameFolder=[" + (folder ?? "null") + "] exe=[" + (folder != null ? GameExe(folder) ?? "null" : "-") + "] built=[" + (built ?? "null") + "] tools=[" + (tools ?? "null") + "] latest=[" + (latest?.Tag ?? "null") + "]");

        var setupNewer = latest != null && Program.Version != "dev" && latest.Tag != Program.Version && latest.SetupUrl != null;
        var toolsNewer = latest != null && latest.Tag != tools;
        var gameBehind = hasGame && built != null && tools != null && built != tools;   // (new tools already here, game not rebuilt yet)
        var update = setupNewer || toolsNewer || gameBehind;
        var target = latest?.Tag ?? tools ?? "";

        HomeFill(hasGame, hasGame ? "Triforce " + (built ?? tools ?? "?") : null, folder, built, target, update, offline);
        ShowSavesLine();
        Show(HomePage);
        if (!update) stage.Gleam(450);

        if (update)
            homeGo = () =>
            {
                Program.Log("update: merge " + target);
                if (latest != null && (setupNewer || toolsNewer)) { merge = true; _ = Merge(); }
                else if (hasGame && Tools.Find() is { } t) { OutPath.Text = folder!; BeginUpdate(); Run(t, new List<string> { "--update", folder! }, keepRows: true); }
            };
        else homeGo = AfterHome;
    }

    /// The page's words for one state of things (also what --snapshot shows, with sample values)
    void HomeFill(bool hasGame, string? game, string? folder, string? built, string target, bool update, Exception? offline)
    {
        GameMark.Fill = Brush(hasGame ? "Gold" : "TextFaint");
        GameLine.Text = hasGame ? game : "No game built yet";
        GamePath.Text = hasGame ? folder : "Build one from your own copy of the game.";

        if (offline != null)
        {
            var none = offline.Message.Contains("no Triforce release");
            NewsMark.Fill = Brush("TextFaint");
            NewsLine.Text = none ? "No release is published yet" : "Couldn't check for updates";
            NewsDetail.Text = (none ? "" : "GitHub could not be reached. ") + (hasGame ? "You can play what you have." : "You can build with the tools you have.");
            NewsDetail.Foreground = Brush("TextDim");
        }
        else if (update)
        {
            NewsMark.Fill = Brush("Gold300");
            NewsLine.Text = "Triforce " + target + " is ready";
            NewsDetail.Text = hasGame && built != null ? "Your game is " + built + "." : "Newer than the tools you have.";
            NewsDetail.Foreground = Brush("Gold300");
        }
        else
        {
            NewsMark.Fill = Brush("Good");
            NewsLine.Text = "Triforce " + target;
            NewsDetail.Text = "You're up to date.";
            NewsDetail.Foreground = Brush("Good");
        }

        HomeEyebrow.Label = update ? "A new version" : hasGame ? "IV · Play" : "III · Forge";
        HomeEyebrow.Foreground = Brush(update ? "Gold300" : "Gold");
        HomeTitle.Text = update ? "A new Triforce awaits" : "Welcome back, hero.";
        HomeNote.Text = update ? "Merge the new version into your game now, or keep playing the one you have."
                      : !hasGame ? "The tools are ready. Build your game from your own copy and it will be waiting here."
                      : offline != null ? "Ready when you are."
                      : "Everything is in order. Settings and controls now live in the game's own menu.";
        if (update)
        {
            MergeNote.Text = hasGame
                ? "Update & merge downloads Triforce " + target + " and builds your game folder again with it, from the game files it keeps. Only what changed is compiled again, so it usually takes a few minutes. Your saves and settings stay."
                : "Update downloads Triforce " + target + "'s tools. Not now keeps the ones you have.";
            HomeGoButton.Content = hasGame ? "Update & merge" : "Update";
            NotNowButton.Content = hasGame ? "Play as it is" : "Not now";
            NotNowButton.Visibility = Visibility.Visible;
        }
        else
        {
            MergeNote.Text = "";
            HomeGoButton.Content = hasGame ? "Play" : "Build my game";
            NotNowButton.Visibility = Visibility.Collapsed;
        }
        NewFolderLink.Visibility = hasGame ? Visibility.Visible : Visibility.Collapsed;
    }
}
