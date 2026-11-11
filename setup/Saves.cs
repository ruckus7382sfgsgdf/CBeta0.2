// Saves of this game (Ocarina of Time, 0400486024DEC000), wherever they are: Triforce keeps them in <game>\user\save\<uid>, emulators
// in their NAND (Eden, yuzu, suyu, sudachi, citron, torzu, EvilOOT: nand\user\save\0000000000000000\<user>\0400486024DEC000) or
// Ryujinx's bis\user\save\<index>\0. The files are the same everywhere (account.sav, option.sav, slot_00..05\caption.sav +
// progress.sav), so converting is a copy, with what gets replaced backed up first.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace TriforceSetup;

sealed class SaveSet
{
    public string Name = "", Folder = "";
    public int Slots;
    public DateTime Last;
    public bool Empty;   // (a place with no save yet: only a destination)

    public override string ToString() => Name;
}

static class Saves
{
    public const string TitleId = "0400486024DEC000";
    /// Triforce's save folder (host/nn_applet.c user_uid, host/nn_fs.c: save\<uid hi><uid lo>)
    public const string TriforceUid = "5a3c9e1f0b7d24680000000000000001";
    static readonly string[] Emulators = { "eden", "EvilOOT", "yuzu", "suyu", "sudachi", "citron", "torzu", "Ryujinx" };

    public static string TriforceFolder(string game) => Path.Combine(game, "user", "save", TriforceUid);

    /// A folder holding this game's saves
    public static bool IsSaveSet(string dir) =>
        Directory.Exists(dir) && (File.Exists(Path.Combine(dir, "option.sav")) || File.Exists(Path.Combine(dir, "slot_00", "progress.sav"))
                                  || File.Exists(Path.Combine(dir, "account.sav")));

    public static SaveSet Describe(string name, string dir)
    {
        var s = new SaveSet { Name = name, Folder = dir };
        if (!Directory.Exists(dir)) { s.Empty = true; return s; }
        s.Slots = Directory.GetDirectories(dir, "slot_*").Count(d => File.Exists(Path.Combine(d, "progress.sav")));
        var files = Directory.GetFiles(dir, "*.sav", SearchOption.AllDirectories);
        s.Empty = files.Length == 0;
        s.Last = files.Length > 0 ? files.Max(f => File.GetLastWriteTime(f)) : DateTime.MinValue;
        return s;
    }

    /// Every save of this game found on the PC: the emulators' usual folders, the folders of programs the desktop and Start menu start
    /// (portable emulators keep a "user" folder beside them), and Ryujinx's numbered saves that hold this game's files
    public static List<SaveSet> FindEmulatorSaves()
    {
        var found = new List<SaveSet>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Add(string name, string dir)
        {
            try
            {
                if (!IsSaveSet(dir)) return;
                var full = Path.GetFullPath(dir).TrimEnd('\\');
                if (seen.Add(full)) found.Add(Describe(name, full));
            }
            catch (Exception) { }
        }
        void Nand(string name, string root)   // (root\nand\user\save\0000000000000000\<user>\<title>)
        {
            var users = Path.Combine(root, "nand", "user", "save", "0000000000000000");
            if (!Directory.Exists(users)) return;
            foreach (var u in SafeDirs(users)) Add(name, Path.Combine(u, TitleId));
        }
        void Ryujinx(string name, string root)   // (root\bis\user\save\<index>\0)
        {
            var saves = Path.Combine(root, "bis", "user", "save");
            if (!Directory.Exists(saves)) return;
            foreach (var s in SafeDirs(saves)) Add(name, Path.Combine(s, "0"));
        }
        var appData = Install.TestRoot != null ? Path.Combine(Install.TestRoot, "Roaming") : Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        foreach (var e in Emulators)
        {
            var root = Path.Combine(appData, e);
            if (e == "Ryujinx") Ryujinx("Ryujinx", root); else Nand(Pretty(e), root);
        }
        // (portable copies: the programs the shortcuts start)
        foreach (var exe in Install.TestRoot != null ? Enumerable.Empty<string>() : ShortcutTargets())
        {
            var dir = Path.GetDirectoryName(exe);
            if (dir == null) continue;
            var name = Path.GetFileNameWithoutExtension(exe) + " (" + dir + ")";
            Nand(name, Path.Combine(dir, "user"));
            Ryujinx(name, Path.Combine(dir, "portable"));
        }
        return found.OrderByDescending(s => s.Last).ToList();
    }

    static string Pretty(string e) => e switch
    {
        "eden" => "Eden", "yuzu" => "yuzu", "suyu" => "suyu", "sudachi" => "Sudachi", "citron" => "Citron", "torzu" => "torzu", _ => e,
    };

    static IEnumerable<string> SafeDirs(string d)
    {
        try { return Directory.GetDirectories(d); } catch (Exception) { return Array.Empty<string>(); }
    }

    /// The .exe files the desktop and Start menu shortcuts start
    static IEnumerable<string> ShortcutTargets()
    {
        var places = new[]
        {
            Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory),
            Environment.GetFolderPath(Environment.SpecialFolder.Programs), Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
        };
        var exes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Type? type = Type.GetTypeFromProgID("WScript.Shell");
        if (type == null) return exes;
        object? shell = null;
        try
        {
            shell = Activator.CreateInstance(type);
            foreach (var p in places.Where(Directory.Exists))
            {
                IEnumerable<string> links;
                try { links = Directory.GetFiles(p, "*.lnk", p.EndsWith("Programs", StringComparison.OrdinalIgnoreCase) ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly); }
                catch (Exception) { continue; }
                foreach (var l in links.Take(400))
                {
                    try
                    {
                        var lnk = type.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { l })!;
                        var target = lnk.GetType().InvokeMember("TargetPath", System.Reflection.BindingFlags.GetProperty, null, lnk, null) as string;
                        System.Runtime.InteropServices.Marshal.FinalReleaseComObject(lnk);
                        if (target != null && target.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && File.Exists(target)) exes.Add(target);
                    }
                    catch (Exception) { }
                }
            }
        }
        catch (Exception) { }
        finally { if (shell != null) System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shell); }
        return exes;
    }

    /// A chosen folder: the save set it is, or the first one inside it (a few levels down)
    public static string? FindIn(string dir)
    {
        if (IsSaveSet(dir)) return dir;
        var queue = new Queue<(string, int)>();
        queue.Enqueue((dir, 0));
        var looked = 0;
        while (queue.Count > 0 && looked++ < 4000)
        {
            var (d, depth) = queue.Dequeue();
            foreach (var s in SafeDirs(d))
            {
                if (IsSaveSet(s)) return s;
                if (depth < 7) queue.Enqueue((s, depth + 1));
            }
        }
        return null;
    }

    /// A game or emulator that has these saves open: it would write over the copy when it closes
    public static string? RunningGame()
    {
        foreach (var n in new[] { "Triforce", "hookshot", "EvilOOT", "eden", "yuzu", "suyu", "sudachi", "citron", "torzu", "Ryujinx" })
            foreach (var p in Process.GetProcessesByName(n))
                if (Install.TestRoot == null || InTestRoot(p)) return n;
        return null;
    }

    /// (testing, TRIFORCE_TEST_ROOT: only a game started from inside the test folder counts. The PC's own games have their own
    /// saves, which a test neither reads nor touches)
    static bool InTestRoot(Process p)
    {
        try { return p.MainModule!.FileName.StartsWith(Install.TestRoot!, StringComparison.OrdinalIgnoreCase); }
        catch (Exception) { return false; }
    }

    /// Copies one save set over another: the destination's old files are moved to the backup folder first (nothing is merged slot by
    /// slot: the copy is exactly the source). Returns the backup folder, or null when there was nothing to back up.
    public static string? Copy(string from, string to, string backupRoot, string label)
    {
        var src = Path.GetFullPath(from).TrimEnd('\\');
        var dst = Path.GetFullPath(to).TrimEnd('\\');
        if (!Install.Allowed(dst)) throw new InvalidOperationException(Install.TestOnly);
        if (string.Equals(src, dst, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("That is the same save folder.");
        string? backup = null;
        if (Directory.Exists(dst) && Directory.EnumerateFileSystemEntries(dst).Any())
        {
            var safe = string.Concat(label.Select(c => Path.GetInvalidFileNameChars().Contains(c) || c == ' ' ? '-' : c));
            backup = Path.Combine(backupRoot, DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss") + "_" + safe);
            Directory.CreateDirectory(backup);
            CopyTree(dst, backup);
            Directory.Delete(dst, true);
        }
        Directory.CreateDirectory(dst);
        CopyTree(src, dst);
        return backup;
    }

    static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), true);
        foreach (var d in Directory.GetDirectories(from)) CopyTree(d, Path.Combine(to, Path.GetFileName(d)));
    }
}
