using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace FnOffline;

/// <summary>Paths: builds live in &lt;root&gt;\&lt;version&gt;. The root is config.json "buildsFolder" (set by the installer or
/// with "Change builds folder"); without it, the folder that contains the launcher (portable layout &lt;root&gt;\launcher).</summary>
public static class Paths
{
    /// <summary>This exe's folder; radarnite-play.exe sets the launcher's (--launcher-dir).</summary>
    public static string LauncherDir { get; set; } = AppContext.BaseDirectory.TrimEnd('\\');
    static string? _root;
    public static string Root => _root ??= BuildsFolder() ?? Directory.GetParent(LauncherDir)!.FullName;
    /// <summary>Forget the cached root (config.json was saved).</summary>
    public static void ResetRoot() => _root = null;
    static string? BuildsFolder()
    {
        try
        {
            var f = JsonNode.Parse(File.ReadAllText(Config))?["buildsFolder"]?.GetValue<string>();
            return string.IsNullOrWhiteSpace(f) ? null : Directory.CreateDirectory(Environment.ExpandEnvironmentVariables(f)).FullName.TrimEnd('\\');
        }
        catch { return null; }
    }
    public static string Config => Path.Combine(LauncherDir, "config.json");
    public static string Node => Path.Combine(LauncherDir, "node", "node.exe");
    public static string Backend => Path.Combine(LauncherDir, "backend", "server.mjs");
    public static string RedirectDll => Path.Combine(LauncherDir, "redirect", "fnredirect.dll");
    public static string GameServerDll => Path.Combine(LauncherDir, "gameserver", "radarnite-server.dll");
    public static string PakPatchDll => Path.Combine(LauncherDir, "patches", "fnpakpatch.dll");
    public static string ModsDll => Path.Combine(LauncherDir, "patches", "fnmods.dll");
    /// <summary>fnmods.dll's settings: written by the 3.0 launcher's Mods tab, read by the DLL (FNMODS_CONFIG).</summary>
    public static string ModsConfig => Path.Combine(LauncherDir, "mods.json");
    public static string CosmeticsGen => Path.Combine(LauncherDir, "tools", "cosmetics-gen", "fn-cosmetics-gen.exe");
    public static string Win64(string build) => Path.Combine(build, "FortniteGame", "Binaries", "Win64");
    public static string Shipping(string build) => Path.Combine(Win64(build), "FortniteClient-Win64-Shipping.exe");
    public static string Offline(string build) => Directory.CreateDirectory(Path.Combine(build, "offline")).FullName;
    public static bool IsUnder(string? path, string dir) =>
        path != null && Path.GetFullPath(path).StartsWith(Path.GetFullPath(dir).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase);
}

public sealed class Config
{
    public string DisplayName { get; set; } = "";
    public string AccountId { get; set; } = "";
    public int Port { get; set; } = 3551;
    /// <summary>Legacy (before the mode was picked in the game); kept so old config files still load. Not used.</summary>
    public string Mode { get; set; } = "practice";
    /// <summary>Last playlist asked for in the game (lower-case mnemonic): its match server is pre-started at launch.</summary>
    public string LastPlaylist { get; set; } = "playlist_defaultsolo";
    public int Bots { get; set; } = 20;
    public bool InfiniteMaterials { get; set; } = true;
    public bool InfiniteAmmo { get; set; } = false;
    public bool LateGame { get; set; } = false;
    public string PracticePlaylist { get; set; } = "/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    public string MatchPlaylist { get; set; } = "/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    /// <summary>Bots with the game's own AI (S11+, experimental); false = bots without AI (they only skydive).</summary>
    public bool BotsAI { get; set; } = false;
    /// <summary>Seconds after the player connects before the bus starts by itself (0 = no automatic start).</summary>
    public int AutoStartSeconds { get; set; } = 60;
    /// <summary>Difficulty 1-10 of the bots on the game's AI (radarnite-play --bot-skill; 3.x keeps it in config.json "botSkill").</summary>
    [System.Text.Json.Serialization.JsonIgnore] public int BotSkill { get; set; } = 6;
    /// <summary>Where the builds live ("" = the folder that contains the launcher folder).</summary>
    public string BuildsFolder { get; set; } = "";

    /// <summary>Written into config.json as "_help" (JSON has no comments); rewritten on every save.</summary>
    [System.Text.Json.Serialization.JsonPropertyName("_help")]
    public Dictionary<string, string> Help { get; set; } = new();
    static Dictionary<string, string> HelpText() => new()
    {
        ["displayName"] = "Your in-game name (random by default).",
        ["accountId"] = "Local account id (random; nothing to do with an Epic account).",
        ["port"] = "Port of the local backend on 127.0.0.1.",
        ["mode"] = "Not used any more: pick the mode in the game (lobby -> CHANGE): Creative/Playground = practice alone with infinite materials and ammo; Solo, Duos, Trios, Squads = Battle Royale with bots.",
        ["lastPlaylist"] = "Last mode picked in the game; its match server is prepared when the game starts.",
        ["bots"] = "Bots that fill the bus in Solo/Duos/Trios/Squads (0-99).",
        ["botsAI"] = "false (default) = bots without AI (they drop from the bus and land); true = the game's own bot AI (Season 11+), EXPERIMENTAL: on 17.50 it crashes the match server a few seconds after it starts.",
        ["infiniteMaterials"] = "Infinite building materials in Battle Royale matches (practice always has them).",
        ["infiniteAmmo"] = "Infinite ammo in Battle Royale matches (practice always has it).",
        ["lateGame"] = "Start in late game (small storm, shields).",
        ["practicePlaylist"] = "Playlist the match server runs when you pick Creative/Playground (practice).",
        ["matchPlaylist"] = "Not used any more (the Battle Royale playlist is the one picked in the game).",
        ["autoStartSeconds"] = "Seconds after you connect until the bus leaves by itself (0 = it does not leave by itself).",
        ["buildsFolder"] = "Folder that holds your Fortnite builds, one folder per build (empty = the folder that contains the launcher's folder).",
    };

    static readonly JsonSerializerOptions Opt = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, PropertyNamingPolicy = JsonNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true };

    public static Config Load()
    {
        Config c;
        try { c = JsonSerializer.Deserialize<Config>(File.ReadAllText(Paths.Config), Opt) ?? new(); }
        catch (FileNotFoundException) { c = new(); }
        catch
        {
            // A config.json edited by hand with a mistake: keep the person's file (config.json.broken) instead of silently
            // replacing their name and account with new random ones, and start from the defaults.
            c = new();
            try
            {
                File.Copy(Paths.Config, Paths.Config + ".broken", overwrite: true);
                var raw = File.ReadAllText(Paths.Config); // keep at least the identity (name, account: same locker)
                var n = Regex.Match(raw, "\"displayName\"\\s*:\\s*\"([^\"]{1,32})\"");
                var a = Regex.Match(raw, "\"accountId\"\\s*:\\s*\"([0-9a-f]{32})\"");
                if (n.Success) c.DisplayName = n.Groups[1].Value;
                if (a.Success) c.AccountId = a.Groups[1].Value;
            }
            catch { }
        }
        var changed = false;
        if (string.IsNullOrWhiteSpace(c.DisplayName))
        {
            string[] a = ["Swift", "Tactical", "Silent", "Brave", "Electric", "Midnight", "Golden", "Fierce"];
            string[] b = ["Wolf", "Hawk", "Fox", "Tiger", "Raven", "Lynx", "Bear", "Puma"];
            c.DisplayName = a[Random.Shared.Next(a.Length)] + b[Random.Shared.Next(b.Length)] + Random.Shared.Next(10, 99);
            changed = true;
        }
        if (!Regex.IsMatch(c.AccountId ?? "", "^[0-9a-f]{32}$")) { c.AccountId = Guid.NewGuid().ToString("N"); changed = true; }
        if (changed || !File.Exists(Paths.Config) || c.Help.Count != HelpText().Count) c.Save();
        return c;
    }

    public void Save() { Help = HelpText(); File.WriteAllText(Paths.Config, JsonSerializer.Serialize(this, Opt)); Paths.ResetRoot(); }
}

public enum Support { Yes, Experimental, No }

public sealed record BuildInfo(string Dir, string Version, int Major, int Minor, long Cl, string Engine, string? Folder = null)
{
    /// <summary>Name shown and used to pick the build: the folder under the root (a zip extracted with its own top
    /// folder, e.g. 3.6-CL-4019403\Fortnite\FortniteGame, keeps the outer name; Dir is where FortniteGame is).</summary>
    public string Name => Path.GetFileName(Folder ?? Dir);
    public bool HasEosAntiCheat => File.Exists(Path.Combine(Paths.Win64(Dir), "FortniteClient-Win64-Shipping_EAC_EOS.exe"));

    /// <summary>IoStore builds (UE 4.26+/UE5) ship .utoc/.ucas containers and verify each one's signature when it is
    /// mounted. Our own cosmetic containers under ~mods are unsigned, so on these builds the redirect neutralises that
    /// one signature check in memory (FNOFFLINE_IOSTORE_UNSIGNED). Detected by the presence of any .utoc.</summary>
    public bool UsesIoStore
    {
        get
        {
            var paks = Path.Combine(Dir, "FortniteGame", "Content", "Paks");
            try { return Directory.Exists(paks) && Directory.EnumerateFiles(paks, "*.utoc").Any(); }
            catch { return false; }
        }
    }

    /// <summary>Lobby (backend + redirect): the curl hook and the classic -noeac launch exist up to Chapter 4 (UE 5.1);
    /// from 24.x (EOS anticheat, EOS login) the method does not apply.</summary>
    public (Support s, string why) Lobby => HasEosAntiCheat || Major >= 24
        ? (Support.No, "uses EOS Easy Anti-Cheat and EOS login (24.x+): this method does not apply")
        : Major >= 20 ? (Support.Experimental, "UE5 (20.x-23.x): curl redirect likely, untested")
        : Major < 3 ? (Support.Experimental, "Seasons 1-2: untested")
        : (Support.Yes, "UE4 (3.x-19.x): curl redirect + -noeac launch");

    /// <summary>Local match: Radarnite match server (S3-S15 supported, S1-2 and S16-19 may not work perfectly, S20+ not
    /// supported).</summary>
    public (Support s, string why) Match => Lobby.s == Support.No || Major >= 20
        ? (Support.No, "no match server for this version (Seasons 1-19 only)")
        : Major >= 16 || Major < 3 ? (Support.Experimental, "Seasons 1-2 and 16-19 may not work perfectly")
        : (Support.Yes, "Seasons 3-15 supported");

    /// <summary>Code patches for the match server process only (applied by the redirect DLL, each one only if the
    /// original bytes match exactly). Format: rva:expected:new;...
    /// 17.50 (CL 17388565): the game arms a timer that calls FPlatformMisc::RequestExit(true) a few seconds after a
    /// non-dedicated build starts hosting (net mode checks in several places, all through the same lambda at
    /// exe+0x44d2edc "mov cl,1; jmp RequestExit"). Turning that lambda into "ret" keeps the server in the match.</summary>
    public string ServerPatches => Cl switch
    {
        17388565 => "44d2edc:b101e9:c3",
        _ => ""
    };

    /// <summary>Engine version per Fortnite version (CUE4Parse needs it to read the Paks).</summary>
    public static string EngineFor(int major, int minor) => major switch
    {
        <= 4 => "4.20", 5 or 6 => "4.21", 7 => "4.22", 8 => minor >= 30 ? "4.23" : "4.22", 9 or 10 => "4.23", 11 or 12 => "4.24",
        13 => "4.25", 14 => minor >= 30 ? "4.26" : "4.25", >= 15 and <= 18 => "4.26", 19 or 20 => "5.0", 21 or 22 => "5.1", 23 => "5.2",
        24 or 25 or 26 or 27 => "5.3", 28 or 29 => "5.4", 30 or 31 or 32 or 33 => "5.5", 34 or 35 or 36 => "5.6", _ => "5.7"
    };

    public static BuildInfo? Read(string dir) => Read(dir, out _);

    /// <summary>Reads the build in <paramref name="dir"/>, or in a single nested folder of it (archives that carry their
    /// own top folder). Returns null with the reason when it is not a usable build.</summary>
    public static BuildInfo? Read(string dir, out string? why)
    {
        why = null;
        var gameDir = dir;
        if (!File.Exists(Paths.Shipping(gameDir)))
        {
            var nested = SafeDirs(dir).SelectMany(d => new[] { d }.Concat(SafeDirs(d))).Where(d => File.Exists(Paths.Shipping(d))).ToList();
            if (nested.Count != 1)
            {
                why = nested.Count > 1 ? "more than one game inside" :
                    Directory.Exists(Path.Combine(dir, "FortniteGame")) || SafeDirs(dir).Any(d => Directory.Exists(Path.Combine(d, "FortniteGame")))
                        ? "FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe is missing (incomplete extraction?)"
                        : "no FortniteGame folder (not a Fortnite build)";
                return null;
            }
            gameDir = nested[0];
        }
        var exe = Paths.Shipping(gameDir);
        var folder = gameDir == dir ? null : dir;
        var cache = Path.Combine(gameDir, "offline", "build.json");
        try
        {
            if (File.Exists(cache) && JsonNode.Parse(File.ReadAllText(cache)) is { } j && j["exeLength"]?.GetValue<long>() == new FileInfo(exe).Length)
                return Make(gameDir, j["version"]!.GetValue<string>(), folder);
        }
        catch { }
        string? v;
        try { v = ScanVersion(exe); } catch (Exception e) { why = "cannot read the game executable: " + e.Message; return null; }
        if (v == null) return new BuildInfo(gameDir, "?", 0, 0, 0, "?", folder);
        try
        {
            Directory.CreateDirectory(Path.Combine(gameDir, "offline"));
            File.WriteAllText(cache, new JsonObject { ["version"] = v, ["exeLength"] = new FileInfo(exe).Length }.ToJsonString());
        }
        catch { } // read-only or locked folder: the version is still known, only not cached
        return Make(gameDir, v, folder);
    }

    static IEnumerable<string> SafeDirs(string d) { try { return Directory.GetDirectories(d); } catch { return []; } }

    static BuildInfo Make(string dir, string full, string? folder = null)
    {
        var m = Regex.Match(full, @"Release-((\d+)\.(\d+)[\d.]*)-CL-(\d+)");
        if (!m.Success) return new BuildInfo(dir, full, 0, 0, 0, "?", folder);
        int major = int.Parse(m.Groups[2].Value), minor = int.Parse(m.Groups[3].Value);
        return new BuildInfo(dir, m.Groups[1].Value, major, minor, long.Parse(m.Groups[4].Value), EngineFor(major, minor), folder);
    }

    /// <summary>The build string "++Fortnite+Release-17.50-CL-17388565" stored as UTF-16 in the client executable.</summary>
    public static string? ScanVersion(string exe)
    {
        var pattern = Encoding.Unicode.GetBytes("++Fortnite+Release-");
        var data = File.ReadAllBytes(exe);
        var span = data.AsSpan();
        int off = 0;
        while (true)
        {
            var i = span[off..].IndexOf(pattern);
            if (i < 0) break;
            var pos = off + i;
            var len = Math.Min(120, data.Length - pos);
            var text = Encoding.Unicode.GetString(data, pos, len - len % 2);
            var m = Regex.Match(text, @"\+\+Fortnite\+Release-[\d.]+-CL-\d+");
            if (m.Success) return m.Value;
            off = pos + 2;
        }
        // Some builds keep it only as ASCII.
        var ascii = Encoding.Latin1.GetString(data);
        var m2 = Regex.Match(ascii, @"\+\+Fortnite\+Release-[\d.]+-CL-\d+");
        return m2.Success ? m2.Value : null;
    }

    public static List<BuildInfo> All() => Scan(Paths.Root, out _);

    /// <summary>Every build under <paramref name="root"/>. Folders that are not usable builds never break the list:
    /// they go to <paramref name="skipped"/> with the reason (shown in Activity).</summary>
    public static List<BuildInfo> Scan(string root, out List<(string dir, string why)> skipped)
    {
        var list = new List<BuildInfo>();
        skipped = new();
        foreach (var d in SafeDirs(root))
        {
            if (Path.GetFileName(d).Equals("launcher", StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                if (Read(d, out var why) is { } b) list.Add(b);
                else skipped.Add((d, why ?? "not a build"));
            }
            catch (Exception e) { skipped.Add((d, e.Message)); }
        }
        return list.OrderBy(b => b.Major).ThenBy(b => b.Minor).ThenBy(b => b.Name).ToList();
    }
}

/// <summary>Windows Firewall rules (group "FN Offline Builds"): every exe of a build and the launcher's node.exe may
/// not reach anything but loopback (outbound blocked, inbound from non-loopback blocked). Read through the
/// HNetCfg.FwPolicy2 COM API (no admin needed); created by "--firewall" running elevated.</summary>
public static class Firewall
{
    public const string Group = "FN Offline Builds";
    // Everything except 127.0.0.0/8 and ::1.
    public const string NonLoopback = "0.0.0.0-126.255.255.255,128.0.0.0-255.255.255.255,::-::0,::2-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff";
    const int OUT = 2, IN = 1, BLOCK = 0, ALL_PROFILES = 0x7FFFFFFF;

    public static IEnumerable<string> ProgramsFor(string build) =>
        Directory.EnumerateFiles(build, "*.exe", SearchOption.AllDirectories).Append(Paths.Node).Append(Paths.CosmeticsGen).Where(File.Exists)
            .Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase);

    static dynamic Policy() => Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2")!)!;

    /// <summary>null = OK; otherwise what is missing.</summary>
    public static string? Check(string build)
    {
        dynamic pol = Policy();
        int current = pol.CurrentProfileTypes;
        foreach (var p in new[] { 1, 2, 4 })
            if ((current & p) != 0 && !(bool)pol.FirewallEnabled[p])
                return "Windows Firewall is off for the current network profile";
        var have = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (dynamic r in pol.Rules)
        {
            try
            {
                if ((string?)r.Grouping != Group || !(bool)r.Enabled || (int)r.Action != BLOCK || (int)r.Direction != OUT) continue;
                if ((string?)r.RemoteAddresses is not { } ra || ra.Contains("127.0.0.1")) continue;
                if ((string?)r.ApplicationName is { } app) have.Add(Path.GetFullPath(app));
            }
            catch { }
        }
        var missing = ProgramsFor(build).Where(p => !have.Contains(p)).ToList();
        return missing.Count == 0 ? null : $"firewall rules missing for {missing.Count} programs (e.g. {Path.GetFileName(missing[0])})";
    }

    public static int Create(string build)
    {
        dynamic pol = Policy();
        int n = 0;
        var programs = new HashSet<string>(ProgramsFor(build), StringComparer.OrdinalIgnoreCase);
        var stale = new List<string>();
        foreach (dynamic r in pol.Rules)
        {
            try { if ((string?)r.Grouping == Group && (string?)r.ApplicationName is { } app && programs.Contains(Path.GetFullPath(app))) stale.Add((string)r.Name); } catch { }
        }
        foreach (var name in stale) { try { pol.Rules.Remove(name); } catch { } }
        foreach (var exe in programs)
        {
            var rel = Paths.IsUnder(exe, Paths.LauncherDir) ? Path.Combine(Path.GetFileName(Paths.LauncherDir), Path.GetRelativePath(Paths.LauncherDir, exe)) : Path.GetRelativePath(Paths.Root, exe);
            foreach (var (dir, tag) in new[] { (OUT, "outbound"), (IN, "inbound") })
            {
                var name = $"FN Offline - {rel} ({tag})";
                try { pol.Rules.Remove(name); } catch { }
                dynamic rule = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FWRule")!)!;
                rule.Name = name;
                rule.Description = $"Fortnite Offline: 127.0.0.1 only. Created by Radarnite Launcher ({Paths.LauncherDir}).";
                rule.ApplicationName = exe;
                rule.Protocol = 256; // any
                rule.RemoteAddresses = NonLoopback;
                rule.Direction = dir;
                rule.Action = BLOCK;
                rule.Profiles = ALL_PROFILES;
                rule.Grouping = Group;
                rule.Enabled = true;
                pol.Rules.Add(rule);
                n++;
            }
        }
        return n;
    }

    public static int RemoveAll()
    {
        dynamic pol = Policy();
        var names = new List<string>();
        foreach (dynamic r in pol.Rules) { try { if ((string?)r.Grouping == Group) names.Add((string)r.Name); } catch { } }
        foreach (var n in names) { try { pol.Rules.Remove(n); } catch { } }
        return names.Count;
    }
}

public static class Native
{
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, IntPtr size, int type, int protect);
    [DllImport("kernel32", SetLastError = true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, IntPtr stack, IntPtr start, IntPtr param, int flags, out int tid);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr GetModuleHandleW([MarshalAs(UnmanagedType.LPWStr)] string name);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr GetProcAddress(IntPtr mod, string name);
    [DllImport("kernel32", SetLastError = true)] static extern int WaitForSingleObject(IntPtr h, int ms);
    [DllImport("kernel32", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetExitCodeThread(IntPtr h, out uint code);

    /// <summary>LoadLibraryW(dll) inside the process; true when the DLL loaded.</summary>
    public static bool Inject(int pid, string dll)
    {
        var h = OpenProcess(0x043A, false, pid); // CREATE_THREAD | VM_OPERATION | VM_WRITE | VM_READ | QUERY_INFORMATION
        if (h == IntPtr.Zero) return false;
        try
        {
            var bytes = Encoding.Unicode.GetBytes(dll + "\0");
            var mem = VirtualAllocEx(h, IntPtr.Zero, bytes.Length, 0x3000, 0x04);
            if (mem == IntPtr.Zero || !WriteProcessMemory(h, mem, bytes, bytes.Length, out _)) return false;
            var load = GetProcAddress(GetModuleHandleW("kernel32.dll"), "LoadLibraryW");
            var t = CreateRemoteThread(h, IntPtr.Zero, IntPtr.Zero, load, mem, 0, out _);
            if (t == IntPtr.Zero) return false;
            WaitForSingleObject(t, 15000);
            GetExitCodeThread(t, out var code);
            CloseHandle(t);
            return code != 0;
        }
        finally { CloseHandle(h); }
    }

    // ---- processes created suspended (the build's own launcher/anticheat stubs: they never run a single instruction) ----
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO { public int cb; public string? lpReserved, lpDesktop, lpTitle; public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string? app, StringBuilder? cmd, IntPtr pa, IntPtr ta, bool inherit, int flags, IntPtr env, string? cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    public static int StartSuspended(string exe)
    {
        var si = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
        if (!CreateProcessW(exe, new StringBuilder($"\"{exe}\""), IntPtr.Zero, IntPtr.Zero, false, 0x4 /*CREATE_SUSPENDED*/ | 0x08000000 /*NO_WINDOW*/,
                IntPtr.Zero, Path.GetDirectoryName(exe), ref si, out var pi)) return 0;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    // ---- start a process with a DLL loaded before its own code runs ("early-bird" APC) ----
    [DllImport("kernel32", SetLastError = true)] static extern uint QueueUserAPC(IntPtr pfnAPC, IntPtr hThread, IntPtr data);
    [DllImport("kernel32", SetLastError = true)] static extern uint ResumeThread(IntPtr hThread);

    /// <summary>Creates the process suspended, queues LoadLibraryW(dll) on its main thread and resumes it: the DLL is
    /// loaded right after the process' static imports and before its entry point. Returns the Process (or null).</summary>
    public static Process? StartWithDll(string exe, string args, IDictionary<string, string?> env, string dll, bool hidden, bool suspended = true, bool customEnv = true)
    {
        var block = new StringBuilder();
        var all = Environment.GetEnvironmentVariables().Cast<System.Collections.DictionaryEntry>()
            .ToDictionary(e => (string)e.Key, e => (string?)e.Value, StringComparer.OrdinalIgnoreCase);
        foreach (var (k, v) in env) all[k] = v;
        foreach (var (k, v) in all.OrderBy(k => k.Key, StringComparer.OrdinalIgnoreCase)) if (v != null) block.Append(k).Append('=').Append(v).Append('\0');
        block.Append('\0');
        var envPtr = Marshal.StringToHGlobalUni(block.ToString());
        try
        {
            var si = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
            if (hidden) { si.dwFlags = 1 /*STARTF_USESHOWWINDOW*/; si.wShowWindow = 0 /*SW_HIDE*/; }
            if (!CreateProcessW(exe, new StringBuilder($"\"{exe}\" {args}"), IntPtr.Zero, IntPtr.Zero, false,
                    (suspended ? 0x4 /*CREATE_SUSPENDED*/ : 0) | 0x400 /*CREATE_UNICODE_ENVIRONMENT*/, customEnv ? envPtr : IntPtr.Zero, Path.GetDirectoryName(exe), ref si, out var pi)) return null;
            try
            {
                var bytes = Encoding.Unicode.GetBytes(dll + "\0");
                var mem = VirtualAllocEx(pi.hProcess, IntPtr.Zero, bytes.Length, 0x3000, 0x04);
                if (dll.Length > 0 && mem != IntPtr.Zero && WriteProcessMemory(pi.hProcess, mem, bytes, bytes.Length, out _))
                    HijackEntry(pi.hProcess, pi.hThread, mem);
                if (suspended) ResumeThread(pi.hThread);
                return Process.GetProcessById(pi.dwProcessId);
            }
            finally { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        }
        finally { Marshal.FreeHGlobal(envPtr); }
    }

    [DllImport("kernel32", SetLastError = true)] static extern bool GetThreadContext(IntPtr hThread, IntPtr ctx);
    [DllImport("kernel32", SetLastError = true)] static extern bool SetThreadContext(IntPtr hThread, IntPtr ctx);
    [DllImport("kernel32", SetLastError = true)] static extern bool VirtualProtectEx(IntPtr h, IntPtr addr, IntPtr size, uint prot, out uint old);

    /// <summary>
    /// Entry-point hijack of a suspended new process: its first thread starts in RtlUserThreadStart with the exe's
    /// entry point in RCX. RCX is pointed at a 40-byte stub that calls LoadLibraryW(dll) and then jumps to the real
    /// entry point. The DLL thus loads after the loader finished (all static DLLs and overlays initialised) and before
    /// any of the program's own code (CRT, static constructors, WinMain). x64 only.
    /// </summary>
    static void HijackEntry(IntPtr hProcess, IntPtr hThread, IntPtr dllPathRemote)
    {
        const int CONTEXT_SIZE = 1232, OFF_FLAGS = 0x30, OFF_RCX = 0x80;
        var raw = Marshal.AllocHGlobal(CONTEXT_SIZE + 16);
        try
        {
            var ctx = (IntPtr)(((long)raw + 15) & ~15L);
            for (var i = 0; i < CONTEXT_SIZE; i++) Marshal.WriteByte(ctx, i, 0);
            Marshal.WriteInt32(ctx, OFF_FLAGS, 0x10000B); // CONTEXT_FULL (AMD64)
            if (!GetThreadContext(hThread, ctx)) return;
            var entry = Marshal.ReadInt64(ctx, OFF_RCX);
            var loadLib = (long)GetProcAddress(GetModuleHandleW("kernel32.dll"), "LoadLibraryW");
            var code = new List<byte>();
            code.Add(0x51);                                             // push rcx (keep the entry argument)
            code.AddRange(new byte[] { 0x48, 0x83, 0xEC, 0x20 });       // sub rsp, 20h (shadow space, stack aligned)
            code.AddRange(new byte[] { 0x48, 0xB9 }); code.AddRange(BitConverter.GetBytes((long)dllPathRemote)); // mov rcx, path
            code.AddRange(new byte[] { 0x48, 0xB8 }); code.AddRange(BitConverter.GetBytes(loadLib));            // mov rax, LoadLibraryW
            code.AddRange(new byte[] { 0xFF, 0xD0 });                   // call rax
            code.AddRange(new byte[] { 0x48, 0x83, 0xC4, 0x20 });       // add rsp, 20h
            code.Add(0x59);                                             // pop rcx
            code.AddRange(new byte[] { 0x48, 0xB8 }); code.AddRange(BitConverter.GetBytes(entry));              // mov rax, entry
            code.AddRange(new byte[] { 0xFF, 0xE0 });                   // jmp rax
            var stub = VirtualAllocEx(hProcess, IntPtr.Zero, code.Count, 0x3000, 0x04);
            if (stub == IntPtr.Zero || !WriteProcessMemory(hProcess, stub, code.ToArray(), code.Count, out _)) return;
            VirtualProtectEx(hProcess, stub, code.Count, 0x20 /*PAGE_EXECUTE_READ*/, out _);
            Marshal.WriteInt64(ctx, OFF_RCX, (long)stub);
            SetThreadContext(hThread, ctx);
        }
        finally { Marshal.FreeHGlobal(raw); }
    }

    /// <summary>Full image path of a process; works for suspended processes too (unlike Process.MainModule).</summary>
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageNameW(IntPtr h, int flags, StringBuilder name, ref int size);
    [DllImport("kernel32")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    /// <summary>Developer self-test: start, wait, exit code (keeps its own handle so the code is readable).</summary>
    public static string TestStart(string exe, string args, IDictionary<string, string?> env, string dll, bool suspended = true, bool customEnv = true)
    {
        var p = StartWithDll(exe, args, env, dll, true, suspended, customEnv);
        if (p == null) return "start failed " + Marshal.GetLastWin32Error();
        var h = OpenProcess(0x1000 | 0x00100000 /*SYNCHRONIZE*/, false, p.Id);
        var done = WaitForSingleObject(h, 8000) == 0;
        GetExitCodeProcess(h, out var code); CloseHandle(h);
        if (!done) { try { p.Kill(); } catch { } return "still running after 8 s (killed)"; }
        return $"exited 0x{code:X8}";
    }

    public static IntPtr OpenForExitCode(int pid) => OpenProcess(0x1000 | 0x00100000, false, pid);
    public static string ExitCodeOf(IntPtr h) => h != IntPtr.Zero && GetExitCodeProcess(h, out var c) ? $"0x{c:X8}" : "?";

    public static string? ImagePath(int pid)
    {
        var h = OpenProcess(0x1000 /*QUERY_LIMITED_INFORMATION*/, false, pid);
        if (h == IntPtr.Zero) return null;
        try { var sb = new StringBuilder(1024); int n = sb.Capacity; return QueryFullProcessImageNameW(h, 0, sb, ref n) ? sb.ToString() : null; }
        finally { CloseHandle(h); }
    }

    // ---- hide every top-level window of a process ----
    delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern bool SetWindowTextW(IntPtr h, string text);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
    /// <summary>Hides every top-level window of the process and renames the game window, so another instance of the
    /// game (the client) does not take it for itself.</summary>
    public static void HideWindowsOf(int pid) =>
        EnumWindows((h, _) =>
        {
            GetWindowThreadProcessId(h, out var p);
            if (p != pid) return true;
            if (IsWindowVisible(h)) ShowWindow(h, 0 /*SW_HIDE*/);
            var sb = new StringBuilder(256); GetWindowTextW(h, sb, 256);
            if (sb.ToString().StartsWith("Fortnite", StringComparison.OrdinalIgnoreCase)) SetWindowTextW(h, "Radarnite match server");
            return true;
        }, IntPtr.Zero);

    // ---- job object: everything the launcher starts dies with it ----
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObjectW(IntPtr a, string? name);
    [DllImport("kernel32", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int cls, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int len);
    [DllImport("kernel32", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
    [StructLayout(LayoutKind.Sequential)] struct JOBOBJECT_BASIC_LIMIT_INFORMATION { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public int LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public int ActiveProcessLimit; public UIntPtr Affinity; public int PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential)] struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)] struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION { public JOBOBJECT_BASIC_LIMIT_INFORMATION Basic; public IO_COUNTERS Io; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }
    static IntPtr _job;
    /// <summary>radarnite-play.exe runs inside the 3.0 launcher's session job, which every process it starts inherits:
    /// no job of its own (its KILL_ON_JOB_CLOSE would end the game when the helper exits).</summary>
    public static bool InheritJob;
    public static void AddToJob(int pid)
    {
        if (InheritJob) return;
        if (_job == IntPtr.Zero)
        {
            _job = CreateJobObjectW(IntPtr.Zero, null);
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION { Basic = { LimitFlags = 0x2000 /*KILL_ON_JOB_CLOSE*/ } };
            SetInformationJobObject(_job, 9, ref info, Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>());
        }
        var h = OpenProcess(0x1F0FFF, false, pid);
        if (h != IntPtr.Zero) { AssignProcessToJobObject(_job, h); CloseHandle(h); }
    }
}

/// <summary>Downloads the data a new build needs to list its cosmetics (AES keys and type mappings). Data only, from
/// public archives on GitHub; nothing is executed. Done by the launcher (not by the game, which is firewalled).</summary>
public static class BuildData
{
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromMinutes(5) };
    static BuildData() => Http.DefaultRequestHeaders.UserAgent.ParseAdd("fn-offline-launcher/1.0");

    public static async Task<string> FetchAsync(BuildInfo b, Action<string> log)
    {
        var off = Paths.Offline(b.Dir);
        var aes = Path.Combine(off, "aes.json");
        if (!File.Exists(aes))
        {
            try
            {
                // The archive names files "3.60", "17.50", "2.4.2"...: try the version as read and its two-digit form.
                string? s = null, got = null;
                var names = new[] { b.Version, $"{b.Major}.{b.Minor:00}", $"{b.Major}.{b.Minor}0", $"{b.Major}.{b.Minor}.0" }.Distinct();
                foreach (var n in names)
                {
                    try { s = await Http.GetStringAsync($"https://raw.githubusercontent.com/dippyshere/fortnite-aes-archive/master/api/archive/{n}.json"); got = n; break; }
                    catch { }
                }
                if (s == null) throw new Exception("not in the archive (tried " + string.Join(", ", names) + ")");
                File.WriteAllText(aes, s); log($"AES keys for {b.Version} downloaded (dippyshere/fortnite-aes-archive, {got}.json).");
            }
            catch (Exception e) { log($"No AES keys for {b.Version} ({e.Message}). Not needed if the build is not encrypted; otherwise put aes.json in {off}."); }
        }
        var usmap = Path.Combine(off, "mappings.usmap");
        if (!File.Exists(usmap) && b.Major >= 11) // unversioned packages (mappings needed) from ~S11
        {
            foreach (var (repo, branch) in new[] { ("taiothegamer-tech/Fortnite-Mappings-AES-Keys", "main") })
            {
                try
                {
                    var tree = JsonNode.Parse(await Http.GetStringAsync($"https://api.github.com/repos/{repo}/git/trees/{branch}?recursive=1"))!["tree"]!.AsArray();
                    var hit = tree.Select(t => t!["path"]!.GetValue<string>())
                        .FirstOrDefault(p => p.EndsWith(".usmap") && p.Contains($"Release-{b.Version}-CL-{b.Cl}"))
                        ?? tree.Select(t => t!["path"]!.GetValue<string>()).FirstOrDefault(p => p.EndsWith(".usmap") && p.Contains($"Release-{b.Version}-"));
                    if (hit == null) continue;
                    var url = $"https://raw.githubusercontent.com/{repo}/{branch}/" + string.Join('/', hit.Split('/').Select(Uri.EscapeDataString));
                    File.WriteAllBytes(usmap, await Http.GetByteArrayAsync(url));
                    log($"Mappings for {b.Version} downloaded ({repo}).");
                    break;
                }
                catch (Exception e) { log($"Mappings: {repo}: {e.Message}"); }
            }
            if (!File.Exists(usmap)) log($"No mappings for {b.Version}: put mappings.usmap in {off} (S11+ cosmetics cannot be read without them).");
        }
        return off;
    }

    /// <summary>Why the last generation failed (shown in the toast and Activity); null when it worked.</summary>
    public static string? LastCosmeticsError;

    public static async Task<bool> GenerateCosmeticsAsync(BuildInfo b, Action<string> log)
    {
        var off = Paths.Offline(b.Dir);
        var logFile = Path.Combine(off, "cosmetics-gen.log");
        var lines = new List<string>();
        void L(string s) { lock (lines) lines.Add($"{DateTime.Now:HH:mm:ss} {s}"); log(s); }
        LastCosmeticsError = null;
        try
        {
            await FetchAsync(b, L);
            var args = $"--build \"{b.Dir}\" --out \"{Path.Combine(off, "cosmetics.json")}\" --engine {b.Engine} --version {b.Version}";
            if (File.Exists(Path.Combine(off, "aes.json"))) args += $" --aes \"{Path.Combine(off, "aes.json")}\"";
            if (File.Exists(Path.Combine(off, "mappings.usmap"))) args += $" --mappings \"{Path.Combine(off, "mappings.usmap")}\"";
            if (!File.Exists(Paths.CosmeticsGen)) { LastCosmeticsError = "launcher\\tools\\cosmetics-gen\\fn-cosmetics-gen.exe is missing"; return false; }
            L($"fn-cosmetics-gen {args}");
            var psi = new ProcessStartInfo(Paths.CosmeticsGen, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            using var p = Process.Start(psi)!;
            try { p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch { }
            string? lastErr = null;
            p.OutputDataReceived += (_, e) => { if (e.Data != null) L(e.Data); };
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) { lastErr = e.Data; L(e.Data); } };
            p.BeginOutputReadLine(); p.BeginErrorReadLine();
            await p.WaitForExitAsync();
            if (p.ExitCode == 0) return true;
            LastCosmeticsError = p.ExitCode switch
            {
                3 => "the build's Paks could not be opened: AES key missing or wrong (put aes.json in the build's offline folder)",
                4 => "no cosmetics found in the Paks" + (b.Major >= 11 && !File.Exists(Path.Combine(off, "mappings.usmap")) ? " (mappings.usmap missing)" : ""),
                _ => lastErr ?? $"the generator exited with code {p.ExitCode}",
            };
            return false;
        }
        catch (Exception e) { LastCosmeticsError = e.Message; L("Error: " + e); return false; }
        finally
        {
            if (LastCosmeticsError != null) L("Failed: " + LastCosmeticsError);
            try { lock (lines) File.WriteAllLines(logFile, lines); } catch { }
        }
    }

    /// <summary>Adds a build from a downloaded zip: extracted (one thread, below-normal priority) to &lt;root&gt;\&lt;version&gt;.</summary>
    public static async Task<BuildInfo?> AddZipAsync(string zip, Action<string> log)
    {
        var tmp = Path.Combine(Paths.Root, "_extrayendo_" + DateTime.Now.ToString("yyyyMMddHHmmss"));
        await Task.Run(() =>
        {
            Thread.CurrentThread.Priority = ThreadPriority.BelowNormal;
            using var z = ZipFile.OpenRead(zip);
            long total = z.Entries.Sum(e => e.Length), done = 0; int lastPct = -1;
            foreach (var e in z.Entries)
            {
                var dest = Path.GetFullPath(Path.Combine(tmp, e.FullName));
                if (!dest.StartsWith(tmp, StringComparison.OrdinalIgnoreCase)) continue; // no path traversal
                if (e.FullName.EndsWith("/")) { Directory.CreateDirectory(dest); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                e.ExtractToFile(dest, true);
                done += e.Length;
                var pct = (int)(done * 100 / Math.Max(1, total));
                if (pct != lastPct && pct % 5 == 0) { lastPct = pct; log($"Extracting... {pct} %"); }
            }
        });
        var buildRoot = Directory.EnumerateDirectories(tmp, "FortniteGame", SearchOption.AllDirectories).Select(d => Directory.GetParent(d)!.FullName).FirstOrDefault();
        if (buildRoot == null) { log("The zip does not contain a build (no FortniteGame folder)."); return null; }
        return Register(buildRoot, log, move: true, cleanup: tmp);
    }

    /// <summary>Adds a build from a folder: moved if it is on the same drive, otherwise copied.</summary>
    public static async Task<BuildInfo?> AddFolderAsync(string folder, Action<string> log)
    {
        var root = File.Exists(Paths.Shipping(folder)) ? folder
            : Directory.EnumerateDirectories(folder, "FortniteGame", SearchOption.AllDirectories).Select(d => Directory.GetParent(d)!.FullName).FirstOrDefault();
        if (root == null || !File.Exists(Paths.Shipping(root))) { log("That folder has no FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe."); return null; }
        if (Paths.IsUnder(root, Paths.Root) && Directory.GetParent(root)!.FullName.Equals(Paths.Root, StringComparison.OrdinalIgnoreCase))
            return BuildInfo.Read(root);
        var same = Path.GetPathRoot(root)!.Equals(Path.GetPathRoot(Paths.Root), StringComparison.OrdinalIgnoreCase);
        if (same) return Register(root, log, move: true, cleanup: null);
        var tmp = Path.Combine(Paths.Root, "_copiando_" + DateTime.Now.ToString("yyyyMMddHHmmss"));
        await Task.Run(() =>
        {
            Thread.CurrentThread.Priority = ThreadPriority.BelowNormal;
            foreach (var f in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
            {
                var dest = Path.Combine(tmp, Path.GetRelativePath(root, f));
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                File.Copy(f, dest, true);
            }
        });
        log("Copy finished.");
        return Register(tmp, log, move: true, cleanup: null);
    }

    static BuildInfo? Register(string buildRoot, Action<string> log, bool move, string? cleanup)
    {
        var v = BuildInfo.ScanVersion(Paths.Shipping(buildRoot));
        var m = v == null ? null : Regex.Match(v, @"Release-([\d.]+)-CL");
        var name = m is { Success: true } ? m.Groups[1].Value : "build-" + DateTime.Now.ToString("yyyyMMddHHmm");
        var dest = Path.Combine(Paths.Root, name);
        if (Directory.Exists(dest)) dest += "-" + DateTime.Now.ToString("HHmmss");
        Directory.Move(buildRoot, dest);
        if (cleanup != null && Directory.Exists(cleanup)) { try { Directory.Delete(cleanup, true); } catch { } }
        log($"Build {v ?? "?"} added at {dest}");
        return BuildInfo.Read(dest);
    }
}
