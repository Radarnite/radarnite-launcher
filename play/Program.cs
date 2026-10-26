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
///       [--late-game 0] [--bots-ai 0] [--autostart 60] [--bot-skill 6]
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
        if (args.Length == 1 && args[0] == "--selftest-audio") return SelfTestAudio();
        if (args.Length == 1 && args[0] == "--selftest-audio-tone") return SilentTone();
        var o = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < args.Length; i++)
        {
            if (!args[i].StartsWith("--") || i + 1 >= args.Length) { Out("error", $"radarnite-play: bad argument '{args[i]}' (usage: --build <dir> [--option value]...)"); return 2; }
            o[args[i][2..]] = args[++i];
        }
        string[] known = ["build", "launcher-dir", "port", "name", "last-playlist", "practice-playlist", "bots", "infinite-materials",
                          "infinite-ammo", "late-game", "bots-ai", "autostart", "bot-skill"];
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
            BotSkill = Math.Clamp(Int("bot-skill", def.BotSkill), 1, 10),
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
        if (matchOk) await s.MatchLoop(off);   // returns when the client exits
        if (s.AudioRestore is { } audio) await audio;   // the sound check ends with the client or after its first minutes
        return 0;
    }

    // Developer: Audio.Restore against what a test tool leaves behind. A copy of this exe plays silence (its own audio
    // session, nothing audible); its session is muted at volume 0 the way the fnviewer test tooling mutes the game; that
    // exe runs again (Windows brings the mute back, per exe path); Audio.Restore, as the game start runs it, must leave
    // that session unmuted at a volume above 0. Every process it starts is killed before it returns.
    static int SelfTestAudio()
    {
        var self = Environment.ProcessPath ?? "";
        System.Diagnostics.Process? tone = null;
        List<Audio.State> Now(System.Diagnostics.Process p) => Audio.OnComThread(() => Audio.Sessions([(uint)p.Id])).GetAwaiter().GetResult();
        bool WaitSession(System.Diagnostics.Process p)
        {
            for (var i = 0; i < 60; i++) { if (p.HasExited) return false; if (Now(p).Count > 0) return true; Thread.Sleep(250); }
            return false;
        }
        System.Diagnostics.Process Tone() => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(self, "--selftest-audio-tone") { UseShellExecute = false, CreateNoWindow = true })!;
        try
        {
            tone = Tone();
            if (!WaitSession(tone)) { Out("error", "audio selftest FAILED: the silent player opened no audio session"); return 1; }
            var set = Audio.OnComThread(() => Audio.Set([(uint)tone.Id], true, 0f)).GetAwaiter().GetResult();
            Out("log", $"test tool: muted {set} session(s) of pid {tone.Id} at volume 0");
            tone.Kill(true); tone.WaitForExit(5000); tone.Dispose(); tone = null;

            tone = Tone();
            if (!WaitSession(tone)) { Out("error", "audio selftest FAILED: the second run opened no audio session"); return 1; }
            var before = Now(tone);
            var persisted = before.All(x => x.Muted);
            Out("log", $"second run pid {tone.Id}: {string.Join(", ", before)} (Windows {(persisted ? "kept" : "did not keep")} the mute)");
            if (!persisted) Audio.OnComThread(() => Audio.Set([(uint)tone.Id], true, 0f)).GetAwaiter().GetResult();

            var lines = new List<string>();
            Audio.Restore(tone, l => { lock (lines) lines.Add(l); }).GetAwaiter().GetResult();
            foreach (var l in lines) Out("log", "launcher log: " + l);
            var after = Now(tone);
            var ok = after.Count > 0 && after.All(x => !x.Muted && x.Volume > 0f) && lines.Count > 0;
            Out(ok ? "log" : "error", $"audio selftest {(ok ? "ok" : "FAILED")}: after Audio.Restore {string.Join(", ", after)}");
            return ok ? 0 : 1;
        }
        finally { if (tone != null) try { tone.Kill(true); tone.WaitForExit(5000); } catch { } }
    }

    [System.Runtime.InteropServices.DllImport("winmm")] static extern bool PlaySoundW(IntPtr sound, IntPtr module, uint flags);

    // Plays one second of digital silence in a loop for up to 2 minutes: an audio session with nothing audible in it.
    static int SilentTone()
    {
        const int rate = 8000, bytes = rate * 2;
        var wav = new byte[44 + bytes];
        void Put(int at, string s) => Encoding.ASCII.GetBytes(s).CopyTo(wav, at);
        void U32(int at, int v) => BitConverter.GetBytes(v).CopyTo(wav, at);
        void U16(int at, short v) => BitConverter.GetBytes(v).CopyTo(wav, at);
        Put(0, "RIFF"); U32(4, 36 + bytes); Put(8, "WAVE"); Put(12, "fmt "); U32(16, 16); U16(20, 1); U16(22, 1);
        U32(24, rate); U32(28, rate * 2); U16(32, 2); U16(34, 16); Put(36, "data"); U32(40, bytes);
        var h = System.Runtime.InteropServices.GCHandle.Alloc(wav, System.Runtime.InteropServices.GCHandleType.Pinned);
        try
        {
            // SND_ASYNC | SND_NODEFAULT | SND_MEMORY | SND_LOOP
            if (!PlaySoundW(h.AddrOfPinnedObject(), IntPtr.Zero, 0x1 | 0x2 | 0x4 | 0x8)) return 1;
            Thread.Sleep(TimeSpan.FromMinutes(2));
            PlaySoundW(IntPtr.Zero, IntPtr.Zero, 0);
            return 0;
        }
        finally { h.Free(); }
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
