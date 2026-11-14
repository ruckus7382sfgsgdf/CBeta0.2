// Triforce Setup: the one file players download. It installs itself (per user, no admin), keeps itself and the Triforce tools up to
// date from the releases at triforceproject.com (triforce/downloads on its Forgejo), turns the player's own game files into a game folder (nx2/make_game.py from
// the tools) and starts the game. The game's own update offer (host/hos_update.c) starts it again with --update <folder>.
//
// Built for .NET Framework 4.8 (part of Windows 10 and 11): nothing to install, one small exe. setup/build.ps1 compiles it.
// Release assets it looks for: TriforceSetup.exe (this app) and Triforce-Tools.zip (a "tools" folder: nx2, host, gpu, python, ...).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace TriforceSetup;

static class Program
{
    // (11-13: Triforce's own home. Releases are published in triforce/downloads on its Forgejo; the list is asked on the main
    // domain, which passes /api/v1 on, so the address outlives whatever answers behind it. Forgejo's list has GitHub's
    // names (tag_name, assets[].name / browser_download_url / size) and takes limit= where GitHub took per_page=.
    // Up to v0.3.5 this was api.github.com/repos/ruckus7382sfgsgdf/CBeta0.2: those builds ask there and nowhere else)
    public const string Home = "https://triforceproject.com";
    public const string ReleaseApi = Home + "/api/v1/repos/triforce/downloads/releases?limit=1";
    public const string SetupAsset = "TriforceSetup.exe";
    public const string ToolsAsset = "Triforce-Tools.zip";
    public const string AppName = "Triforce";

    [STAThread]
    static int Main(string[] args)
    {
        try { ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12; } catch (Exception) { }
        Install.NoWrites = args.Contains("--snapshot");
        if (args.Contains("--uninstall")) return Install.Uninstall(args.Contains("--quiet")) ? 0 : 1;
        // (silent install: --install-to <folder> [--no-desktop] [--no-startmenu]; the installed Setup then starts as usual)
        var to = Array.IndexOf(args, "--install-to");
        if (to >= 0 && to + 1 < args.Length)
        {
            var exe = Install.Run(Path.GetFullPath(args[to + 1]), !args.Contains("--no-desktop"), !args.Contains("--no-startmenu"));
            Log("installed " + Version + " to " + Path.GetDirectoryName(exe) + " (silent)");
            Process.Start(new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! });
            return 0;
        }
        if (args.Contains("--where")) { Log(Shell.Where()); return 0; }
        // (testing: --release-test asks GitHub (or TRIFORCE_RELEASE_API) what Setup would, and writes what it found to setup.log)
        if (args.Contains("--release-test"))
        {
            try
            {
                using var http = new HttpClient();
                http.DefaultRequestHeaders.UserAgent.ParseAdd("TriforceSetup/" + Version);
                var rel = Release.Latest(http, CancellationToken.None).Result;
                Log("release-test: tag " + rel.Tag + ", setup " + (rel.SetupUrl ?? "MISSING") + " (" + rel.SetupSize + "), tools " + (rel.ToolsUrl ?? "MISSING") + " (" + rel.ToolsSize + ")");
                return rel.SetupUrl != null && rel.ToolsUrl != null ? 0 : 1;
            }
            catch (Exception e) { Log("release-test failed: " + (e.InnerException ?? e).Message); return 2; }
        }
        // (testing: --convert-saves <from> <to> copies one save folder over another as the Convert saves page does)
        var cv = Array.IndexOf(args, "--convert-saves");
        if (cv >= 0 && cv + 2 < args.Length)
        {
            var b = Saves.Copy(args[cv + 1], args[cv + 2], Path.Combine(Install.SelfDir, "save-backups"), "test");
            Log("convert-saves " + args[cv + 1] + " -> " + args[cv + 2] + ", backup " + (b ?? "none"));
            return 0;
        }
        // (testing: --check-game <.nup | exefs folder> writes the version check's answer to setup.log)
        var cg = Array.IndexOf(args, "--check-game");
        if (cg >= 0 && cg + 1 < args.Length)
        {
            var tools = Tools.Find();
            if (tools == null) { Log("check-game: no tools"); return 2; }
            var path = args[cg + 1];
            var (ok, text) = Runner.Check(tools, new[] { "--check", path.EndsWith(".nup", StringComparison.OrdinalIgnoreCase) ? "--nup" : "--exefs", path }).Result;
            Log("check-game " + path + ": " + (ok ? "OK " : "BAD ") + text + " (tools " + tools.Root + ", python " + tools.Python + ")");
            return ok ? 0 : 1;
        }
        // (testing without a screen, --snapshot or TRIFORCE_UI_SCRIPT: a failure is written down, never shown in a message box)
        var shots = Array.IndexOf(args, "--snapshot");
        var unattended = shots >= 0 && shots + 1 < args.Length ? args[shots + 1]
            : Environment.GetEnvironmentVariable("TRIFORCE_UI_SCRIPT") is { Length: > 0 } ? Environment.GetEnvironmentVariable("TRIFORCE_UI_SHOTS") ?? Install.SelfDir : null;
        void Report(Exception e)
        {
            try { Directory.CreateDirectory(unattended!); File.AppendAllText(Path.Combine(unattended!, "error.txt"), e + Environment.NewLine); } catch (Exception) { }
        }
        var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
        app.DispatcherUnhandledException += (_, e) =>
        {
            e.Handled = true;
            if (unattended != null) { Report(e.Exception); Environment.Exit(3); }
            MessageBox.Show("Triforce Setup ran into a problem:\n\n" + e.Exception.Message, "Triforce Setup", MessageBoxButton.OK, MessageBoxImage.Error);
        };
        Shell w;
        try { w = new Shell(args); }
        catch (Exception e) when (unattended != null) { Report(e); return 3; }
        return app.Run(w.Window);
    }

    /// This build's release tag (setup/build.ps1 -Version), "dev" for a local build
    public static string Version =>
        typeof(Program).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "dev";

    /// setup.log beside the installed Setup: what it checked, downloaded and ran (for bug reports)
    public static void Log(string line)
    {
        if (Install.NoWrites) return;
        try
        {
            var file = Path.Combine(Install.InstalledDir ?? Install.SelfDir, "setup.log");
            if (File.Exists(file) && new FileInfo(file).Length > (2 << 20)) File.Delete(file);
            File.AppendAllText(file, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + line + Environment.NewLine);
        }
        catch (Exception) { }
    }

    public static Stream Resource(string name) => typeof(Program).Assembly.GetManifestResourceStream(name)
        ?? throw new FileNotFoundException("missing resource " + name);

    public static BitmapImage Image(string name)
    {
        var b = new BitmapImage();
        b.BeginInit();
        b.StreamSource = Resource(name);
        b.CacheOption = BitmapCacheOption.OnLoad;
        b.EndInit();
        b.Freeze();
        return b;
    }
}

/// Where Triforce lives on this PC, and the pieces of a per-user install (shortcuts, the Apps list entry).
static class Install
{
    const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\Triforce";
    const string SettingsKey = @"Software\Triforce";

    /// (testing: TRIFORCE_TEST_ROOT=<folder> keeps a whole install inside that folder. What would go to the registry is a text
    /// file there, the shortcuts go to its Desktop and Start Menu folders, the build's work folder is its build folder and emulator
    /// saves are looked for in its Roaming folder. The PC's real install is neither read nor touched.)
    public static readonly string? TestRoot = Environment.GetEnvironmentVariable("TRIFORCE_TEST_ROOT") is { Length: > 0 } t ? Path.GetFullPath(t) : null;
    static string TestStore => Path.Combine(TestRoot!, "registry.txt");

    static string? Read(string key, string name)
    {
        if (TestRoot == null)
        {
            using var k = Registry.CurrentUser.OpenSubKey(key);
            return k?.GetValue(name) as string;
        }
        var start = key + "\\" + name + "=";
        return File.Exists(TestStore) ? File.ReadAllLines(TestStore).LastOrDefault(l => l.StartsWith(start))?.Substring(start.Length) : null;
    }

    static void Write(string key, string name, object value)
    {
        if (NoWrites) return;
        if (TestRoot == null)
        {
            using var k = Registry.CurrentUser.CreateSubKey(key);
            if (value is int n) k.SetValue(name, n, RegistryValueKind.DWord); else k.SetValue(name, value);
            return;
        }
        Directory.CreateDirectory(TestRoot);
        var start = key + "\\" + name + "=";
        var lines = File.Exists(TestStore) ? File.ReadAllLines(TestStore).Where(l => !l.StartsWith(start)).ToList() : new List<string>();
        lines.Add(start + value);
        File.WriteAllLines(TestStore, lines);
    }

    static void Remove(string key)
    {
        if (TestRoot == null) { try { Registry.CurrentUser.DeleteSubKeyTree(key, false); } catch (Exception) { } return; }
        if (File.Exists(TestStore)) File.WriteAllLines(TestStore, File.ReadAllLines(TestStore).Where(l => !l.StartsWith(key + "\\")).ToList());
    }

    /// May this run write there? Always, except in a test run (TRIFORCE_TEST_ROOT), which installs, builds, copies saves and
    /// starts games only inside its own folder, whatever folder is typed or picked: so a look at Setup cannot change the PC's
    /// real install, real game or real saves
    public static bool Allowed(string path)
    {
        if (TestRoot == null) return true;
        try { return (Path.GetFullPath(path).TrimEnd('\\') + "\\").StartsWith(TestRoot.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase); }
        catch (Exception) { return false; }
    }
    public static string TestOnly => "This is a test run of Setup: it only writes inside its own folder, " + TestRoot + ".";

    static string LocalData => TestRoot ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
    public static string DefaultDir => Path.Combine(LocalData, "Programs", "Triforce");
    public static string Self => Assembly.GetExecutingAssembly().Location;
    public static string SelfDir => Path.GetDirectoryName(Self)!;

    /// The install folder from the Apps list entry, when Triforce is installed
    public static string? InstalledDir
    {
        get
        {
            var d = Read(UninstallKey, "InstallLocation");
            return d != null && File.Exists(Path.Combine(d, Program.SetupAsset)) ? d : null;
        }
    }

    public static bool RunningInstalled =>
        InstalledDir is { } d && string.Equals(Path.GetFullPath(d).TrimEnd('\\'), Path.GetFullPath(SelfDir).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);

    // ---- small settings (HKCU\Software\Triforce) ----
    public static string? Get(string name) => Read(SettingsKey, name);
    /// (--snapshot: pictures of the pages only; nothing is written to the registry or to setup.log)
    public static bool NoWrites;

    public static void Set(string name, string value) => Write(SettingsKey, name, value);

    /// Copies this exe into the folder, registers it in the Apps list and makes the shortcuts. Returns the installed exe.
    public static string Run(string dir, bool desktop, bool startMenu)
    {
        if (!Allowed(dir)) throw new InvalidOperationException(TestOnly);
        Directory.CreateDirectory(dir);
        var exe = Path.Combine(dir, Program.SetupAsset);
        if (!string.Equals(Path.GetFullPath(exe), Path.GetFullPath(Self), StringComparison.OrdinalIgnoreCase))
            File.Copy(Self, exe, true);
        Write(UninstallKey, "DisplayName", Program.AppName);
        Write(UninstallKey, "DisplayIcon", exe + ",0");
        Write(UninstallKey, "DisplayVersion", Program.Version);
        Write(UninstallKey, "Publisher", "Triforce");
        Write(UninstallKey, "InstallLocation", dir);
        Write(UninstallKey, "UninstallString", "\"" + exe + "\" --uninstall");
        Write(UninstallKey, "QuietUninstallString", "\"" + exe + "\" --uninstall --quiet");
        Write(UninstallKey, "URLInfoAbout", Program.Home);
        Write(UninstallKey, "NoModify", 1);
        Write(UninstallKey, "NoRepair", 1);
        Set("Desktop", desktop ? "1" : "0");
        Set("StartMenu", startMenu ? "1" : "0");
        // ("Triforce" opens Setup, not the game: every start goes through the update question first, then Play starts the game)
        try
        {
            if (startMenu) Shortcut(Path.Combine(StartFolder, "Triforce.lnk"), exe, "", dir, exe, "Play Triforce");
            if (desktop) Shortcut(DesktopLink, exe, "", dir, exe, "Play Triforce");
        }
        catch (Exception e) { Program.Log("shortcuts: " + e.Message); }
        return exe;
    }

    static string StartFolder => Path.Combine(TestRoot != null ? Path.Combine(TestRoot, "Start Menu") : Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Triforce");
    static string DesktopLink => Path.Combine(TestRoot != null ? Path.Combine(TestRoot, "Desktop") : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Triforce.lnk");

    static void Shortcut(string link, string target, string args, string workDir, string icon, string description)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(link)!);
        var type = Type.GetTypeFromProgID("WScript.Shell")!;
        var shell = Activator.CreateInstance(type)!;
        try
        {
            var lnk = type.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { link })!;
            var t = lnk.GetType();
            t.InvokeMember("TargetPath", BindingFlags.SetProperty, null, lnk, new object[] { target });
            t.InvokeMember("Arguments", BindingFlags.SetProperty, null, lnk, new object[] { args });
            t.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, lnk, new object[] { workDir });
            t.InvokeMember("IconLocation", BindingFlags.SetProperty, null, lnk, new object[] { icon + ",0" });
            t.InvokeMember("Description", BindingFlags.SetProperty, null, lnk, new object[] { description });
            t.InvokeMember("Save", BindingFlags.InvokeMethod, null, lnk, null);
            Marshal.FinalReleaseComObject(lnk);
        }
        finally { Marshal.FinalReleaseComObject(shell); }
    }

    /// Removes Setup, its tools and the shortcuts. Game folders and saves are the player's and stay.
    public static bool Uninstall(bool quiet)
    {
        var dir = InstalledDir ?? SelfDir;
        if (!quiet && MessageBox.Show("Remove Triforce Setup and its tools from this PC?\n\nYour game folders and saves are not touched.",
                "Uninstall Triforce", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return false;
        foreach (var f in new[] { DesktopLink })
            try { if (File.Exists(f)) File.Delete(f); } catch (Exception) { }
        try { if (Directory.Exists(StartFolder)) Directory.Delete(StartFolder, true); } catch (Exception) { }
        // (the build's work folder and compile cache, nx2/make_game.py --work: several GB, made again by a build)
        var work = TestRoot != null ? Path.Combine(TestRoot, "build") : Path.Combine(LocalData, "Triforce", "build");
        try { if (Directory.Exists(work)) Directory.Delete(work, true); } catch (Exception) { }
        Remove(UninstallKey);
        Remove(SettingsKey);
        // (this exe is running from the folder: a short-lived cmd removes it once we are gone)
        if (File.Exists(Path.Combine(dir, Program.SetupAsset)))
            Process.Start(new ProcessStartInfo("cmd.exe", "/c timeout /t 2 /nobreak >nul & rmdir /s /q \"" + dir + "\"")
            { CreateNoWindow = true, UseShellExecute = false, WorkingDirectory = Path.GetTempPath() });
        if (!quiet) MessageBox.Show("Triforce has been removed.", "Uninstall Triforce", MessageBoxButton.OK, MessageBoxImage.Information);
        return true;
    }
}

/// What a GitHub release offers
sealed class Release
{
    public string Tag = "";
    public string? SetupUrl, ToolsUrl;
    public long SetupSize, ToolsSize;

    /// The latest release (TRIFORCE_RELEASE_API overrides the address, for testing against a local copy)
    public static async Task<Release> Latest(HttpClient http, CancellationToken token)
    {
        var url = Environment.GetEnvironmentVariable("TRIFORCE_RELEASE_API") is { Length: > 0 } o ? o
            : Program.ReleaseApi;   // (the newest, pre-releases too)
        using var ask = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        using var both = CancellationTokenSource.CreateLinkedTokenSource(token, ask.Token);
        using var response = await http.GetAsync(url, both.Token);
        if (response.StatusCode == HttpStatusCode.NotFound) throw new InvalidOperationException("There is no Triforce release yet.");
        response.EnsureSuccessStatusCode();
        var r = Parse(await response.Content.ReadAsStringAsync());
        if (r.Tag.Length == 0) throw new InvalidOperationException("There is no Triforce release yet.");   // (an empty list: nothing published)
        return r;
    }

    /// The release list's answer (Forgejo's, GitHub's before: the same names), read with a real JSON parser: a list of releases (the newest first) or one release. Each asset's
    /// object holds a nested "uploader" object, so nothing simpler than a parser reads it right (v0.1.0 found no assets)
    public static Release Parse(string json)
    {
        var parsed = new System.Web.Script.Serialization.JavaScriptSerializer { MaxJsonLength = int.MaxValue }.DeserializeObject(json);
        var release = parsed is object[] list ? (list.Length > 0 ? list[0] as Dictionary<string, object> : null) : parsed as Dictionary<string, object>;
        var r = new Release();
        if (release == null) return r;
        r.Tag = release.TryGetValue("tag_name", out var tag) ? tag as string ?? "" : "";
        if (release.TryGetValue("assets", out var assets) && assets is object[] files)
            foreach (var f in files.OfType<Dictionary<string, object>>())
            {
                var name = f.TryGetValue("name", out var n) ? n as string : null;
                var link = f.TryGetValue("browser_download_url", out var l) ? l as string : null;
                var size = f.TryGetValue("size", out var s) && s != null ? Convert.ToInt64(s) : 0;
                if (string.Equals(name, Program.SetupAsset, StringComparison.OrdinalIgnoreCase)) { r.SetupUrl = link; r.SetupSize = size; }
                if (string.Equals(name, Program.ToolsAsset, StringComparison.OrdinalIgnoreCase)) { r.ToolsUrl = link; r.ToolsSize = size; }
            }
        return r;
    }

    /// A file downloaded with progress (0..1) into place (a .part file until complete)
    public static async Task Download(HttpClient http, string url, string file, long size, Action<double, long> progress, CancellationToken token)
    {
        var part = file + ".part";
        using (var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token))
        {
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? size;
            using var from = await response.Content.ReadAsStreamAsync();
            using var to = File.Create(part);
            var buffer = new byte[1 << 20];
            long have = 0;
            var shown = DateTime.MinValue;
            for (int n; (n = await from.ReadAsync(buffer, 0, buffer.Length, token)) > 0;)
            {
                await to.WriteAsync(buffer, 0, n, token);
                have += n;
                if (DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(150))
                {
                    shown = DateTime.UtcNow;
                    progress(total > 0 ? Math.Min(1.0, (double)have / total) : 0, have);
                }
            }
            if (total > 0 && have != total) throw new IOException("The download ended early (" + (have >> 20) + " of " + (total >> 20) + " MB).");
            progress(1, have);
        }
        if (File.Exists(file)) File.Delete(file);
        File.Move(part, file);
    }
}

/// The Triforce tools beside the installed Setup: nx2/make_game.py and its own Python.
sealed class Tools
{
    public readonly string Root, Python;
    Tools(string root, string python) { Root = root; Python = python; }

    public static string Folder => Path.Combine(Install.InstalledDir ?? Install.SelfDir, "tools");

    /// The release these tools came from (tools/version.txt), null when there are none
    public static string? InstalledVersion
    {
        get
        {
            var v = Path.Combine(Folder, "version.txt");
            return File.Exists(v) && File.Exists(Path.Combine(Folder, "nx2", "make_game.py")) ? File.ReadAllText(v).Trim() : null;
        }
    }

    public static Tools? Find()
    {
        var bundled = Folder;
        if (File.Exists(Path.Combine(bundled, "nx2", "make_game.py")))
        {
            var py = Path.Combine(bundled, "python", "python.exe");
            return new Tools(bundled, File.Exists(py) ? py : "python");
        }
        // (a development copy: Setup run from inside a checkout uses its sources and the PC's Python. Never in a test run,
        // which has its own tools or none)
        if (Install.TestRoot != null) return null;
        for (var d = new DirectoryInfo(Install.SelfDir); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "nx2", "make_game.py")))
                return new Tools(d.FullName, Environment.GetEnvironmentVariable("TRIFORCE_PYTHON") ?? "python");
        return null;
    }

    /// Unpacks Triforce-Tools.zip as the tools folder (the old one replaced only once the new one is complete)
    public static void Unpack(string zip, string tag, Action<double> progress)
    {
        var target = Folder;
        var part = target + ".part";
        if (Directory.Exists(part)) Directory.Delete(part, true);
        Directory.CreateDirectory(part);
        using (var z = ZipFile.OpenRead(zip))
        {
            // (the zip holds tools/... or the tools' contents at its root)
            // (and zips packed on Windows may say tools\... instead)
            var prefix = z.Entries.All(e => e.FullName.Replace('\\', '/').StartsWith("tools/", StringComparison.OrdinalIgnoreCase)) ? "tools/" : "";
            var full = Path.GetFullPath(part) + Path.DirectorySeparatorChar;
            for (var i = 0; i < z.Entries.Count; i++)
            {
                var e = z.Entries[i];
                var name = e.FullName.Replace('\\', '/').Substring(prefix.Length);
                if (name.Length == 0) continue;
                var dest = Path.GetFullPath(Path.Combine(part, name));
                if (!dest.StartsWith(full, StringComparison.OrdinalIgnoreCase)) continue;   // (no paths out of the folder)
                if (name.EndsWith("/")) { Directory.CreateDirectory(dest); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                e.ExtractToFile(dest, true);
                if ((i & 15) == 0) progress((double)i / z.Entries.Count);
            }
        }
        File.WriteAllText(Path.Combine(part, "version.txt"), tag + "\n");
        var old = target + ".old";
        if (Directory.Exists(old)) Directory.Delete(old, true);
        if (Directory.Exists(target)) Directory.Move(target, old);
        Directory.Move(part, target);
        try { if (Directory.Exists(old)) Directory.Delete(old, true); } catch (Exception) { }
        progress(1);
    }
}

/// nx2/make_game.py as a child process: its lines as they come, and how it ended.
sealed class Runner
{
    readonly Tools tools;
    readonly List<string> args;
    Process? process;
    string? error;
    bool cancelled;

    public event Action<string>? Line;
    public event Action<string?>? Finished;

    readonly string? ngxSdk;   // (NVIDIA DLSS chosen, its license accepted: the folder NVIDIA's SDK is fetched into and built from)
    public Runner(Tools tools, List<string> args, string? ngxSdk = null) { this.tools = tools; this.args = args; this.ngxSdk = ngxSdk; }

    static string Quote(string a)
    {
        if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return a;
        var sb = new StringBuilder("\"");
        for (var i = 0; i < a.Length; i++)
        {
            var slashes = 0;
            while (i < a.Length && a[i] == '\\') { slashes++; i++; }
            if (i == a.Length) { sb.Append('\\', slashes * 2); break; }
            if (a[i] == '"') sb.Append('\\', slashes * 2 + 1).Append('"');
            else sb.Append('\\', slashes).Append(a[i]);
        }
        return sb.Append('"').ToString();
    }

    static ProcessStartInfo Info(Tools tools, IEnumerable<string> args, string? ngxSdk = null)
    {
        var all = new List<string> { "-I", Path.Combine(tools.Root, "nx2", "make_game.py") };
        all.AddRange(args);
        var si = new ProcessStartInfo(tools.Python, string.Join(" ", all.Select(Quote)))
        {
            WorkingDirectory = tools.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        si.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
        if (ngxSdk != null) si.EnvironmentVariables["HOOKSHOT_NGX_SDK"] = ngxSdk;   // (nx2/make_game.py --fetch-ngx-sdk, nx2/build.py)
        // (written into the game folder: the game starts Setup again from here to update, host/hos_update.c)
        var installed = Install.InstalledDir is { } d ? Path.Combine(d, Program.SetupAsset) : Install.Self;
        si.EnvironmentVariables["HOOKSHOT_BUILDER_EXE"] = installed;
        foreach (var (name, folder) in new[] { ("EMSDK", "emsdk"), ("RX_CPM", "cpm"), ("WGPU_NATIVE", "wgpu-native"), ("HOOKSHOT_MINGW", "llvm-mingw") })
        {
            var path = Path.Combine(tools.Root, folder);
            if (Directory.Exists(path)) si.EnvironmentVariables[name] = path;
        }
        return si;
    }

    public void Start()
    {
        Task.Run(() =>
        {
            try
            {
                // (a test run builds and updates game folders only inside its own folder)
                foreach (var name in new[] { "--out", "--update" })
                {
                    var i = args.IndexOf(name);
                    if (i >= 0 && i + 1 < args.Count && !Install.Allowed(args[i + 1])) { Finished?.Invoke(Install.TestOnly); return; }
                }
                using var p = Process.Start(Info(tools, args, ngxSdk))!;
                process = p;
                p.ErrorDataReceived += (_, e) => { if (e.Data != null) Line?.Invoke(e.Data); };
                p.BeginErrorReadLine();
                try { p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception) { }
                for (string? line; (line = p.StandardOutput.ReadLine()) != null;)
                {
                    if (line.StartsWith("@@error ")) error = line.Substring(8);
                    Line?.Invoke(line);
                }
                p.WaitForExit();
                Finished?.Invoke(cancelled ? "cancelled" : p.ExitCode == 0 ? null : error ?? "The build tools stopped without saying why. See the details.");
            }
            catch (Exception e)
            {
                Finished?.Invoke("The build tools could not start: " + e.Message);
            }
        });
    }

    public void Cancel()
    {
        cancelled = true;
        try
        {
            if (process != null && !process.HasExited)
                Process.Start(new ProcessStartInfo("taskkill", "/PID " + process.Id + " /T /F") { CreateNoWindow = true, UseShellExecute = false })?.WaitForExit(5000);
        }
        catch (Exception) { }
    }

    /// Is this the supported game and version? The answer in plain words.
    public static Task<(bool Ok, string Text)> Check(Tools tools, IEnumerable<string> args) => Task.Run(() =>
    {
        try
        {
            using var p = Process.Start(Info(tools, args))!;
            p.ErrorDataReceived += (_, _) => { };
            p.BeginErrorReadLine();
            var result = (false, "The game files could not be read.");
            for (string? line; (line = p.StandardOutput.ReadLine()) != null;)
            {
                if (line.StartsWith("@@check ok ")) result = (true, line.Substring(11));
                else if (line.StartsWith("@@check bad ")) result = (false, line.Substring(12));
                else if (line.StartsWith("@@error ")) result = (false, line.Substring(8));
            }
            p.WaitForExit();
            return result;
        }
        catch (Exception)
        {
            if (Directory.Exists(Path.Combine(tools.Root, "python")) && !File.Exists(Path.Combine(tools.Root, "python", "python.exe")))
                return (false, "Triforce's own Python (tools\\python\\python.exe) is missing. Your antivirus probably removed it: restore it from " +
                               "quarantine, or start Triforce Setup again to download the tools once more.");
            return (false, "Python was not found. Start Triforce Setup again to download the Triforce tools.");
        }
    });
}

/// Folder picker (the modern Explorer one, via IFileOpenDialog)
static class FolderPicker
{
    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialogRCW { }

    [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileOpenDialog
    {
        [PreserveSig] int Show(IntPtr parent);
        void SetFileTypes(); void SetFileTypeIndex(); void GetFileTypeIndex(); void Advise(); void Unadvise();
        void SetOptions(uint fos); void GetOptions(out uint fos);
        void SetDefaultFolder(IShellItem psi); void SetFolder(IShellItem psi);
        void GetFolder(); void GetCurrentSelection();
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name); void GetFileName();
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel(); void SetFileNameLabel();
        void GetResult(out IShellItem item);
    }

    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem
    {
        void BindToHandler(); void GetParent();
        void GetDisplayName(uint sigdn, [MarshalAs(UnmanagedType.LPWStr)] out string name);
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr bc, [MarshalAs(UnmanagedType.LPStruct)] Guid riid, out IShellItem item);

    /// (testing, TRIFORCE_UI_SCRIPT: the folders the next picks answer with, no dialog shown)
    public static readonly Queue<string> Answers = new();

    public static string? Pick(Window owner, string title, string? start)
    {
        if (Answers.Count > 0) return Answers.Dequeue();
        var d = (IFileOpenDialog)new FileOpenDialogRCW();
        try
        {
            d.SetOptions(0x20 | 0x40 | 0x800);   // (PICKFOLDERS | FORCEFILESYSTEM | PATHMUSTEXIST)
            d.SetTitle(title);
            if (start != null)
            {
                var s = start;
                while (s.Length > 3 && !Directory.Exists(s)) s = Path.GetDirectoryName(s) ?? "";
                if (Directory.Exists(s))
                    try { SHCreateItemFromParsingName(s, IntPtr.Zero, typeof(IShellItem).GUID, out var item); d.SetFolder(item); } catch (Exception) { }
            }
            if (d.Show(new System.Windows.Interop.WindowInteropHelper(owner).Handle) != 0) return null;
            d.GetResult(out var result);
            result.GetDisplayName(0x80058000, out var path);   // (SIGDN_FILESYSPATH)
            return path;
        }
        finally { Marshal.FinalReleaseComObject(d); }
    }
}
