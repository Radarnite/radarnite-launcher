using System.Diagnostics;
using System.Net.Http;
using System.Text;

namespace FnOffline;

/// <summary>One play session: checks, backend, optional local game server, client. Everything it starts is in a
/// job object, so closing the launcher (or the client exiting) stops all of it.</summary>
public sealed class Session
{
    readonly BuildInfo _b;
    readonly Config _c;
    readonly Action<string> _log;
    readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(3) };
    Process? _backend, _server, _client;
    public bool Running => _client is { HasExited: false };
    /// <summary>Launch progress for the UI: (stage, state) with stage backend|server|game and state run|ok|bad|skip.</summary>
    public Action<string, string>? Stage;
    /// <summary>Live state of the local match, read from the server's own logs: (phase, detail). Phases: waiting (server
    /// listening, nobody joined), joined, warmup, bus, storm, ended, down (the match server process is gone).</summary>
    public Action<string, string>? Match;
    /// <summary>Why the last start failed (shown in the launch bar).</summary>
    public string? Failure;
    void Fail(string why) { Failure = why; _log(why); }

    public Session(BuildInfo b, Config c, Action<string> log) { _b = b; _c = c; _log = log; }

    // Launch flags per era. The flags, the "caldera" token's anticheat provider and the fake (suspended) anticheat stub must agree:
    // 17.50 with an EasyAntiCheat caldera + -fromfl=be made the client show "Fortnite was not started correctly"
    // (LogAppES: closing code 24). The tokens are public samples, not credentials.
    const string CalderaBE16 = "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJhY2NvdW50X2lkIjoiOWM1MDY1MTEwYzdhNGQ3MDk1ODYyZGE1ZWU4MTU5NjIiLCJnZW5lcmF0ZWQiOjE3MTQ2MDQzOTgsImNhbGRlcmFHdWlkIjoiZWQ3NGZhNWMtZmNmYy00YWM1LWJlMTktZmY1ODQ1MDI0MzU0IiwiYWNQcm92aWRlciI6IkJhdHRsRXllIiwibm90ZXMiOiIiLCJwcmUiOmZhbHNlLCJmYWxsYmFjayI6ZmFsc2V9.YCfBeSzOKM6vMZpe7SXiOxlTdnbFd2Ve2WQv8SAUDHnaDDpSvmZhQKIsjHFNAIYL5t65Pl7zb2yVDVspE6pQ-g";
    const string CalderaBEOld = "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJhY2NvdW50X2lkIjoiOWM1MDY1MTEwYzdhNGQ3MDk1ODYyZGE1ZWU4MTU5NjIiLCJnZW5lcmF0ZWQiOjE3MTM3MDUwODUsImNhbGRlcmFHdWlkIjoiZWIzYzM4YjctM2U4Yy00NDZiLWE4NTYtMjM5OTUzMTc3ZDRiIiwiYWNQcm92aWRlciI6IkJhdHRsRXllIiwibm90ZXMiOiIiLCJwcmUiOmZhbHNlLCJmYWxsYmFjayI6ZmFsc2V9.QD69Oonbv7SIcXhYHewzQvh4ymnESQ1LRFQh8_Ufvj8M9bwpl_AbY9wpVRX2lz0x-xslYS40Rf4L34GZJpb20A";

    /// <summary>Game arguments for this build; also which build stub must exist (suspended) as the fake anticheat.</summary>
    (string args, string fakeAc) LaunchArgs(string login)
    {
        string a, ac;
        if (_b.Major > 15)
        {
            var fl = _b.Version switch { "16.00" => "5dh74c635862g575778132fb", "16.10" => "b234a7cc879f663d845c54f9", "16.20" => "c98d8b7a7239f78e234a9abc",
                                         "16.30" => "f45a67dd982a77b9538d1e4f", "16.40" => "a12b789c45f67e8912d3c4ff", _ => "3db3ba5dcbd2e16703f3978d" };
            a = "-obfuscationid=PM3G4r-oF6DIG-fbJUON8rECU73FWA -epicapp=Fortnite -epicenv=Prod -EpicPortal -epiclocale=en -epicsandboxid=fn " +
                $"-noeac -fromfl=be -caldera={CalderaBE16} -fltoken={fl} -skippatchcheck";
            ac = "FortniteClient-Win64-Shipping_BE.exe";
        }
        else
        {
            a = "-obfuscationid=2oaCH8bqszvAC9LdV0CSZoTD5AxLCQ -EpicPortal -epicapp=Fortnite -epicenv=Prod -epiclocale=en-us -epicportal " +
                $"-noeac -nobe -fromfl=be -fltoken=h1cdhchd10150221h130eB56 -caldera={CalderaBEOld} -skippatchcheck";
            ac = "FortniteClient-Win64-Shipping_EAC.exe";
        }
        // -userdir: keep this build's Saved folder (logs, settings, downloads) inside the build, never in the real
        // Fortnite's %LOCALAPPDATA%\FortniteGame.
        a += $" -nocrashreports -AUTH_LOGIN={login} -AUTH_PASSWORD=offline -AUTH_TYPE=epic";
        // XMPP is pointed at the local backend by a hotfix file the backend serves (cloudstorage/system DefaultEngine.ini);
        // this build ignores -ini: command-line overrides.
        return (a, ac);
    }

    /// <summary>Everything that must hold before anything is started. Returns null when OK.</summary>
    public static string? Preflight(BuildInfo b, bool needMatch)
    {
        foreach (var p in Process.GetProcesses())
        {
            string name = p.ProcessName, path = Native.ImagePath(p.Id) ?? "";
            if (name.StartsWith("EpicGamesLauncher", StringComparison.OrdinalIgnoreCase) || name.Equals("EpicWebHelper", StringComparison.OrdinalIgnoreCase))
                return "The Epic Games Launcher is open: quit it completely (also from the tray) and try again.";
            if (name.StartsWith("FortniteClient", StringComparison.OrdinalIgnoreCase) || name.StartsWith("FortniteLauncher", StringComparison.OrdinalIgnoreCase))
            {
                if (path == "" || !Paths.IsUnder(path, Paths.Root))
                    return "The real Fortnite is running: close it before playing offline.";
                return "An offline build is already running: use Stop first.";
            }
        }
        if (b.Lobby.s == Support.No) return $"Fortnite {b.Version} is not supported: {b.Lobby.why}.";
        if (!File.Exists(Paths.Node) || !File.Exists(Paths.Backend) || !File.Exists(Paths.RedirectDll)) return "A launcher file is missing (node, backend or redirect).";
        if (Firewall.Check(b.Dir) is { } fw) return $"Firewall: {fw}. Press Create firewall rules (asks for administrator permission).";
        return null;
    }

    readonly DateTime _started = DateTime.Now;
    public DateTime StartedAt => _started;

    public async Task<bool> StartAsync()
    {
        // The mode is chosen inside the game (lobby -> CHANGE -> Playground / Solo / Duos / Trios / Squads): the match
        // server is started on demand for the playlist the client asks for (see MatchLoop).
        var matchOk = _b.Match.s != Support.No && File.Exists(Paths.GameServerDll);
        if (Preflight(_b, matchOk) is { } why) { Fail("Not starting: " + why); return false; }
        var off = Paths.Offline(_b.Dir);
        if (!File.Exists(Path.Combine(off, "cosmetics.json"))) _log("Warning: this build has no cosmetics list yet (press Generate cosmetics): only the defaults will show.");
        await FitOwnPaks(off);

        // 1. backend (127.0.0.1 only)
        Stage?.Invoke("backend", "run");
        _backend = Start(Paths.Node, $"\"{Paths.Backend}\" --build \"{_b.Dir}\" --config \"{Paths.Config}\" --port {_c.Port}{(matchOk ? "" : " --no-match")}", null, hidden: true);
        if (!await WaitHttp($"http://127.0.0.1:{_c.Port}/offline/health", 15)) { Stage?.Invoke("backend", "bad"); Fail("The backend did not start (see offline\\backend.log)."); Stop(); return false; }
        _log($"Backend ready on 127.0.0.1:{_c.Port} (local account \"{_c.DisplayName}\").");
        Stage?.Invoke("backend", "ok");
        if (_b.Match.s == Support.No) _log($"No local match for {_b.Version}: {_b.Match.why}. Lobby only.");
        else if (!matchOk) _log("Local match: launcher\\gameserver\\radarnite-server.dll is missing. Lobby only.");
        else if (_b.Match.s == Support.Experimental) _log($"Local match is EXPERIMENTAL on {_b.Version}: {_b.Match.why}.");

        // 2. client
        var env = GameEnv(Path.Combine(off, "redirect-client.log"), "sandbox-client");
        Stage?.Invoke("game", "run");
        var (clientArgs, clientAc) = LaunchArgs($"{San(_c.DisplayName)}@offline.local");
        _client = StartGame(clientArgs, env, hidden: false);
        if (_client == null) { Fail("Could not start the game process."); Stop(); return false; }
        StartFakeLaunchers(clientAc);
        if (!await InjectRedirect(_client, Path.Combine(off, "redirect-client.log"))) { Stop(); return false; }
        LoadPakPatch(_client, "Game");
        Stage?.Invoke("game", "ok");
        _log($"Fortnite {_b.Version} started. Pick a mode in the game (CHANGE in the lobby) and press PLAY. Closing the game stops everything.");

        // 3. match server on demand
        if (matchOk) _ = Task.Run(() => MatchLoop(off));
        return true;
    }

    /// <summary>What the match server runs for a playlist the client asked for. Practice (Playground) runs the
    /// configured practice playlist with infinite materials and ammo and no bots; the Battle Royale playlists run
    /// themselves with the configured bots filling the bus. Unknown codes fall back to Solo.</summary>
    sealed record ServerPlan(string Code, string Playlist, bool Practice, int Bots);
    ServerPlan PlanFor(string code)
    {
        code = (code ?? "").ToLowerInvariant();
        if (code.Contains("playground") || code.Contains("creative")) return new(code, _c.PracticePlaylist, true, 0);
        var name = code switch
        {
            "playlist_defaultduo" => "Playlist_DefaultDuo", "playlist_trios" => "Playlist_Trios", "playlist_defaultsquad" => "Playlist_DefaultSquad",
            _ => "Playlist_DefaultSolo",
        };
        return new(code == "" ? "playlist_defaultsolo" : code, $"/Game/Athena/Playlists/{name}.{name}", false, Math.Clamp(_c.Bots, 0, 99));
    }

    ServerPlan? _plan;           // what the current match server runs
    bool _serverReady;           // it listens and the backend knows it
    string _phase = "";          // its match phase (WatchMatch): "waiting" = nobody joined yet, reusable
    readonly List<int> _serverAux = new(); // its suspended stub processes
    sealed record Wanted(string playlist, int seq);

    /// <summary>Follows the client's matchmaking requests (backend /offline/wanted) and keeps a match server for the
    /// requested playlist: a fresh server of the same playlist is reused, anything else is replaced. The last
    /// playlist is remembered and pre-started at the next launch so the first PLAY is quick.</summary>
    async Task MatchLoop(string off)
    {
        var handled = 0;
        var pre = PlanFor(string.IsNullOrEmpty(_c.LastPlaylist) ? "playlist_defaultsolo" : _c.LastPlaylist);
        _log($"Preparing a match server for {Pretty(pre)}.");
        await StartServerAsync(off, pre);
        while (_client is { HasExited: false })
        {
            Wanted? w = null;
            try { w = System.Text.Json.JsonSerializer.Deserialize<Wanted>(await _http.GetStringAsync($"http://127.0.0.1:{_c.Port}/offline/wanted")); } catch { }
            if (w != null && w.seq != handled && w.seq > 0)
            {
                handled = w.seq;
                var plan = PlanFor(w.playlist);
                if (_c.LastPlaylist != plan.Code) { _c.LastPlaylist = plan.Code; try { _c.Save(); } catch { } }
                if (_server is { HasExited: false } && _serverReady && _plan?.Code == plan.Code && _phase == "waiting")
                {
                    _log($"Match: {Pretty(plan)} (server already waiting).");
                    await PostReady(plan);
                }
                else
                {
                    _log($"Match: {Pretty(plan)} requested, starting its match server (about a minute)...");
                    await StopServerAsync();
                    await StartServerAsync(off, plan);
                }
            }
            else if (_server is { HasExited: true } && _serverReady) { _serverReady = false; try { await _http.PostAsync($"http://127.0.0.1:{_c.Port}/offline/server-down", null); } catch { } }
            await Task.Delay(1000);
        }
    }

    static string Pretty(ServerPlan p) => p.Practice ? "Practice (infinite materials)" : p.Code switch
    {
        "playlist_defaultduo" => "Duos", "playlist_trios" => "Trios", "playlist_defaultsquad" => "Squads", _ => "Solo",
    } + (p.Bots > 0 ? $" with {p.Bots} bots" : "");

    Task PostReady(ServerPlan plan) => _http.PostAsync($"http://127.0.0.1:{_c.Port}/offline/server-ready",
        new StringContent(System.Text.Json.JsonSerializer.Serialize(new { address = "127.0.0.1", port = 7777, playlist = plan.Code })));

    async Task StopServerAsync()
    {
        _serverReady = false; _phase = "";
        try { await _http.PostAsync($"http://127.0.0.1:{_c.Port}/offline/server-down", null); } catch { }
        var s = _server; _server = null;
        if (s is { HasExited: false }) { _stoppingServer = true; try { s.Kill(true); await s.WaitForExitAsync(); } catch { } await Task.Delay(2000); _stoppingServer = false; } // let its files and port 7777 go
        foreach (var pid in _serverAux) try { Process.GetProcessById(pid).Kill(); } catch { }
        _serverAux.Clear();
        Stage?.Invoke("server", "");
    }
    bool _stoppingServer;

    async Task StartServerAsync(string off, ServerPlan plan)
    {
        var log = Path.Combine(off, "sandbox-server", "LocalAppData", "FortniteGame", "Saved", "Logs", "FortniteGame.log"); // the server Saved folder (sandbox)
        var dllLog = Path.Combine(Paths.Win64(_b.Dir), "radarnite-server.log"); // the match server DLL writes its own log in its working dir
        try { File.Delete(log); } catch { }
        try { File.Delete(dllLog); } catch { }
        // Only what THIS server writes counts: when a previous server was just replaced its logs may still exist
        // (deleting fails while a handle is open), and their "Region " / "Listening on port" lines made the
        // launcher inject the match server DLL into the new process at once (crash during its init) and report it ready
        // after one second, sending the client to a server that was not there.
        long pLog = FileLen(log), pDll = FileLen(dllLog);
        var started = DateTime.Now;
        var env = GameEnv(Path.Combine(off, "redirect-server.log"), "sandbox-server");
        var practice = plan.Practice;
        _plan = plan; _serverReady = false; _phase = "";
        env["FNOFFLINE_PLAYLIST"] = plan.Playlist;
        env["FNOFFLINE_BOTS"] = plan.Bots.ToString();
        env["FNOFFLINE_INFINITE_MATS"] = _c.InfiniteMaterials || practice ? "1" : "0";
        env["FNOFFLINE_INFINITE_AMMO"] = _c.InfiniteAmmo || practice ? "1" : "0";
        env["FNOFFLINE_LATEGAME"] = _c.LateGame ? "1" : "0";
        env["FNOFFLINE_BOTS_AI"] = !practice && _c.BotsAI ? "1" : "0";
        env["FNOFFLINE_AUTOSTART_SECONDS"] = Math.Max(0, _c.AutoStartSeconds).ToString();
        env["FNOFFLINE_AUTOSTART_MINPLAYERS"] = "1";
        if (_b.ServerPatches.Length > 0) env["FNOFFLINE_PATCHES"] = _b.ServerPatches;
        var (serverArgs, serverAc) = LaunchArgs("server@offline.local");
        _server = StartGame(serverArgs + $" -nullrhi -nosplash -nosound -windowed -ResX=320 -ResY=240 \"-ExecCmds=t.MaxFPS 30\"", env, hidden: true);
        if (_server == null) { _log("Warning: could not start the match server process."); return; }
        _serverAux.AddRange(StartFakeLaunchers(serverAc));
        try { _server.PriorityClass = ProcessPriorityClass.Normal; } catch { }
        // This build ignores -nullrhi (it still creates a D3D11 device): keep every window of the server hidden for its
        // whole life (game window, the server DLL's own window, error dialogs) and keep it tiny and at 30 fps.
        var serverPid = _server.Id;
        if (Environment.GetEnvironmentVariable("RADARNITE_DEBUG_NOHIDE") != "1")
            _ = Task.Run(async () => { while (_server is { HasExited: false }) { Native.HideWindowsOf(serverPid); await Task.Delay(500); } });
        if (!await InjectRedirect(_server, Path.Combine(off, "redirect-server.log"))) { _server = null; return; }
        LoadPakPatch(_server, "Match server");
        Stage?.Invoke("server", "run");
        _log("Match server starting (no rendering)...");
        // Inject the game server once the engine is up ("Region " line), then
        // tell the backend's matchmaker where to send the client when the server listens.
        var injected = false;
        var fresh = new StringBuilder();
        for (var t = 0; t < 600 && _server is { HasExited: false }; t++)
        {
            await Task.Delay(500);
            foreach (var l in ReadNew(log, ref pLog)) fresh.AppendLine(l);
            foreach (var l in ReadNew(dllLog, ref pDll)) fresh.AppendLine(l);
            var text = fresh.ToString();
            if (!injected && text.Contains("Region "))
            {
                injected = Native.Inject(_server.Id, Paths.GameServerDll);
                _log(injected ? "Match server: loaded." : "Match server: could not load radarnite-server.dll.");
                if (!injected) return;
            }
            // The server DLL logs "LogNet: Listening on port 7777!" (radarnite-server.log) when it calls World->Listen();
            // UE's own log has "UFortReplicationGraph is enabled for" once the server is joinable.
            if (injected && (text.Contains("Listening on port", StringComparison.OrdinalIgnoreCase)
                             || text.Contains("UFortReplicationGraph is enabled for", StringComparison.OrdinalIgnoreCase)))
            {
                try { await PostReady(plan); } catch { }
                _serverReady = true;
                Stage?.Invoke("server", "ok");
                _log($"Match server ready: {Pretty(plan)} on 127.0.0.1:7777.");
                _ = Task.Run(() => WatchMatch(log, dllLog));
                return;
            }
        }
        if (_server is { HasExited: true } && !_stoppingServer)
            _log($"Error: the match server for {Pretty(plan)} closed while starting (exit code {Native.ExitCodeOf(_exitHandles.GetValueOrDefault(_server.Id))}, after {(DateTime.Now - started).TotalSeconds:0} s).");
        Stage?.Invoke("server", "bad");
        _log("Warning: the match server did not get ready (see offline\\sandbox-server\\LocalAppData\\FortniteGame\\Saved\\Logs). The lobby still works.");
    }

    /// <summary>Environment of a game process. Its per-user folders (where the engine keeps Saved: logs, settings,
    /// downloads) are a sandbox inside the build: the redirect DLL answers SHGetKnownFolderPath with
    /// FNOFFLINE_SANDBOX (the engine ignores -userdir), and the environment variables point there too.</summary>
    Dictionary<string, string?> GameEnv(string redirectLog, string sandbox)
    {
        try { File.Delete(redirectLog); } catch { }
        var sb = Directory.CreateDirectory(Path.Combine(Paths.Offline(_b.Dir), sandbox)).FullName;
        return new()
        {
            ["FNOFFLINE_PORT"] = _c.Port.ToString(),
            ["FNOFFLINE_LOG"] = redirectLog,
            ["FNOFFLINE_SANDBOX"] = sb,
            // Old OpenSSL in these builds crashes on CPUs with SHA extensions (Ryzen): hide them, as the community launchers do.
            ["OPENSSL_ia32cap"] = ":~0x20000000",
            // 3.x-15.x builds check their own code: the redirect's 14-byte jmp made 14.30 and 3.6 ask the Epic launcher to
            // "verify" and crash (0xC0000005, no crash report) right after the engine started, client and server alike.
            // A hardware breakpoint on curl_easy_setopt changes no code (17.50 keeps the inline hook, which it tolerates).
            ["FNOFFLINE_CURL_HOOK"] = _b.Major <= 15 ? "hwbp" : null,
            // IoStore builds (UE 4.26+/UE5) verify each container's signature at mount; our own cosmetic containers
            // under ~mods are unsigned. The redirect neutralises that one check in memory so they mount (unsigned
            // containers are skipped past the check, as when signing is off; real signed containers stay validated).
            ["FNOFFLINE_IOSTORE_UNSIGNED"] = _b.UsesIoStore ? "1" : null,
            ["LOCALAPPDATA"] = Directory.CreateDirectory(Path.Combine(sb, "LocalAppData")).FullName,
            ["APPDATA"] = Directory.CreateDirectory(Path.Combine(sb, "RoamingAppData")).FullName,
            ["TEMP"] = Directory.CreateDirectory(Path.Combine(sb, "Temp")).FullName,
            ["TMP"] = Path.Combine(sb, "Temp"),
        };
    }

    /// <summary>The build's own FortniteLauncher.exe and BattlEye/EAC stubs, created SUSPENDED (they never execute):
    /// the client checks they exist. Nothing of the system's anticheat (services/drivers) is touched.</summary>
    List<int> StartFakeLaunchers(string fakeAc)
    {
        var pids = new List<int>();
        foreach (var name in new[] { fakeAc, "FortniteLauncher.exe" })
        {
            var exe = Path.Combine(Paths.Win64(_b.Dir), name);
            if (!File.Exists(exe)) continue;
            var pid = Native.StartSuspended(exe);
            if (pid != 0) { Native.AddToJob(pid); pids.Add(pid); }
        }
        return pids;
    }

    readonly Dictionary<int, IntPtr> _exitHandles = new();

    /// <summary>The game exe with fnredirect.dll loaded before any of its code runs (curl hook + folder sandbox).</summary>
    Process? StartGame(string args, Dictionary<string, string?> env, bool hidden)
    {
        var p = Native.StartWithDll(Paths.Shipping(_b.Dir), args, env, Paths.RedirectDll, hidden);
        if (p != null) { Native.AddToJob(p.Id); _exitHandles[p.Id] = Native.OpenForExitCode(p.Id); }
        return p;
    }

    /// <summary>Waits for the redirect DLL's log line (it was loaded at process start).</summary>
    async Task<bool> InjectRedirect(Process p, string log)
    {
        // Builds whose code is decrypted at run time (3.x, 14.x) report the hook a few seconds after start: up to 130 s.
        for (var i = 0; i < 520 && !ReadShared(log).Contains("OK:") && !ReadShared(log).Contains("UNSUPPORTED") && !p.HasExited; i++) await Task.Delay(250);
        if (p.HasExited) { Fail($"The game process exited at start (exit code {Native.ExitCodeOf(_exitHandles.GetValueOrDefault(p.Id))})."); return false; }
        if (ReadShared(log) == "" && !Native.Inject(p.Id, Paths.RedirectDll)) { Fail("Could not load fnredirect.dll into the game. Stopped."); return false; }
        return await CheckRedirect(log);
    }

    // With our own cosmetic paks 14.30 closed by itself (the log just stops, no crash report) after "Pak chunk signing
    // mismatch" warnings. fnpakpatch.dll (src\pakpatch: LargePakPatch's two patches, which LargePakPatch.dll itself could
    // not place on 14.30) turns the "unsafe environment" check and the "Memory pools allocations failed, exiting..." exit
    // into no-ops. It waits for the decrypted code itself and logs every step to <sandbox>\pakpatch.log.
    static readonly string[] PakPatchBuilds = { "14.30" };
    void LoadPakPatch(Process p, string who)
    {
        if (!PakPatchBuilds.Contains(_b.Version) || !File.Exists(Paths.PakPatchDll)) return;
        _log(Native.Inject(p.Id, Paths.PakPatchDll) ? $"{who}: pak patch loaded (see offline\\sandbox-*\\pakpatch.log)." : $"Warning: {who}: could not load fnpakpatch.dll.");
    }

    // Our own cosmetic paks (offline\custom-cosmetics.json) must match a signature file of the build, or 14.30 shows
    // "Corrupted data was found in game files" and quits. Importers that copy a .sig (older fn-port / import-skin runs)
    // undo that, so every launch of a classic-pak build checks them first and fits any that do not match
    // (tools\sigfit-installed.ps1 -> fn-modkit sigfit, FortnitePorting reports/pak-sigfit.md); a second or two when nothing changed.
    async Task FitOwnPaks(string off)
    {
        var script = Path.Combine(Paths.LauncherDir, "tools", "sigfit-installed.ps1");
        if (_b.UsesIoStore || !File.Exists(Path.Combine(off, "custom-cosmetics.json"))) return;
        if (!File.Exists(script)) { _log($"Warning: own paks NOT checked: {script} is missing (the game may stop with \"Corrupted data\")."); return; }
        var psi = new ProcessStartInfo("powershell.exe", $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\" -Build \"{_b.Dir}\"")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        try
        {
            using var p = Process.Start(psi)!;
            var err = p.StandardError.ReadToEndAsync();
            var lines = (await p.StandardOutput.ReadToEndAsync()).Split('\n').Select(l => l.Trim()).Where(l => l != "").ToList();
            await p.WaitForExitAsync();
            foreach (var l in lines.Where(l => l.StartsWith("fitted ") || l.StartsWith("FAILED "))) _log("Own paks: " + l);
            if (lines.LastOrDefault() is { } sum) _log((p.ExitCode == 0 ? "Own paks: " : "Warning: own paks: ") + sum);
            if (p.ExitCode != 0 && (await err).Trim() is { Length: > 0 } e) _log("Warning: own paks: " + e.Split('\n')[0].Trim());
        }
        catch (Exception e) { _log("Warning: could not check the own paks' signatures: " + e.Message); }
    }

    async Task<bool> CheckRedirect(string log)
    {
        await Task.Delay(100);
        var text = ReadShared(log);
        if (text.Contains("UNSUPPORTED")) { Fail($"Fortnite {_b.Version}: the redirect did not find curl in this executable: unsupported version. Stopped."); return false; }
        if (!text.Contains("OK:")) { Fail("The redirect did not confirm (see offline\\redirect-*.log). Stopped for safety."); return false; }
        return true;
    }

    Process Start(string exe, string args, Dictionary<string, string?>? env, bool hidden)
    {
        var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = hidden, WorkingDirectory = Path.GetDirectoryName(exe)! };
        if (hidden) psi.WindowStyle = ProcessWindowStyle.Hidden;
        if (env != null) foreach (var (k, v) in env) psi.Environment[k] = v;
        var p = Process.Start(psi)!;
        Native.AddToJob(p.Id);
        return p;
    }

    async Task<bool> WaitHttp(string url, int seconds)
    {
        for (var i = 0; i < seconds * 4; i++)
        {
            try { if ((await _http.GetAsync(url)).IsSuccessStatusCode) return true; } catch { }
            await Task.Delay(250);
        }
        return false;
    }

    /// <summary>Follows the match server's game log and its DLL's log (only the new bytes, once a second) and reports the
    /// match phase: "Join succeeded", "Set AthenaGamePhase to [..]" (UE/Fortnite) and the DLL's bus timer.</summary>
    async Task WatchMatch(string serverLog, string dllLog)
    {
        long pS = FileLen(serverLog), pR = FileLen(dllLog);
        var server = _server;
        _phase = "waiting"; Match?.Invoke("waiting", "Waiting for you: press PLAY in the game");
        while (server is { HasExited: false })
        {
            await Task.Delay(1000);
            foreach (var line in ReadNew(serverLog, ref pS))
            {
                if (line.Contains("LogNet: Join succeeded: ")) { _phase = "joined"; Match?.Invoke("joined", "You joined the match"); _log("You joined the local match."); }
                else if (line.Contains("Set AthenaGamePhase to [") && line.IndexOf('[', line.IndexOf("Set AthenaGamePhase")) is var i and > 0)
                {
                    var ph = line[(i + 1)..].Split(']')[0];
                    var (k, d) = ph switch
                    {
                        "Warmup" => ("warmup", "Warm-up island"), "Aircraft" => ("bus", "Battle bus"),
                        "SafeZones" => ("storm", "Storm"), "EndGame" => ("ended", "Match over"), _ => ("", ""),
                    };
                    if (k != "") { _phase = k; Match?.Invoke(k, d); _log($"Match: {d}."); }
                }
                else if (line.Contains("=== Critical error: ===")) _log("Error: the match server crashed (see offline\\sandbox-server\\LocalAppData\\FortniteGame\\Saved\\Logs).");
            }
            foreach (var line in ReadNew(dllLog, ref pR))
                if (line.Contains("[fnoffline] Starting the bus in ") && line.Split("in ").Last().Split(' ')[0] is var sec)
                    Match?.Invoke("warmup", $"Warm-up island · bus in {sec} s");
        }
        if (_client is { HasExited: false } && !_stoppingServer && _server == server) { Match?.Invoke("down", "The lobby still works · details in Activity"); _log("Warning: the match server stopped. The lobby still works."); }
    }

    static long FileLen(string f) { try { return new FileInfo(f).Length; } catch { return 0; } }

    static List<string> ReadNew(string f, ref long pos)
    {
        var lines = new List<string>();
        try
        {
            using var s = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (s.Length < pos) pos = 0; // recreated
            if (s.Length == pos) return lines;
            s.Position = pos;
            var text = new StreamReader(s, Encoding.UTF8).ReadToEnd();
            var cut = text.LastIndexOf('\n');
            if (cut < 0) return lines;
            pos += Encoding.UTF8.GetByteCount(text.AsSpan(0, cut + 1));
            lines.AddRange(text[..cut].Split('\n'));
        }
        catch { }
        return lines;
    }

    static string ReadShared(string f)
    {
        try { using var s = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); return new StreamReader(s, Encoding.UTF8).ReadToEnd(); }
        catch { return ""; }
    }

    static string San(string s) => new string(s.Where(char.IsLetterOrDigit).ToArray()) is { Length: > 0 } t ? t : "player";

    public Task WaitClientAsync() => _client?.WaitForExitAsync() ?? Task.CompletedTask;

    /// <summary>How the game process ended ("0x00000000" when it was closed normally), for the log.</summary>
    public string ClientExitCode => _client == null ? "?" : Native.ExitCodeOf(_exitHandles.GetValueOrDefault(_client.Id));

    public void Stop() { StopAll(_log); AuditWrites(); }

    /// <summary>Lists files changed during the session in the real Fortnite/Epic per-user folders (there should be
    /// none: the build writes inside its own folder). Written to offline\writes-outside.log.</summary>
    public void AuditWrites()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var dirs = new[] { Path.Combine(local, "FortniteGame"), Path.Combine(local, "CrashReportClient"), Path.Combine(local, "EpicGamesLauncher"),
                           Path.Combine(local, "UnrealEngine"), Path.Combine(local, "EpicOnlineServicesUIHelper"), Path.Combine(roaming, "Epic") };
        var hits = new List<string>();
        foreach (var d in dirs.Where(Directory.Exists))
            try
            {
                foreach (var f in Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories))
                    try { if (File.GetLastWriteTime(f) >= _started.AddSeconds(-2)) hits.Add($"{File.GetLastWriteTime(f):O} {f}"); } catch { }
            }
            catch { }
        var report = Path.Combine(Paths.Offline(_b.Dir), "writes-outside.log");
        File.WriteAllLines(report, new[] { $"session {_started:O} - {DateTime.Now:O}: {hits.Count} files changed outside the build" }.Concat(hits.Take(500)));
        _log(hits.Count == 0 ? "Write check: nothing written outside the build folder." : $"Write check: {hits.Count} files changed in real Fortnite/Epic folders during the session (see offline\\writes-outside.log). They may come from the real game if it was used meanwhile.");
    }

    /// <summary>Kills only processes whose executable is inside the builds root (S:\FortniteBuilds): the offline
    /// builds, their suspended stubs and the launcher's node.exe. Never anything else.</summary>
    public static int StopAll(Action<string> log)
    {
        var n = 0;
        var self = Environment.ProcessId;
        foreach (var p in Process.GetProcesses())
        {
            if (p.Id == self) continue;
            var path = Native.ImagePath(p.Id);
            if (path == null || !Paths.IsUnder(path, Paths.Root)) continue;
            if (Path.GetFileName(path) is var fn && (fn.Equals("FnOffline.exe", StringComparison.OrdinalIgnoreCase) || fn.Equals("RadarniteLauncher.exe", StringComparison.OrdinalIgnoreCase))) continue;
            try { p.Kill(true); n++; } catch { }
        }
        log(n > 0 ? $"Stopped {n} offline build processes." : "Nothing was running.");
        return n;
    }
}
