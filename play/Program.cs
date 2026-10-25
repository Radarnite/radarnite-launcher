using System.Text;
using System.Text.Json;

namespace FnOffline;

/// <summary>
/// radarnite-play.exe: starts the game client of one build exactly as the 2.x launcher does when you press Play
/// (Session.StartClientAsync: redirect DLL loaded at process start, the suspended stubs, the pak patch), for a backend
/// the 3.0 launcher already started. Then, when the build has a local match, it keeps 2.x's on-demand match server
/// (Session.MatchLoop) until the client exits; otherwise it exits as soon as the client runs.
///
///   radarnite-play.exe --build &lt;dir&gt; [--launcher-dir &lt;dir&gt;] [--port 3551] [--name &lt;displayName&gt;]
///       [--last-playlist &lt;code&gt;] [--practice-playlist &lt;path&gt;] [--bots 20] [--infinite-materials 1] [--infinite-ammo 0]
///       [--late-game 0] [--bots-ai 0] [--autostart 60]
///
/// stdout, UTF-8, one event per line:
///   log: &lt;text&gt;               an Activity line
///   stage: &lt;stage&gt; &lt;state&gt;     game|server, run|ok|bad|"" (Session.Stage)
///   match: &lt;phase&gt; &lt;detail&gt;    the local match (Session.Match)
///   playlist: &lt;code&gt;          the playlist asked for in the game: save it as config.json "lastPlaylist"
///   pid: &lt;n&gt;                  the client runs (redirect confirmed)
///   error: &lt;text&gt;             why it did not start; exit code 1 (2 = bad arguments)
/// It never stops anything it did not start and never writes config.json: the launcher owns both.
/// </summary>
static class PlayProgram
{
    static readonly object Gate = new();
    static TextWriter? _out;

    static void Out(string kind, string? text)
    {
        var line = $"{kind}: {(text ?? "").Replace("\r\n", " ").Replace('\r', ' ').Replace('\n', ' ')}";
        lock (Gate) { try { _out?.WriteLine(line); } catch { } }   // the launcher went away: nothing to report to
    }

    static async Task<int> Main(string[] args)
    {
        try { _out = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" }; } catch { }
        try { return await Run(args); }
        catch (Exception e) { Out("error", "radarnite-play: " + e.Message); return 1; }
    }

    static async Task<int> Run(string[] args)
    {
        if (args.Length == 1 && args[0] == "--selftest-json") return SelfTestJson();
        var o = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < args.Length; i++)
        {
            if (!args[i].StartsWith("--") || i + 1 >= args.Length) { Out("error", $"radarnite-play: bad argument '{args[i]}' (usage: --build <dir> [--option value]...)"); return 2; }
            o[args[i][2..]] = args[++i];
        }
        string[] known = ["build", "launcher-dir", "port", "name", "last-playlist", "practice-playlist", "bots", "infinite-materials",
                          "infinite-ammo", "late-game", "bots-ai", "autostart"];
        if (o.Keys.FirstOrDefault(k => !known.Contains(k, StringComparer.OrdinalIgnoreCase)) is { } unknown) { Out("error", $"radarnite-play: unknown option --{unknown}"); return 2; }
        if (!o.TryGetValue("build", out var dir) || string.IsNullOrWhiteSpace(dir)) { Out("error", "radarnite-play: --build <dir> is required"); return 2; }
        if (o.TryGetValue("launcher-dir", out var ld) && !string.IsNullOrWhiteSpace(ld)) Paths.LauncherDir = Path.GetFullPath(ld).TrimEnd('\\');

        var def = new Config();
        int Int(string k, int d) => o.TryGetValue(k, out var v) && int.TryParse(v, out var n) ? n : d;
        bool Bool(string k, bool d) => o.TryGetValue(k, out var v) ? v is "1" or "true" or "True" : d;
        string Str(string k, string d) => o.TryGetValue(k, out var v) ? v : d;
        var c = new Config
        {
            DisplayName = Str("name", "player"), Port = Int("port", def.Port), LastPlaylist = Str("last-playlist", def.LastPlaylist),
            PracticePlaylist = Str("practice-playlist", def.PracticePlaylist), Bots = Int("bots", def.Bots),
            InfiniteMaterials = Bool("infinite-materials", def.InfiniteMaterials), InfiniteAmmo = Bool("infinite-ammo", def.InfiniteAmmo),
            LateGame = Bool("late-game", def.LateGame), BotsAI = Bool("bots-ai", def.BotsAI), AutoStartSeconds = Int("autostart", def.AutoStartSeconds),
        };

        var b = BuildInfo.Read(dir, out var why);
        if (b == null) { Out("error", $"Not a usable build: {dir} ({why ?? "not a build"})."); return 1; }
        if (!File.Exists(Paths.RedirectDll)) { Out("error", $"A launcher file is missing: {Paths.RedirectDll}."); return 1; }

        // Everything started from here inherits the launcher's session job (KILL_ON_JOB_CLOSE): no job of our own,
        // or the client would die with this process.
        Native.InheritJob = true;
        var matchOk = b.Match.s != Support.No && File.Exists(Paths.GameServerDll);
        Session? s = null;
        // Session.Fail sets Failure, then logs it: that line goes out once, as "error:" (the launcher logs it)
        s = new Session(b, c, l => { if (l != s?.Failure) Out("log", l); })
        {
            Stage = (k, v) => Out("stage", $"{k} {v}"),
            Match = (k, d) => Out("match", $"{k} {d}"),
            SaveConfig = cfg => Out("playlist", cfg.LastPlaylist),
        };
        var off = Paths.Offline(b.Dir);
        if (!await s.StartClientAsync(off))
        {
            // 2.x stops the session here; the client is the only thing this process started that is still running
            if (s.ClientPid != 0) try { System.Diagnostics.Process.GetProcessById(s.ClientPid).Kill(true); } catch { }
            Out("error", s.Failure ?? "The game did not start.");
            return 1;
        }
        Out("pid", s.ClientPid.ToString());
        if (!matchOk) return 0;
        await s.MatchLoop(off);   // returns when the client exits
        return 0;
    }

    // Developer: the reflection-based System.Text.Json calls of Session.MatchLoop/PostReady (a positional record and an
    // anonymous object) still work in the trimmed exe.
    sealed record WantedProbe(string playlist, int seq);
    static int SelfTestJson()
    {
        var w = JsonSerializer.Deserialize<WantedProbe>("{\"playlist\":\"playlist_defaultduo\",\"seq\":3}");
        var a = JsonSerializer.Serialize(new { address = "127.0.0.1", port = 7777, playlist = "playlist_defaultduo" });
        var ok = w is { playlist: "playlist_defaultduo", seq: 3 } && a == "{\"address\":\"127.0.0.1\",\"port\":7777,\"playlist\":\"playlist_defaultduo\"}";
        Out(ok ? "log" : "error", $"json selftest {(ok ? "ok" : "FAILED")}: {w} {a}");
        return ok ? 0 : 1;
    }
}
