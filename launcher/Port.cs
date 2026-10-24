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
    /// and icons (fortnite-api.com, cached for a day; without network the ids are shown) and its state in this build.</summary>
    public static async Task<JsonObject> CatalogAsync(BuildInfo b, bool refresh, Action<string> log)
    {
        var (tool, why) = FindTool();
        if (tool == null) return new JsonObject { ["error"] = why };
        var paks = CurrentGamePaks();
        if (paks == null) return new JsonObject { ["error"] = "Fortnite is not installed on this PC (the current game is where the cosmetics come from)" };
        var stamp = Directory.GetFiles(paks, "*.utoc").Select(f => File.GetLastWriteTimeUtc(f)).DefaultIfEmpty().Max();
        if (refresh || !File.Exists(ItemsFile) || File.GetLastWriteTimeUtc(ItemsFile) < stamp)
        {
            log("Listing the cosmetics of the installed Fortnite (fn-port list, about a minute)...");
            var tmp = ItemsFile + ".tmp";
            var (code, output) = await RunAsync(tool.Dotnet, $"\"{tool.Dll}\" list --out \"{tmp}\" --cache \"{Path.Combine(Work, "cache")}\" --input \"{paks}\"", null);
            if (code != 0 || !File.Exists(tmp)) return new JsonObject { ["error"] = $"fn-port list failed (exit {code}): {Last(output)}" };
            File.Move(tmp, ItemsFile, true);
        }
        var names = await NamesAsync(refresh, log);
        var have = Imported(b);
        var inBuild = BuildIds(b);
        var failed = Failed(b);
        var arr = new JsonArray();
        foreach (var it in JsonNode.Parse(File.ReadAllText(ItemsFile))!.AsArray())
        {
            var cls = it?["class"]?.GetValue<string>() ?? "";
            if (!Types.TryGetValue(cls, out var type)) continue;
            var path = it!["path"]!.GetValue<string>();
            var id = it["id"]?.GetValue<string>() ?? path[(path.LastIndexOf('.') + 1)..];
            var n = names.TryGetValue(id.ToLowerInvariant(), out var nm) ? nm : null;
            var status = have.Contains(path) ? "imported" : inBuild.Contains(id.ToLowerInvariant()) ? "build" : failed.TryGetValue(path, out var f) ? f : "new";
            arr.Add(new JsonObject
            {
                ["path"] = path, ["id"] = id, ["type"] = type, ["status"] = status,
                ["name"] = n?["name"]?.GetValue<string>(), ["rarity"] = n?["rarity"]?.GetValue<string>(), ["icon"] = n?["icon"]?.GetValue<string>(),
                ["set"] = n?["set"]?.GetValue<string>(), ["added"] = n?["added"]?.GetValue<string>(),
            });
        }
        var used = have.Count;
        return new JsonObject { ["items"] = arr, ["slotsFree"] = Math.Max(0, ChunkSlots - used), ["game"] = paks };
    }

    /// <summary>id (lower case) -> name, rarity, icon, set, date added, from fortnite-api.com's BR list.</summary>
    static async Task<Dictionary<string, JsonNode>> NamesAsync(bool refresh, Action<string> log)
    {
        if (refresh || !File.Exists(NamesFile) || File.GetLastWriteTimeUtc(NamesFile) < DateTime.UtcNow.AddDays(-1))
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
                            ["name"] = d["name"]?.GetValue<string>(), ["rarity"] = d["rarity"]?["value"]?.GetValue<string>(),
                            ["icon"] = d["images"]?["smallIcon"]?.GetValue<string>() ?? d["images"]?["icon"]?.GetValue<string>(),
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
        internal Process? Proc;
        volatile bool _installing;
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
    }

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
        log($"Importing {list.Count} cosmetics of the current Fortnite into {b.Version} (fn-port; the build's files are backed up in offline\\backup-port-*)...");
        var args = $"-NoProfile -ExecutionPolicy Bypass -File \"{tool.Script}\" -Build \"{b.Dir}\" -Items \"{file}\" -Work \"{Work}\" -Dll \"{tool.Dll}\"";
        var (code, output) = await RunAsync("powershell.exe", args, line =>
        {
            try { File.AppendAllText(logFile, line + "\r\n"); } catch { }
            line = line.Trim();
            if (line.StartsWith("OK ")) { job.Ok++; job.Done++; job.Installing(true); }
            else if (line.StartsWith("FAILED ")) { job.Failed++; job.Done++; job.Installing(true); }
            else if (line.StartsWith("batch ") || line.StartsWith("done:")) job.Installing(false);
            else if (Regex.Match(line, @"^items: \d+ listed, (\d+) to port") is { Success: true } m) job.Total = int.Parse(m.Groups[1].Value);
            else if (line.Length == 0) return;
            job.Line = line;
            if (!line.StartsWith("OK ")) log("Import: " + line);
            progress();
        }, job);
        if (job.Cancelled) { log($"Import stopped: {job.Ok} imported. Import again to continue where it stopped."); return false; }
        try { File.AppendAllText(logFile, $"exit {code}\r\n{output}"); } catch { }
        if (code != 0 && job.Ok == 0) { log($"Could not import (exit {code}): {Last(output)} (log: {logFile})."); return false; }
        log($"Import finished: {job.Ok} imported, {job.Failed} not converted (details: {Path.Combine(Work, $"report-{Path.GetFileName(b.Dir)}.json")}).");
        return true;
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
