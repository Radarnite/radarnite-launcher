using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace FnOffline;

/// <summary>
/// Cosmetics of the installed (current) Fortnite imported into an offline classic-pak build as NEW items (nothing of
/// the build is replaced). The conversion is FortnitePorting's fn-port (tools/fn-port): `fn-port list` gives every
/// cosmetic of the current game, its windows\port.ps1 converts and installs them in batches (own encrypted pak per
/// item + offline\custom-cosmetics.json, which the backend reads; resumable, backups, 70 % CPU rule, refuses while the
/// build runs). This class only finds those tools, lists, and runs port.ps1 for the items picked in the window.
/// fn-port is not shipped with the launcher: it is looked for in launcher\tools\fn-port (fn-port.dll + port.ps1) and
/// then in the FortnitePorting build folders of the PC that ported cosmetics before.
/// </summary>
public static class Porter
{
    /// <summary>Builds fn-port has a donor set for (tools/fn-port/donors-&lt;version&gt;.json).</summary>
    public static readonly HashSet<string> Builds = new() { "14.30" };
    public static bool Supports(BuildInfo b) => !b.UsesIoStore && Builds.Contains(b.Version);

    /// <summary>fn-port types (item definition classes) with a porter; contrails and lobby music have none.</summary>
    public static readonly Dictionary<string, string> Types = new()
    {
        ["AthenaCharacterItemDefinition"] = "outfit", ["AthenaBackpackItemDefinition"] = "backpack", ["AthenaPickaxeItemDefinition"] = "pickaxe",
        ["AthenaDanceItemDefinition"] = "emote", ["AthenaGliderItemDefinition"] = "glider", ["AthenaLoadingScreenItemDefinition"] = "loadingscreen",
        ["AthenaSprayItemDefinition"] = "spray", ["AthenaEmojiItemDefinition"] = "emoji", ["AthenaItemWrapDefinition"] = "wrap",
    };
    /// <summary>Pak chunks port.ps1 hands out (2000-9999): one per item.</summary>
    public const int ChunkSlots = 8000;

    public static string Work => Directory.CreateDirectory(Path.Combine(Paths.LauncherDir, "port")).FullName;
    static string DotnetRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "fp-dotnet");

    public sealed record Tool(string Dotnet, string Dll, string Script);

    /// <summary>fn-port (framework-dependent dll + the .NET runtime it was built for) and port.ps1, or why not.</summary>
    public static (Tool? tool, string why) FindTool()
    {
        var dotnet = Path.Combine(DotnetRoot, "dotnet.exe");
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var candidates = new[]
        {
            (Path.Combine(Paths.LauncherDir, "tools", "fn-port", "fn-port.dll"), Path.Combine(Paths.LauncherDir, "tools", "fn-port", "port.ps1")),
            (Path.Combine(local, "fp-build", "port-cosmeticos", "src", "tools", "fn-port", "bin", "Release", "net10.0", "fn-port.dll"), @"S:\port-cosmeticos\ps\port.ps1"),
        };
        foreach (var (dll, ps) in candidates)
            if (File.Exists(dll) && File.Exists(ps))
                return File.Exists(dotnet) ? (new Tool(dotnet, dll, ps), "") : (null, $"the .NET runtime fn-port needs is missing ({dotnet})");
        return (null, "the cosmetics importer (fn-port) is not installed: put fn-port.dll and port.ps1 in launcher\\tools\\fn-port");
    }

    /// <summary>The Paks folder of the installed Fortnite (Epic Games Launcher's install list), else the default path.</summary>
    public static string? CurrentGamePaks()
    {
        try
        {
            var dat = @"C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat";
            if (File.Exists(dat))
                foreach (var e in JsonNode.Parse(File.ReadAllText(dat))?["InstallationList"]?.AsArray() ?? [])
                    if (e?["AppName"]?.GetValue<string>() == "Fortnite" && e["InstallLocation"]?.GetValue<string>() is { } loc
                        && Directory.Exists(Path.Combine(loc, "FortniteGame", "Content", "Paks")))
                        return Path.Combine(loc, "FortniteGame", "Content", "Paks");
        }
        catch { }
        var def = @"C:\Program Files\Epic Games\Fortnite\FortniteGame\Content\Paks";
        return Directory.Exists(def) ? def : null;
    }

    static string ItemsFile => Path.Combine(Work, "items.json");
    static string NamesFile => Path.Combine(Work, "names.json");

    /// <summary>Every cosmetic of the current game (fn-port list, cached until the game updates) with display names
    /// and icons (fortnite-api.com, cached for a day; without network the ids are shown) and its state in this build.
    /// Both run at once and <paramref name="stage"/> reports them: ("names"|"list", estimated ms, done); as soon as the
    /// names are in, <paramref name="early"/> gets fortnite-api's list (no source paths yet: shown, not importable).</summary>
    public static async Task<JsonObject> CatalogAsync(BuildInfo b, bool refresh, Action<string> log, Action<string, long, bool>? stage = null, Action<JsonObject>? early = null)
    {
        var (tool, why) = FindTool();
        if (tool == null) return new JsonObject { ["error"] = why };
        var paks = CurrentGamePaks();
        if (paks == null) return new JsonObject { ["error"] = "Fortnite is not installed on this PC (the current game is where the cosmetics come from)" };
        var stamp = Directory.GetFiles(paks, "*.utoc").Select(f => File.GetLastWriteTimeUtc(f)).DefaultIfEmpty().Max();
        var needList = refresh || !File.Exists(ItemsFile) || File.GetLastWriteTimeUtc(ItemsFile) < stamp;
        var listTask = Task.FromResult<string?>(null);
        if (needList)
        {
            // how long the last listing took (the first one, with no fn-port cache, takes minutes)
            long est = 60_000; try { est = long.Parse(File.ReadAllText(ListMsFile)); } catch { }
            stage?.Invoke("list", est, false);
            log("Listing the cosmetics of the installed Fortnite (fn-port list)...");
            listTask = Task.Run(async () =>
            {
                var t0 = DateTime.Now;
                var tmp = ItemsFile + ".tmp";
                var (code, output) = await RunAsync(tool.Dotnet, $"\"{tool.Dll}\" list --out \"{tmp}\" --cache \"{Path.Combine(Work, "cache")}\" --input \"{paks}\"", null);
                if (code != 0 || !File.Exists(tmp)) return $"fn-port list failed (exit {code}): {Last(output)}";
                File.Move(tmp, ItemsFile, true);
                try { File.WriteAllText(ListMsFile, ((long)(DateTime.Now - t0).TotalMilliseconds).ToString()); } catch { }
                return null;
            });
        }
        stage?.Invoke("names", 4000, false);
        var names = await NamesAsync(refresh, log);
        stage?.Invoke("names", 0, true);
        var have = Imported(b);
        var inBuild = BuildIds(b);
        var failed = Failed(b);
        if (!listTask.IsCompleted && early != null)
        {
            var haveIds = have.Select(p => p[(p.LastIndexOf('.') + 1)..].ToLowerInvariant()).ToHashSet();
            var pre = new JsonArray();
            foreach (var (id, n) in names)
                if (n["type"]?.GetValue<string>() is { } t && Types.ContainsValue(t))
                    pre.Add(Entry(null, n["id"]?.GetValue<string>() ?? id, t, haveIds.Contains(id) ? "imported" : inBuild.Contains(id) ? "build" : "new", n));
            early(new JsonObject { ["items"] = pre, ["partial"] = true });
        }
        if (await listTask is { } err) return new JsonObject { ["error"] = err };
        if (needList) stage?.Invoke("list", 0, true);
        var arr = new JsonArray();
        foreach (var it in JsonNode.Parse(File.ReadAllText(ItemsFile))!.AsArray())
        {
            var cls = it?["class"]?.GetValue<string>() ?? "";
            if (!Types.TryGetValue(cls, out var type)) continue;
            var path = it!["path"]!.GetValue<string>();
            var id = it["id"]?.GetValue<string>() ?? path[(path.LastIndexOf('.') + 1)..];
            var n = names.TryGetValue(id.ToLowerInvariant(), out var nm) ? nm : null;
            var status = have.Contains(path) ? "imported" : inBuild.Contains(id.ToLowerInvariant()) ? "build" : failed.TryGetValue(path, out var f) ? f : "new";
            arr.Add(Entry(path, id, type, status, n));
        }
        var used = have.Count;
        return new JsonObject { ["items"] = arr, ["slotsFree"] = Math.Max(0, ChunkSlots - used), ["game"] = paks };
    }

    static JsonObject Entry(string? path, string id, string type, string status, JsonNode? n) => new()
    {
        ["path"] = path, ["id"] = id, ["type"] = type, ["status"] = status,
        ["name"] = n?["name"]?.GetValue<string>(), ["rarity"] = n?["rarity"]?.GetValue<string>(), ["series"] = n?["series"]?.GetValue<string>(),
        ["icon"] = n?["icon"]?.GetValue<string>(), ["featured"] = n?["featured"]?.GetValue<string>(), ["set"] = n?["set"]?.GetValue<string>(), ["added"] = n?["added"]?.GetValue<string>(),
    };

    /// <summary>id (lower case) -> name, rarity, icon, set, date added, from fortnite-api.com's BR list.</summary>
    static async Task<Dictionary<string, JsonNode>> NamesAsync(bool refresh, Action<string> log)
    {
        var stale = true;
        try { stale = !File.ReadAllText(NamesFile).Contains("\"type\":"); } catch { }
        if (refresh || stale || File.GetLastWriteTimeUtc(NamesFile) < DateTime.UtcNow.AddDays(-1))
            try
            {
                using var http = new HttpClient(new HttpClientHandler { AutomaticDecompression = System.Net.DecompressionMethods.All }) { Timeout = TimeSpan.FromSeconds(60) };
                http.DefaultRequestHeaders.UserAgent.ParseAdd("RadarniteLauncher");
                var data = JsonNode.Parse(await http.GetStringAsync("https://fortnite-api.com/v2/cosmetics/br"))?["data"]?.AsArray() ?? [];
                var o = new JsonObject();
                foreach (var d in data)
                    if (d?["id"]?.GetValue<string>() is { } id)
                        o[id.ToLowerInvariant()] = new JsonObject
                        {
                            ["id"] = id, ["type"] = d["type"]?["value"]?.GetValue<string>(),
                            ["name"] = d["name"]?.GetValue<string>(), ["rarity"] = d["rarity"]?["value"]?.GetValue<string>(), ["series"] = d["series"]?["value"]?.GetValue<string>(),
                            ["icon"] = d["images"]?["icon"]?.GetValue<string>() ?? d["images"]?["smallIcon"]?.GetValue<string>(), ["featured"] = d["images"]?["featured"]?.GetValue<string>(),
                            ["set"] = d["set"]?["value"]?.GetValue<string>(), ["added"] = d["added"]?.GetValue<string>(),
                        };
                File.WriteAllText(NamesFile, o.ToJsonString());
            }
            catch (Exception e) { log($"Warning: no cosmetic names and icons (fortnite-api.com: {e.Message}); the ids are shown instead."); }
        var map = new Dictionary<string, JsonNode>();
        try { foreach (var (k, v) in JsonNode.Parse(File.ReadAllText(NamesFile))!.AsObject()) if (v != null) map[k] = v; } catch { }
        return map;
    }

    /// <summary>Source paths already imported into the build (custom-cosmetics.json entries with source "fn-port:&lt;path&gt;").</summary>
    public static HashSet<string> Imported(BuildInfo b)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            var f = Path.Combine(b.Dir, "offline", "custom-cosmetics.json");
            if (File.Exists(f))
                foreach (var it in JsonNode.Parse(File.ReadAllText(f).TrimStart('\uFEFF'))?["items"]?.AsArray() ?? [])
                    if (it?["source"]?.GetValue<string>() is { } s && s.StartsWith("fn-port:")) set.Add(s[8..]);
        }
        catch { }
        return set;
    }

    /// <summary>Lower-case ids of the build's own cosmetics (offline\cosmetics.json): the same item already exists there.</summary>
    static HashSet<string> BuildIds(BuildInfo b)
    {
        var set = new HashSet<string>();
        try
        {
            var f = Path.Combine(b.Dir, "offline", "cosmetics.json");
            if (File.Exists(f))
                foreach (var it in JsonNode.Parse(File.ReadAllText(f))?["items"]?.AsArray() ?? [])
                    if (it?["templateId"]?.GetValue<string>() is { } t) set.Add(t[(t.IndexOf(':') + 1)..].ToLowerInvariant());
        }
        catch { }
        return set;
    }

    /// <summary>port.ps1's state for the build: items it found already in the build ("build") or could not convert.</summary>
    static Dictionary<string, string> Failed(BuildInfo b)
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            var f = Path.Combine(Work, $"state-{Path.GetFileName(b.Dir)}.json");
            if (File.Exists(f))
                foreach (var (k, v) in JsonNode.Parse(File.ReadAllText(f).TrimStart('\uFEFF'))!.AsObject())
                    switch (v?["status"]?.GetValue<string>())
                    {
                        case "exists": map[k] = "build"; break;
                        case "failed" or "crashed" or "unsupported": map[k] = "failed"; break;
                    }
        }
        catch { }
        return map;
    }

    /// <summary>A running import (one at a time).</summary>
    public sealed class Job
    {
        public int Total, Done, Ok, Failed;
        public string Build = "", Line = "";
        public bool Cancelled;
        // what is going on right now, for the window: phase (wait = CPU check / next batch, load = fn-port reading the
        // current game, convert, install = paks and custom-cosmetics.json, sign = fitting the pak signatures), the item
        // being converted, every item's state and port.ps1's / fn-port's last lines
        public string Phase = "start", Current = "";
        public DateTime Started = DateTime.Now, PhaseSince = DateTime.Now, CurrentSince = DateTime.Now;
        public int Batch, Cpu = -1;
        public double SecPerItem;
        public readonly Dictionary<string, string> States = new(StringComparer.OrdinalIgnoreCase); // path -> pending|queued|converting|ok|failed
        public readonly List<string> Order = new();
        public readonly List<(DateTime t, string text)> Recent = new();
        internal Process? Proc;
        volatile bool _installing;
        internal readonly object Lock = new();
        internal void SetPhase(string p) { if (Phase != p) { Phase = p; PhaseSince = DateTime.Now; } }
        internal void Note(string line) { lock (Lock) { Recent.Add((DateTime.Now, line)); if (Recent.Count > 60) Recent.RemoveRange(0, Recent.Count - 60); } }
        /// <summary>Marks an item converted (or not); counted once, whether fn-port's or port.ps1's line comes first.</summary>
        internal void Result(string path, bool ok)
        {
            lock (Lock)
            {
                if (States.GetValueOrDefault(path) is "ok" or "failed") return;
                States[path] = ok ? "ok" : "failed"; Done++; if (ok) Ok++; else Failed++;
                if (string.Equals(Current, path, StringComparison.OrdinalIgnoreCase)) Current = "";
            }
        }
        /// <summary>The next queued item of the batch becomes the one being converted.</summary>
        internal void Next()
        {
            lock (Lock)
            {
                var n = Order.FirstOrDefault(p => States.GetValueOrDefault(p) is "queued" or "converting");
                if (n == null) { Current = ""; return; }
                if (!string.Equals(Current, n, StringComparison.OrdinalIgnoreCase)) { Current = n; CurrentSince = DateTime.Now; States[n] = "converting"; }
            }
        }
        // After a batch's "OK"/"FAILED" lines port.ps1 copies its paks and rewrites custom-cosmetics.json: a stop then
        // waits for the batch's "batch N:" line, so no pak is left installed without its entry. A batch still being
        // converted is dropped (port.ps1 picks it up again from its state file next time).
        internal void Installing(bool on) { _installing = on; if (!on && Cancelled) Kill(); }
        public void Cancel() { Cancelled = true; if (!_installing) Kill(); }
        /// <summary>Stop and wait for port.ps1 to exit (killed anyway after <paramref name="ms"/>).</summary>
        public async Task CancelAndWaitAsync(int ms)
        {
            Cancel();
            var t0 = DateTime.Now;
            while (Proc is { HasExited: false } && (DateTime.Now - t0).TotalMilliseconds < ms) await Task.Delay(200);
            Kill();
        }
        void Kill() { try { Proc?.Kill(true); } catch { } }

        /// <summary>Snapshot for the window (times in ms so it can keep counting between updates).</summary>
        public object View()
        {
            lock (Lock)
            {
                var now = DateTime.Now;
                var left = Math.Max(0, Total - Done);
                // seconds per item: this import's own pace once something finished, else the last import's
                var rate = Ok > 0 ? (now - Started).TotalSeconds / Done : SecPerItem;
                var shown = Order.Where(p => States.GetValueOrDefault(p) is not "pending").ToList();
                return new
                {
                    build = Build, total = Total, done = Done, ok = Ok, failed = Failed, line = Line, phase = Phase, batch = Batch, cpu = Cpu,
                    batches = (int)Math.Ceiling(Total / (double)BatchSize), elapsedMs = (long)(now - Started).TotalMilliseconds,
                    phaseMs = (long)(now - PhaseSince).TotalMilliseconds, current = Current, currentMs = (long)(now - CurrentSince).TotalMilliseconds,
                    etaMs = rate > 0 && left > 0 ? (long)(left * rate * 1000) : 0,
                    items = shown.Select(p => new { path = p, st = States[p] }).ToArray(),
                    recent = Recent.TakeLast(14).Select(r => new { t = r.t.ToString("HH:mm:ss"), text = r.text }).ToArray(),
                };
            }
        }
    }

    /// <summary>Items per fn-port process (port.ps1's -Batch).</summary>
    const int BatchSize = 25;
    static string SpeedFile => Path.Combine(Work, "speed.txt");
    static string ListMsFile => Path.Combine(Work, "list-ms.txt");

    /// <summary>Converts and installs the given source paths into the build with port.ps1. Progress lines:
    /// "items: N listed, M to port", "OK &lt;path&gt; …" / "FAILED &lt;path&gt; …" per item, "batch …", "done: …".</summary>
    public static async Task<bool> ImportAsync(BuildInfo b, IReadOnlyList<string> paths, Job job, Action<string> log, Action progress)
    {
        var (tool, why) = FindTool();
        if (tool == null) { log("Could not import: " + why + "."); return false; }
        var classes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        try { foreach (var it in JsonNode.Parse(File.ReadAllText(ItemsFile))!.AsArray()) classes[it!["path"]!.GetValue<string>()] = it["class"]?.GetValue<string>() ?? ""; } catch { }
        var list = new JsonArray(paths.Distinct().Select(p => (JsonNode)new JsonObject { ["path"] = p, ["class"] = classes.GetValueOrDefault(p, "") }).ToArray());
        var file = Path.Combine(Work, $"import-{DateTime.Now:yyyyMMdd-HHmmss}.json");
        var logFile = Path.ChangeExtension(file, ".log"); // port.ps1's whole output, for when something goes wrong
        File.WriteAllText(file, list.ToJsonString());
        job.Build = b.Version; job.Total = list.Count;
        foreach (var p in paths.Distinct()) { job.Order.Add(p); job.States[p] = "pending"; }
        try { job.SecPerItem = double.Parse(File.ReadAllText(SpeedFile), System.Globalization.CultureInfo.InvariantCulture); } catch { job.SecPerItem = 14; }
        log($"Importing {list.Count} cosmetics of the current Fortnite into {b.Version} (fn-port; the build's files are backed up in offline\\backup-port-*)...");
        var args = $"-NoProfile -ExecutionPolicy Bypass -File \"{tool.Script}\" -Build \"{b.Dir}\" -Items \"{file}\" -Work \"{Work}\" -Dll \"{tool.Dll}\" -Batch {BatchSize}";
        using var stop = new CancellationTokenSource();
        var tail = Task.Run(() => TailBatchAsync(job, progress, stop.Token));
        var (code, output) = await RunAsync("powershell.exe", args, line =>
        {
            try { File.AppendAllText(logFile, line + "\r\n"); } catch { }
            line = line.Trim();
            if (line.Length == 0) return;
            job.Note(line);
            if (Regex.Match(line, @"^(OK|FAILED) (\S+)") is { Success: true } r) { job.Result(r.Groups[2].Value.TrimEnd(':'), r.Groups[1].Value == "OK"); job.SetPhase("install"); job.Installing(true); }
            else if (line.StartsWith("batch ")) { job.Installing(false); job.SetPhase(job.Done < job.Total ? "wait" : "install"); }
            else if (line.StartsWith("done:")) { job.Installing(false); job.SetPhase("sign"); }
            else if (line.StartsWith("cpu:")) job.SetPhase("cpu");
            else if (Regex.Match(line, @"^items: \d+ listed, (\d+) to port") is { Success: true } m) { job.Total = int.Parse(m.Groups[1].Value); job.SetPhase("wait"); }
            job.Line = line;
            if (!line.StartsWith("OK ")) log("Import: " + line);
            progress();
        }, job);
        stop.Cancel();
        try { await tail; } catch { }
        if (job.Ok > 0) try { File.WriteAllText(SpeedFile, ((DateTime.Now - job.Started).TotalSeconds / job.Done).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture)); } catch { }
        if (job.Cancelled) { log($"Import stopped: {job.Ok} imported. Import again to continue where it stopped."); return false; }
        try { File.AppendAllText(logFile, $"exit {code}\r\n{output}"); } catch { }
        if (code != 0 && job.Ok == 0) { log($"Could not import (exit {code}): {Last(output)} (log: {logFile})."); return false; }
        log($"Import finished: {job.Ok} imported, {job.Failed} not converted (details: {Path.Combine(Work, $"report-{Path.GetFileName(b.Dir)}.json")}).");
        return true;
    }

    /// <summary>port.ps1 only prints a batch's results once its fn-port process ends; meanwhile fn-port writes to
    /// &lt;work&gt;\batch\stdout.txt: reading it live gives the phase (reading the current game until its "localization"
    /// line, then converting item after item in the order of batch\items.json) and each result as it happens.</summary>
    static async Task TailBatchAsync(Job job, Action progress, CancellationToken ct)
    {
        var dir = Path.Combine(Work, "batch");
        string batchKey = "", partial = "";
        long pos = 0;
        var cpu = new CpuMeter();
        while (!ct.IsCancellationRequested)
        {
            try { await Task.Delay(500, ct); } catch { break; }
            // port.ps1 waits silently while the CPU is above 70 %: the window shows the reading
            if (cpu.Sample() is int c) { job.Cpu = c; if (job.Phase is "wait" or "cpu") { if (c > 70 && (DateTime.Now - job.PhaseSince).TotalSeconds > 5) job.SetPhase("cpu"); progress(); } }
            try
            {
                var itemsFile = Path.Combine(dir, "items.json");
                if (!File.Exists(itemsFile) || File.GetLastWriteTime(itemsFile) < job.Started) continue;
                var batch = JsonNode.Parse(File.ReadAllText(itemsFile))!.AsArray().Select(x => x!["path"]!.GetValue<string>()).ToList();
                var key = string.Join("|", batch);
                if (key != batchKey) // a new batch: its items are queued, fn-port starts by reading the current game
                {
                    batchKey = key; pos = 0; partial = "";
                    lock (job.Lock) { job.Batch++; foreach (var bp in batch) if (job.States.GetValueOrDefault(bp) is null or "pending") { job.States[bp] = "queued"; if (!job.Order.Contains(bp)) job.Order.Add(bp); } }
                    job.SetPhase("load"); job.Note($"batch {job.Batch}: fn-port is reading the current Fortnite ({batch.Count} items)");
                    progress();
                }
                var so = Path.Combine(dir, "stdout.txt");
                if (!File.Exists(so)) continue;
                using var fs = new FileStream(so, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (fs.Length < pos) pos = 0;
                if (fs.Length == pos) { if (job.Phase is "convert" or "load") progress(); continue; }
                fs.Seek(pos, SeekOrigin.Begin);
                using var rd = new StreamReader(fs);
                var text = partial + await rd.ReadToEndAsync(ct);
                pos = fs.Length;
                var lines = text.Split('\n');
                partial = lines[^1];
                foreach (var raw in lines[..^1])
                {
                    var line = raw.Trim();
                    if (line.Length == 0) continue;
                    if (line.StartsWith("localization")) { job.SetPhase("convert"); job.Next(); continue; }
                    if (Regex.Match(line, @"^(OK|FAILED) (\S+)") is { Success: true } r)
                    {
                        var path = r.Groups[2].Value.TrimEnd(':');
                        job.Result(path, r.Groups[1].Value == "OK");
                        job.Note(r.Groups[1].Value == "OK" ? "converted " + path[(path.LastIndexOf('.') + 1)..] : line);
                        job.Next();
                        if (job.Current == "") job.SetPhase("install");
                    }
                    else job.Note(line);
                }
                progress();
            }
            catch (OperationCanceledException) { break; }
            catch { } // the batch folder is being replaced: next round
        }
    }

    /// <summary>Whole-PC CPU use (GetSystemTimes), averaged over about 2 s.</summary>
    sealed class CpuMeter
    {
        [System.Runtime.InteropServices.DllImport("kernel32")] static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
        readonly Queue<(long idle, long total)> _q = new();
        public int? Sample()
        {
            if (!GetSystemTimes(out var idle, out var kernel, out var user)) return null;
            _q.Enqueue((idle, kernel + user)); // kernel time includes idle time
            if (_q.Count > 5) _q.Dequeue();
            if (_q.Count < 2) return null;
            var (i0, t0) = _q.Peek();
            var dt = kernel + user - t0;
            return dt <= 0 ? null : (int)Math.Round(100.0 * (1 - (idle - i0) / (double)dt));
        }
    }

    static string Last(string output) => output.Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.Length > 0) ?? "no output";

    static async Task<(int code, string output)> RunAsync(string exe, string args, Action<string>? onLine, Job? job = null)
    {
        var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        psi.Environment["DOTNET_ROOT"] = DotnetRoot;
        psi.Environment["DOTNET_GCHeapHardLimit"] = "0xC0000000"; // 3 GB, as port.ps1 caps each batch
        using var p = Process.Start(psi)!;
        if (job != null) job.Proc = p;
        try { p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch { }
        var all = new System.Text.StringBuilder();
        var err = p.StandardError.ReadToEndAsync();
        while (await p.StandardOutput.ReadLineAsync() is { } line) { all.AppendLine(line); onLine?.Invoke(line); }
        await p.WaitForExitAsync();
        all.Append(await err);
        return (p.ExitCode, all.ToString());
    }
}
