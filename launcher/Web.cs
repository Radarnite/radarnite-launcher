using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace FnOffline;

/// <summary>
/// Radarnite Launcher window. A WebView2 (the Edge runtime already on Windows) shows ui\index.html. Custom frame:
/// the window keeps its native styles (WS_THICKFRAME | WS_CAPTION | min/max/sysmenu), so Snap, Aero Shake, window
/// animations, the DWM shadow and rounded corners keep working, but WM_NCCALCSIZE gives the whole window to the
/// client area (no system title bar). The page draws its own title bar:
///   - dragging, double-click maximize and the right-click system menu come from WebView2's non-client region
///     support (CSS app-region: drag on the title bar);
///   - resizing is native: in the normal state the WebView is inset by a thin border (same colour as the page) and
///     WM_NCHITTEST answers HTLEFT/HTTOPRIGHT/... there, so Windows runs its own size loop, cursors and Snap;
///   - minimize / maximize / close buttons call the host; Alt+Space opens the system menu.
/// Everything else goes through the same logic as before (BuildInfo, Firewall, BuildData, Session). The page is served
/// from the launcher folder under a private virtual host and cannot navigate anywhere else. If WebView2 cannot start,
/// the plain WinForms windows are used instead.
/// </summary>
sealed class WebHost : Form
{
    const string Host = "radarnite.launcher";
    readonly WebView2 _web = new() { Dock = DockStyle.Fill, DefaultBackgroundColor = Color.FromArgb(16, 17, 20) };
    readonly BuildInfo? _autoPlay; // set by --play <build> (desktop shortcut): select it and launch
    Session? _session;
    string _state = "idle", _sessionVersion = "", _failure = "";
    readonly Dictionary<string, string> _stages = new();
    string? _selected;
    readonly List<string> _backlog = new();
    bool _ready;
    /// <summary>Developer self-test: render the page, save a PNG of the real window content, exit.</summary>
    public string? SelfTestPng;
    public string? SelfTestScript;

    public WebHost(BuildInfo? autoPlay)
    {
        _autoPlay = autoPlay;
        _selected = autoPlay?.Name;
        Text = "Radarnite Launcher";
        BackColor = Color.FromArgb(16, 17, 20);
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = new Icon(Path.Combine(Paths.LauncherDir, "radarnite.ico")); } catch { try { Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!); } catch { } }
        Controls.Add(_web);
        Load += async (_, _) =>
        {
            var s = DeviceDpi / 96f;
            UpdateBorder();
            MinimumSize = new Size((int)(760 * s), (int)(520 * s));
            var wa = Screen.FromControl(this).WorkingArea;
            Size = new Size(Math.Min((int)(1180 * s), wa.Width - 40), Math.Min((int)(740 * s), wa.Height - 40));
            if (StartPosition == FormStartPosition.CenterScreen) Location = new Point(wa.Left + (wa.Width - Width) / 2, wa.Top + (wa.Height - Height) / 2);
            if (SelfTestPng != null) { var t = new System.Windows.Forms.Timer { Interval = 45000 }; t.Tick += (_, _) => Close(); t.Start(); }
            try { await InitAsync(); }
            catch (Exception e)
            {
                try { File.AppendAllText(Path.Combine(Paths.LauncherDir, "ui-error.log"), $"{DateTime.Now:O} {e}\r\n"); } catch { }
                if (SelfTestPng != null) { Close(); return; }
                Hide();
                Form fallback = _autoPlay == null ? new MainForm() : new PlayForm(_autoPlay);
                fallback.FormClosed += (_, _) => Close();
                fallback.Show();
            }
        };
        Resize += (_, _) => { UpdateBorder(); PostWindow(); };
        Activated += (_, _) => PostWindow();
        Deactivate += (_, _) => PostWindow();
        FormClosing += (_, e) =>
        {
            if (_session is { Running: true } && SelfTestPng == null &&
                MessageBox.Show(this, "Fortnite is running. Closing Radarnite Launcher stops the game and the local servers.", "Radarnite Launcher",
                    MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) { e.Cancel = true; return; }
            if (_session != null) { _session.Stop(); Session.StopAll(_ => { }); }
        };
    }

    public static bool Available()
    {
        try { return !string.IsNullOrEmpty(CoreWebView2Environment.GetAvailableBrowserVersionString()); } catch { return false; }
    }

    // ---------------------------------------------------------------- custom frame
    const int WM_NCCALCSIZE = 0x0083, WM_NCLBUTTONDOWN = 0x00A1, WM_SYSCOMMAND = 0x0112, SC_KEYMENU = 0xF100;
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct NCCALCSIZE_PARAMS { public RECT r0, r1, r2; public IntPtr pos; }
    [StructLayout(LayoutKind.Sequential)] struct MARGINS { public int l, r, t, b; }
    [DllImport("user32")] static extern int GetSystemMetricsForDpi(int index, uint dpi);
    [DllImport("user32")] static extern bool ReleaseCapture();
    [DllImport("user32")] static extern bool GetCursorPos(out Point p);
    [DllImport("user32")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    [DllImport("dwmapi")] static extern int DwmExtendFrameIntoClientArea(IntPtr h, ref MARGINS m);
    [DllImport("dwmapi")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        int on = 1; try { DwmSetWindowAttribute(Handle, 20, ref on, 4); } catch { }            // dark mode frame
        var m = new MARGINS { t = 1 }; try { DwmExtendFrameIntoClientArea(Handle, ref m); } catch { } // keep the DWM shadow
        // force a frame recalculation so WM_NCCALCSIZE below applies from the start
        SetWindowPos(Handle, IntPtr.Zero, 0, 0, 0, 0, 0x0027 /*NOMOVE|NOSIZE|NOZORDER|FRAMECHANGED*/);
    }
    [DllImport("user32")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);

    // Native resize border (px at the window's DPI). The page used to start the size loop from thin strips, but
    // Chromium keeps the mouse captured in its own window while the button is down, so a drag towards the inside of
    // the window stopped after the first move (measured with a scripted drag: shrinking only moved 30 px of 300).
    int Border => WindowState == FormWindowState.Normal ? (int)Math.Round(5 * DeviceDpi / 96.0) : 0;
    void UpdateBorder() { var b = Border; if (Padding.All != b) Padding = new Padding(b); }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == 0x0084 /*WM_NCHITTEST*/ && WindowState == FormWindowState.Normal)
        {
            base.WndProc(ref m);
            if (m.Result == (IntPtr)1 /*HTCLIENT*/)
            {
                var pt = PointToClient(new Point((short)(m.LParam.ToInt64() & 0xFFFF), (short)((m.LParam.ToInt64() >> 16) & 0xFFFF)));
                int b = Border, corner = b * 3;
                bool l = pt.X < b, r = pt.X >= ClientSize.Width - b, t = pt.Y < b, bo = pt.Y >= ClientSize.Height - b;
                bool lc = pt.X < corner, rc = pt.X >= ClientSize.Width - corner, tc = pt.Y < corner, bc = pt.Y >= ClientSize.Height - corner;
                int ht = (t && lc) || (l && tc) ? 13 : (t && rc) || (r && tc) ? 14 : (bo && lc) || (l && bc) ? 16 : (bo && rc) || (r && bc) ? 17
                       : l ? 10 : r ? 11 : t ? 12 : bo ? 15 : 0;
                if (ht != 0) m.Result = (IntPtr)ht;
            }
            return;
        }
        if (m.Msg == WM_NCCALCSIZE && m.WParam != IntPtr.Zero)
        {
            // No system non-client area. When maximized Windows positions the window beyond the monitor by the frame
            // size: pull the client rect back in so nothing is cut off.
            if (WindowState == FormWindowState.Maximized)
            {
                var p = Marshal.PtrToStructure<NCCALCSIZE_PARAMS>(m.LParam);
                var dpi = (uint)DeviceDpi;
                int fx = GetSystemMetricsForDpi(32 /*SM_CXFRAME*/, dpi) + GetSystemMetricsForDpi(92 /*SM_CXPADDEDBORDER*/, dpi);
                int fy = GetSystemMetricsForDpi(33 /*SM_CYFRAME*/, dpi) + GetSystemMetricsForDpi(92, dpi);
                p.r0.L += fx; p.r0.R -= fx; p.r0.T += fy; p.r0.B -= fy;
                Marshal.StructureToPtr(p, m.LParam, false);
            }
            m.Result = IntPtr.Zero;
            return;
        }
        base.WndProc(ref m);
    }

    protected override CreateParams CreateParams
    {
        get { var cp = base.CreateParams; cp.Style |= 0x00C00000 /*WS_CAPTION*/ | 0x00040000 /*WS_THICKFRAME*/ | 0x00020000 | 0x00010000 | 0x00080000; return cp; }
    }

    void WindowOp(string op)
    {
        switch (op)
        {
            case "minimize": WindowState = FormWindowState.Minimized; break;
            case "maximize": WindowState = WindowState == FormWindowState.Maximized ? FormWindowState.Normal : FormWindowState.Maximized; break;
            case "close": Close(); break;
            case "sysmenu": SendMessage(Handle, WM_SYSCOMMAND, (IntPtr)SC_KEYMENU, (IntPtr)' '); break;
            default:
                // resize from the page's edge strips: hand over to the native sizing loop
                var ht = op switch { "n" => 12, "s" => 15, "w" => 10, "e" => 11, "nw" => 13, "ne" => 14, "sw" => 16, "se" => 17, _ => 0 };
                if (ht != 0 && WindowState == FormWindowState.Normal) {
                    // lParam = the cursor in screen coordinates, as a real WM_NCLBUTTONDOWN carries: the size loop
                    // measures from it (with 0,0 the first moves were mis-measured and corner drags stopped short).
                    GetCursorPos(out var cp);
                    ReleaseCapture(); SendMessage(Handle, WM_NCLBUTTONDOWN, (IntPtr)ht, (IntPtr)((cp.Y << 16) | (cp.X & 0xFFFF)));
                }
                break;
        }
    }

    void PostWindow() { if (_ready) Post(new { type = "window", maximized = WindowState == FormWindowState.Maximized, active = ContainsFocus || ActiveForm == this }); }

    // ---------------------------------------------------------------- web view
    async Task InitAsync()
    {
        var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(Paths.LauncherDir, "webview2-radarnite"));
        await _web.EnsureCoreWebView2Async(env);
        var core = _web.CoreWebView2;
        core.Settings.AreDevToolsEnabled = false;
        core.Settings.AreDefaultContextMenusEnabled = false;
        core.Settings.IsStatusBarEnabled = false;
        core.Settings.IsZoomControlEnabled = false;
        core.Settings.IsGeneralAutofillEnabled = false;
        core.Settings.IsPasswordAutosaveEnabled = false;
        core.Settings.AreBrowserAcceleratorKeysEnabled = false; // no Ctrl+R/F5/Ctrl+P etc.
        try { core.Settings.IsNonClientRegionSupportEnabled = true; } catch { } // CSS app-region: drag title bar
        core.SetVirtualHostNameToFolderMapping(Host, Path.Combine(Paths.LauncherDir, "ui"), CoreWebView2HostResourceAccessKind.Deny);
        core.NavigationStarting += (_, e) => { if (!e.Uri.StartsWith($"https://{Host}/", StringComparison.OrdinalIgnoreCase)) e.Cancel = true; };
        core.NewWindowRequested += (_, e) => e.Handled = true;
        core.WebMessageReceived += (_, e) => { try { OnMessage(JsonNode.Parse(e.WebMessageAsJson)!.AsObject()); } catch (Exception ex) { Log("Error: " + ex.Message, "bad"); } };
        core.Navigate($"https://{Host}/index.html");
    }

    void Post(object msg)
    {
        if (InvokeRequired) { try { BeginInvoke(() => Post(msg)); } catch { } return; }
        var json = JsonSerializer.Serialize(msg);
        if (!_ready) { _backlog.Add(json); return; }
        try { _web.CoreWebView2?.PostWebMessageAsJson(json); } catch { }
    }

    void Log(string s, string? level = null)
    {
        Post(new { type = "log", text = s, level, ts = DateTime.Now.ToString("HH:mm:ss") });
        var b = _session != null ? _sessionBuild : null;
        if (b != null) try { File.AppendAllText(Path.Combine(Paths.Offline(b.Dir), "launcher.log"), $"{DateTime.Now:O} {s}\r\n"); } catch { }
    }

    BuildInfo? _sessionBuild;
    (string phase, string detail) _match = ("", "");
    object SessionDto() => new { state = _state, version = _sessionVersion, message = _failure, stages = _stages, match = new { phase = _match.phase, detail = _match.detail } };
    void PostSession() => Post(new { type = "session", session = SessionDto() });

    readonly HashSet<string> _skippedSeen = new(StringComparer.OrdinalIgnoreCase);

    // Builds + config for the page. A folder that is not a usable build (partial extraction, an archive's own top
    // folder without the game, an unsupported layout) is listed in Activity with its reason and never empties the
    // list; one bad build (unreadable cosmetics, firewall query error) only affects its own fields.
    void PushState() => Task.Run(() =>
    {
        try
        {
            var c = Config.Load();
            var all = BuildInfo.Scan(Paths.Root, out var skipped);
            foreach (var (dir, why) in skipped)
                if (_skippedSeen.Add(dir + "|" + why)) Log($"Not a build, ignored: {dir} ({why}).", "warn");
            var builds = all.Select(b =>
            {
                string fw; try { fw = Firewall.Check(b.Dir) ?? "OK"; } catch (Exception e) { fw = e.Message; }
                int cos = 0;
                try { var f = Path.Combine(b.Dir, "offline", "cosmetics.json"); if (File.Exists(f)) cos = JsonNode.Parse(File.ReadAllText(f))?["items"]?.AsArray().Count ?? 0; } catch { }
                (Support s, string why) lob = (Support.No, "unknown"), mat = (Support.No, "unknown");
                try { lob = b.Lobby; mat = b.Match; } catch (Exception e) { lob = (Support.No, e.Message); }
                return new { name = b.Name, dir = b.Dir, version = b.Version, major = b.Major, minor = b.Minor, cl = b.Cl, engine = b.Engine,
                    lobby = lob.s.ToString(), lobbyWhy = lob.why, match = mat.s.ToString(), matchWhy = mat.why,
                    matchServer = File.Exists(Paths.GameServerDll), cosmetics = cos, firewall = fw };
            }).ToList();
            Post(new { type = "state", builds, selected = _selected, root = Paths.Root, session = SessionDto(),
                config = new { displayName = c.DisplayName, mode = c.Mode, bots = c.Bots, botsAI = c.BotsAI, infiniteMaterials = c.InfiniteMaterials, infiniteAmmo = c.InfiniteAmmo, lateGame = c.LateGame } });
        }
        catch (Exception e)
        {
            try { File.AppendAllText(Path.Combine(Paths.LauncherDir, "ui-error.log"), $"{DateTime.Now:O} PushState {e}\r\n"); } catch { }
            Log("Error: could not read the builds (" + e.Message + "). Details in launcher\\ui-error.log.", "bad");
            Post(new { type = "state", builds = Array.Empty<object>(), selected = _selected, root = Paths.Root, session = SessionDto(), config = new { } });
        }
    });

    BuildInfo? Sel(JsonObject m)
    {
        var name = m["build"]?.GetValue<string>() ?? _selected;
        var b = BuildInfo.All().FirstOrDefault(x => x.Name == name);
        if (b == null) Log("Pick a build first.", "warn");
        return b;
    }

    async void OnMessage(JsonObject m)
    {
        switch (m["cmd"]?.GetValue<string>())
        {
            case "jsError": // the page reports its own script errors (it would otherwise just stay blank)
                {
                    var t = m["text"]?.GetValue<string>() ?? "";
                    try { File.AppendAllText(Path.Combine(Paths.LauncherDir, "ui-error.log"), $"{DateTime.Now:O} page {t}\r\n"); } catch { }
                    break;
                }
            case "ready":
                _ready = true;
                foreach (var j in _backlog) _web.CoreWebView2.PostWebMessageAsJson(j);
                _backlog.Clear();
                PushState(); PostWindow();
                if (SelfTestPng != null) { await SelfTestCaptureAsync(); break; }
                if (_autoPlay != null) { await LaunchAsync(_autoPlay); break; }
                _ = RunUpdateAsync(false); // offline-first background check; silent when there is no network
                break;
            case "win": WindowOp(m["op"]?.GetValue<string>() ?? ""); break;
            case "select": _selected = m["build"]?.GetValue<string>(); break;
            case "refresh": PushState(); break;
            case "checkUpdates": await RunUpdateAsync(true); break;
            case "saveConfig":
            {
                var c = Config.Load();
                foreach (var (k, v) in m["config"]!.AsObject())
                    switch (k)
                    {
                        case "mode": c.Mode = v!.GetValue<string>(); break;
                        case "displayName": c.DisplayName = v!.GetValue<string>().Trim(); break;
                        case "bots": c.Bots = Math.Clamp(v!.GetValue<int>(), 0, 99); break;
                        case "botsAI": c.BotsAI = v!.GetValue<bool>(); break;
                        case "infiniteMaterials": c.InfiniteMaterials = v!.GetValue<bool>(); break;
                        case "infiniteAmmo": c.InfiniteAmmo = v!.GetValue<bool>(); break;
                        case "lateGame": c.LateGame = v!.GetValue<bool>(); break;
                    }
                c.Save();
                break;
            }
            case "launch": if (Sel(m) is { } lb) await LaunchAsync(lb); break;
            case "stop":
                _state = "stopping"; PostSession();
                await Task.Run(() => { _session?.Stop(); Session.StopAll(s => Log(s)); });
                _state = "idle"; _stages.Clear(); _match = ("", ""); PostSession(); PushState();
                break;
            case "addZip":
            {
                using var d = new OpenFileDialog { Filter = "Fortnite build (*.zip)|*.zip", Title = "Add a Fortnite build" };
                if (d.ShowDialog(this) != DialogResult.OK) break;
                Log($"Extracting {d.FileName} into {Paths.Root} (this can take a long time)...");
                if (await BuildData.AddZipAsync(d.FileName, s => Log(s)) is { } nb) await AfterAdd(nb);
                break;
            }
            case "addFolder":
            {
                using var d = new FolderBrowserDialog { Description = "Build folder (the one that contains FortniteGame)" };
                if (d.ShowDialog(this) != DialogResult.OK) break;
                if (await BuildData.AddFolderAsync(d.SelectedPath, s => Log(s)) is { } nb) await AfterAdd(nb);
                break;
            }
            case "chooseRoot":
            {
                if (_state is "launching" or "running" or "stopping") { Log("Stop the game before changing the builds folder."); break; }
                using var d = new FolderBrowserDialog { Description = "Folder for your Fortnite builds (one folder per build)", UseDescriptionForTitle = true, SelectedPath = Paths.Root };
                if (d.ShowDialog(this) != DialogResult.OK) break;
                var c = Config.Load(); c.BuildsFolder = d.SelectedPath; c.Save();
                Log($"Builds folder: {Paths.Root}");
                _selected = null; PushState();
                break;
            }
            case "cosmetics":
                if (Sel(m) is { } cb)
                {
                    Post(new { type = "toast", text = $"Generating cosmetics for {cb.Version}… (a few minutes)" });
                    Log($"Generating the cosmetics of {cb.Version} from the build's own data...");
                    var ok = await BuildData.GenerateCosmeticsAsync(cb, s => Log(s));
                    var why = BuildData.LastCosmeticsError;
                    Log(ok ? "Cosmetics ready." : $"Could not generate the cosmetics of {cb.Version}: {why} (log: {Path.Combine(Paths.Offline(cb.Dir), "cosmetics-gen.log")}).", ok ? null : "bad");
                    Post(new { type = "toast", text = ok ? "Cosmetics ready" : $"Could not generate the cosmetics: {why}", level = ok ? "ok" : "bad" });
                    PushState();
                }
                break;
            case "firewall":
                if (Sel(m) is { } fb)
                {
                    var ok = Program.RunElevated($"--firewall \"{fb.Name}\"");
                    Log(ok ? "Firewall rules created." : "The rules were not created (was the administrator prompt cancelled?).", ok ? null : "warn");
                    PushState();
                }
                break;
            case "shortcut":
                if (Sel(m) is { } sb) { var p = Program.CreateShortcut(sb); Log($"Shortcut created: {p}"); Post(new { type = "toast", text = "Desktop shortcut created", level = "ok" }); }
                break;
            case "openLogs":
                if (Sel(m) is { } ob) Process.Start(new ProcessStartInfo("explorer.exe", $"\"{Paths.Offline(ob.Dir)}\""));
                break;
            case "config":
                Process.Start(new ProcessStartInfo("notepad.exe", $"\"{Paths.Config}\""));
                break;
        }
    }

    async Task LaunchAsync(BuildInfo b)
    {
        if (_state is "launching" or "running") return;
        _selected = b.Name; _sessionVersion = b.Version; _sessionBuild = b; _failure = ""; _stages.Clear(); _match = ("", "");
        if (Firewall.Check(b.Dir) is { } fw)
        {
            Log("Firewall: " + fw, "warn");
            if (!Program.RunElevated($"--firewall \"{b.Name}\""))
            {
                _state = "error"; _failure = "Not starting without the firewall rules"; PostSession(); return;
            }
            PushState();
        }
        var c = Config.Load();
        _state = "launching"; PostSession();
        Log($"Launching Fortnite {b.Version} (CL {b.Cl}) - player \"{c.DisplayName}\", {c.Bots} bots in Battle Royale");
        _session = new Session(b, c, s => Log(s)) { Stage = (k, v) => { _stages[k] = v; PostSession(); }, Match = (p, d) => { _match = (p, d); PostSession(); } };
        if (!await _session.StartAsync())
        {
            _state = "error"; _failure = _session.Failure ?? "Launch failed"; PostSession(); _session = null; return;
        }
        _state = "running"; PostSession();
        WindowState = FormWindowState.Minimized;
        await _session.WaitClientAsync();
        var code = _session.ClientExitCode;
        if (code is "0x00000000" or "0x00000001" or "?") Log("The game closed: stopping the local servers.");
        else Log($"Error: the game crashed (exit code {code}, after {(int)(DateTime.Now - _session.StartedAt).TotalSeconds} s). Details: offline\\redirect-client.log and the game log in offline\\sandbox-client. Stopping the local servers.", "bad");
        await Task.Run(() => _session?.Stop());
        _session = null; _state = "idle"; _stages.Clear(); _match = ("", ""); PostSession();
        if (_autoPlay != null) { Close(); return; } // shortcut launch: the window goes away with the game
        WindowState = FormWindowState.Normal; Activate();
    }

    async Task AfterAdd(BuildInfo b)
    {
        _selected = b.Name;
        Log($"Fortnite {b.Version}: lobby {b.Lobby.s} ({b.Lobby.why}); local match {b.Match.s} ({b.Match.why}).");
        PushState();
        if (b.Lobby.s == Support.No) return;
        Log("Generating the cosmetics list from the build's own data...");
        Log(await BuildData.GenerateCosmeticsAsync(b, s => Log(s)) ? "Cosmetics ready." : $"Could not generate the cosmetics: {BuildData.LastCosmeticsError}.");
        if (Firewall.Check(b.Dir) != null && MessageBox.Show(this, $"Create the firewall rules for {b.Version} now? (asks for administrator permission)", "Radarnite Launcher", MessageBoxButtons.YesNo) == DialogResult.Yes)
            Program.RunElevated($"--firewall \"{b.Name}\"");
        PushState();
    }

    // ---------------------------------------------------------------- self-update
    // A newer public release (Radarnite/radarnite-launcher): offer it, then download, verify and run its installer,
    // closing the launcher so NSIS can replace the locked files. Never while a game is running (canInstall = false).
    async Task RunUpdateAsync(bool manual)
    {
        try
        {
            var canInstall = _session is not { Running: true } && _state is "idle";
            void Toast(string t, string l) => Post(new { type = "toast", text = t, level = l });
            if (await Updater.RunInteractiveAsync(this, manual, canInstall, Toast)) { if (_session != null) { _session.Stop(); Session.StopAll(_ => { }); } Application.Exit(); }
        }
        catch (Exception e) { try { File.AppendAllText(Path.Combine(Paths.LauncherDir, "update.log"), $"{DateTime.Now:O} RunUpdateAsync {e}\r\n"); } catch { } }
    }

    // ---------------------------------------------------------------- self-test (developer)
    async Task SelfTestCaptureAsync()
    {
        await Task.Delay(2000);
        if (SelfTestScript != null) { await _web.CoreWebView2.ExecuteScriptAsync(SelfTestScript); await Task.Delay(800); }
        using (var fs = File.Create(SelfTestPng!)) await _web.CoreWebView2.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, fs);
        // window-frame facts, measured on the real window
        var report = new JsonObject
        {
            ["dpi"] = DeviceDpi, ["size"] = $"{Width}x{Height}", ["client"] = $"{ClientSize.Width}x{ClientSize.Height}",
            ["nonClientRegions"] = _web.CoreWebView2.Settings.IsNonClientRegionSupportEnabled,
            ["webviewVersion"] = _web.CoreWebView2.Environment.BrowserVersionString,
        };
        if (Environment.GetEnvironmentVariable("RADARNITE_SELFTEST_MAXIMIZE") != "1") { File.WriteAllText(Path.ChangeExtension(SelfTestPng!, ".json"), report.ToJsonString(new JsonSerializerOptions { WriteIndented = true })); Close(); return; }
        WindowState = FormWindowState.Maximized; await Task.Delay(400);
        var wa = Screen.FromControl(this).WorkingArea; var pt = PointToScreen(Point.Empty);
        report["maximizedClient"] = $"{ClientSize.Width}x{ClientSize.Height} at {pt.X},{pt.Y}; work area {wa.Width}x{wa.Height} at {wa.X},{wa.Y}";
        report["maximizedFitsWorkArea"] = pt.X == wa.X && pt.Y == wa.Y && ClientSize.Width == wa.Width && ClientSize.Height == wa.Height;
        WindowState = FormWindowState.Normal; await Task.Delay(300);
        report["restored"] = $"{Width}x{Height}";
        File.WriteAllText(Path.ChangeExtension(SelfTestPng!, ".json"), report.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        Close();
    }
}
