using System.Diagnostics;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace FnOffline;

/// <summary>
/// Self-update from the public GitHub releases of Radarnite/radarnite-launcher. On start (and from the "Check for
/// updates" menu item) the launcher asks the GitHub API for the latest release, compares its tag (semver) with this
/// build's version, and when a newer one exists it downloads the installer attached to the release
/// (RadarniteLauncherSetup-&lt;ver&gt;.exe), checks it against the release's &lt;name&gt;.sha256 asset (or, failing that,
/// the asset's own sha256 "digest" from the API), runs it and closes the launcher so NSIS can replace the files.
/// Offline-first: any network, parse or verification error is written to launcher\update.log and otherwise ignored,
/// so a PC with no internet is never blocked and no unverified binary is ever run.
/// </summary>
public static class Updater
{
    public const string Repo = "Radarnite/radarnite-launcher";
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromMinutes(10) };
    static Updater() => Http.DefaultRequestHeaders.UserAgent.ParseAdd("radarnite-launcher");

    /// <summary>This build's version (the assembly version = FnOffline.csproj &lt;Version&gt;, the single source of truth).</summary>
    public static Version Current
    {
        get { var v = Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0, 0); return new Version(Math.Max(v.Major, 0), Math.Max(v.Minor, 0), Math.Max(v.Build, 0)); }
    }

    static void Log(string s)
    {
        try { File.AppendAllText(Path.Combine(Paths.LauncherDir, "update.log"), $"{DateTime.Now:O} {s}\r\n"); } catch { }
    }

    public sealed record Release(Version Version, string Tag, string InstallerUrl, string? Sha256Url, string? Digest, string Notes);

    /// <summary>The latest release when it is newer than this build; null if up to date, offline, or on any error.</summary>
    public static async Task<Release?> CheckAsync()
    {
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{Repo}/releases/latest");
            req.Headers.Accept.ParseAdd("application/vnd.github+json");
            using var res = await Http.SendAsync(req);
            res.EnsureSuccessStatusCode();
            var j = JsonNode.Parse(await res.Content.ReadAsStringAsync())!.AsObject();
            var tag = j["tag_name"]?.GetValue<string>() ?? "";
            if (!TryParse(tag, out var ver)) { Log($"latest release tag is not a version: '{tag}'"); return null; }
            string? installer = null, sha = null, digest = null;
            foreach (var a in j["assets"]?.AsArray() ?? new JsonArray())
            {
                var name = a!["name"]?.GetValue<string>() ?? "";
                var url = a["browser_download_url"]?.GetValue<string>();
                if (name.StartsWith("RadarniteLauncherSetup-", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                { installer = url; digest = a["digest"]?.GetValue<string>(); }
                else if (name.EndsWith(".sha256", StringComparison.OrdinalIgnoreCase)) sha = url;
            }
            if (ver <= Current) { Log($"up to date (installed {Current}, latest {ver})"); return null; }
            if (installer == null) { Log($"release {tag} has no RadarniteLauncherSetup-*.exe asset"); return null; }
            Log($"update available: {ver} > {Current}");
            return new Release(ver, tag, installer, sha, digest, j["body"]?.GetValue<string>() ?? "");
        }
        catch (Exception e) { Log("check failed: " + e.Message); return null; }
    }

    /// <summary>Download the installer to %TEMP%, verify its sha256, and start it. Returns true when the installer was
    /// launched (the caller must then exit so NSIS can replace the locked files).</summary>
    public static async Task<bool> DownloadVerifyRunAsync(Release r)
    {
        try
        {
            var dst = Path.Combine(Path.GetTempPath(), $"RadarniteLauncherSetup-{r.Version}.exe");
            Log($"downloading {r.InstallerUrl} -> {dst}");
            var bytes = await Http.GetByteArrayAsync(r.InstallerUrl);
            await File.WriteAllBytesAsync(dst, bytes);

            string? want = null;
            if (r.Sha256Url != null)
                try { want = First64Hex(await Http.GetStringAsync(r.Sha256Url)); } catch (Exception e) { Log("sha256 asset download failed: " + e.Message); }
            if (want == null && r.Digest != null && r.Digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase))
                want = r.Digest["sha256:".Length..].Trim().ToLowerInvariant();
            if (want == null) { Log("no sha256 to verify against; refusing to run the installer"); return false; }

            var got = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            if (!string.Equals(got, want, StringComparison.OrdinalIgnoreCase)) { Log($"sha256 mismatch: got {got}, want {want}; not running"); try { File.Delete(dst); } catch { } return false; }
            Log($"sha256 ok ({got}); starting installer");

            Process.Start(new ProcessStartInfo(dst) { UseShellExecute = true });
            return true;
        }
        catch (Exception e) { Log("download/run failed: " + e.Message); return false; }
    }

    /// <summary>Whole GUI flow (the app is WinForms throughout): check, ask, download, run. Returns true when an install
    /// was started, so the caller should exit the launcher. <paramref name="canInstall"/> is false while a game is
    /// running: then a newer version is only announced, never installed from under the running game.</summary>
    public static async Task<bool> RunInteractiveAsync(IWin32Window? owner, bool manual, bool canInstall, Action<string, string>? toast = null)
    {
        toast ??= (_, _) => { };
        if (manual) toast("Checking for updates…", "info");
        var r = await CheckAsync();
        if (r == null) { if (manual) toast($"Radarnite Launcher is up to date ({Current}).", "ok"); return false; }
        if (!canInstall) { toast($"Radarnite Launcher {r.Version} is available (finish playing, then reopen the launcher).", "info"); return false; }
        if (MessageBox.Show(owner, $"Radarnite Launcher {r.Version} is available (you have {Current}).\r\n\r\nDownload and install it now? The launcher will close while it updates.",
                "Radarnite Launcher update", MessageBoxButtons.YesNo, MessageBoxIcon.Information) != DialogResult.Yes) return false;
        toast($"Downloading Radarnite Launcher {r.Version}…", "info");
        if (await DownloadVerifyRunAsync(r)) return true;
        toast("Update failed (see launcher\\update.log).", "bad");
        return false;
    }

    static string? First64Hex(string s)
    {
        foreach (var tok in s.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
            if (tok.Length >= 64 && tok[..64].All(Uri.IsHexDigit)) return tok[..64].ToLowerInvariant();
        return null;
    }

    static bool TryParse(string tag, out Version v)
    {
        v = new Version(0, 0, 0);
        var s = tag.Trim();
        if (s.StartsWith("v", StringComparison.OrdinalIgnoreCase)) s = s[1..];
        var core = s.Split('-', '+')[0]; // strip pre-release / build metadata
        var parts = core.Split('.');
        if (parts.Length < 2 || !int.TryParse(parts[0], out var maj) || !int.TryParse(parts[1], out var min)) return false;
        int pat = 0;
        if (parts.Length >= 3 && !int.TryParse(parts[2], out pat)) return false;
        v = new Version(maj, min, pat);
        return true;
    }
}
