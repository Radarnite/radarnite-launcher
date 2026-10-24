using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace FnOffline;

static class Program
{
    /// <summary>
    /// RadarniteLauncher.exe                      window: list builds, add (folder/zip), generate cosmetics, firewall, play, stop
    /// RadarniteLauncher.exe --play 17.50         play that build (small status window; closes when the game closes)
    /// RadarniteLauncher.exe --stop               stop every offline build process (only those under the builds root)
    /// RadarniteLauncher.exe --firewall 17.50     (elevated) create the firewall rules for that build (+ node.exe)
    /// RadarniteLauncher.exe --set-builds-folder D:\Builds   (installer) where the builds live
    /// RadarniteLauncher.exe --firewall-remove    (elevated) remove every rule of the group "FN Offline Builds"
    /// RadarniteLauncher.exe --check 17.50        console report: version, support, firewall, preflight; starts nothing
    /// RadarniteLauncher.exe --cosmetics 17.50    download keys/mappings and generate cosmetics.json (console)
    /// RadarniteLauncher.exe --update-check       console: is a newer public release available? (installs nothing)
    /// RadarniteLauncher.exe --new-instance       developer: open the main window even if one is already open
    /// The main window is single-instance per Windows session (whatever folder the exe runs from): starting it again
    /// brings the open window to the front and exits. --play and the console commands are not affected.
    /// </summary>
    [STAThread]
    static int Main(string[] args)
    {
        var cmd = args.FirstOrDefault() ?? "";
        var arg = args.Skip(1).FirstOrDefault();
        BuildInfo? Find(string? v) => BuildInfo.All().FirstOrDefault(b => b.Name.Equals(v, StringComparison.OrdinalIgnoreCase) || b.Version == v);
        var report = new StringBuilder();
        void Out(string s) { report.AppendLine(s); try { Console.WriteLine(s); } catch { } }

        switch (cmd)
        {
            case "--stop":
                Session.StopAll(Out);
                return 0;
            case "--firewall":
            {
                var b = Find(arg);
                if (b == null) { Out($"No build named {arg}"); return 2; }
                var n = Firewall.Create(b.Dir);
                Out($"{n} firewall rules created for {b.Version}. Check: {Firewall.Check(b.Dir) ?? "OK"}");
                return Firewall.Check(b.Dir) == null ? 0 : 1;
            }
            case "--set-builds-folder": // installer: where the builds live (created if missing)
                if (string.IsNullOrWhiteSpace(arg)) { Out("usage: --set-builds-folder <folder>"); return 2; }
                var cfg = Config.Load();
                cfg.BuildsFolder = Directory.CreateDirectory(Path.GetFullPath(Environment.ExpandEnvironmentVariables(arg))).FullName;
                cfg.Save();
                Out($"Builds folder: {cfg.BuildsFolder}");
                return 0;
            case "--firewall-remove":
                Out($"{Firewall.RemoveAll()} rules of the group \"{Firewall.Group}\" removed.");
                return 0;
            case "--update-check": // report whether a newer public release exists (no window, nothing is installed)
            {
                var r = Updater.CheckAsync().GetAwaiter().GetResult();
                Out(r == null
                    ? $"Up to date (installed {Updater.Current}; repo {Updater.Repo})."
                    : $"Update available: {r.Version} (installed {Updater.Current}); installer {r.InstallerUrl}");
                return r == null ? 0 : 10;
            }
            case "--scan": // developer/test: list the builds under <root> (default: the launcher's root) and the ignored folders
            {
                var list = BuildInfo.Scan(arg ?? Paths.Root, out var skipped);
                foreach (var b in list) Out($"build {b.Name} | {b.Version} | CL {b.Cl} | dir {b.Dir} | lobby {b.Lobby.s} | match {b.Match.s}");
                foreach (var (d, why) in skipped) Out($"skip {Path.GetFileName(d)} | {why}");
                return 0;
            }
            case "--check":
            {
                var c = Config.Load();
                foreach (var b in BuildInfo.All().Where(b => arg == null || b.Name == arg || b.Version == arg))
                {
                    Out($"{b.Name}: Fortnite {b.Version} CL {b.Cl}, UE {b.Engine}");
                    Out($"  lobby: {b.Lobby.s} ({b.Lobby.why}); local match: {b.Match.s} ({b.Match.why})");
                    Out($"  cosmetics: {(File.Exists(Path.Combine(b.Dir, "offline", "cosmetics.json")) ? "yes" : "no")}; firewall: {Firewall.Check(b.Dir) ?? "OK"}");
                    Out($"  preflight: {Session.Preflight(b, c.Mode != "lobby") ?? "OK"}");
                }
                Out($"config: {c.DisplayName} mode={c.Mode} bots={c.Bots} port={c.Port}");
                return 0;
            }
            case "--selftest-inject": // developer: start <exe> wait <log>, inject the redirect DLL like a game launch
            {
                var psi = new ProcessStartInfo(arg!, $"wait \"{args[2]}\"") { UseShellExecute = false, RedirectStandardOutput = true };
                psi.Environment["FNOFFLINE_PORT"] = "3551"; psi.Environment["FNOFFLINE_LOG"] = args[2];
                using var p = Process.Start(psi)!;
                Thread.Sleep(500);
                Out("inject: " + Native.Inject(p.Id, Paths.RedirectDll));
                Out(p.StandardOutput.ReadToEnd());
                p.WaitForExit();
                return p.ExitCode;
            }
            case "--cosmetics":
            {
                var b = Find(arg);
                if (b == null) { Out($"No build named {arg}"); return 2; }
                return BuildData.GenerateCosmeticsAsync(b, Out).GetAwaiter().GetResult() ? 0 : 1;
            }
        }

        ApplicationConfiguration.Initialize();
        if (cmd == "--selftest-ui") // developer: open the real window invisibly, save a PNG + frame facts, exit
        {
            var f = new WebHost(null)
            {
                SelfTestPng = arg ?? Path.Combine(Paths.LauncherDir, "selftest-ui.png"),
                SelfTestScript = args.Length > 2 && File.Exists(args[2]) ? File.ReadAllText(args[2]) : null,
                StartPosition = FormStartPosition.Manual, Location = new Point(-32000, -32000), ShowInTaskbar = false,
            };
            Application.Run(f);
            return File.Exists(f.SelfTestPng) ? 0 : 1;
        }
        if (cmd == "--play")
        {
            var b = Find(arg);
            if (b == null) { MessageBox.Show($"Build {arg} not found in {Paths.Root}.", "Radarnite Launcher"); return 2; }
            Application.Run(WebHost.Available() ? new WebHost(b) : new PlayForm(b));
            return 0;
        }
        if (cmd != "--new-instance" && !SingleInstance.Claim()) return 0; // another main window is open: it was brought to the front
        Form main = WebHost.Available() ? new WebHost(null) : new MainForm();
        SingleInstance.Listen(main);
        Application.Run(main);
        SingleInstance.Release();
        return 0;
    }

    /// <summary>Desktop shortcut "Fortnite &lt;build&gt; Offline" that plays that build.</summary>
    public static string CreateShortcut(BuildInfo b)
    {
        var desk = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        var path = Path.Combine(desk, $"Fortnite {b.Name} Offline.lnk");
        dynamic sh = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell")!)!;
        dynamic lnk = sh.CreateShortcut(path);
        lnk.TargetPath = Environment.ProcessPath; lnk.Arguments = $"--play \"{b.Name}\""; lnk.WorkingDirectory = Paths.LauncherDir;
        lnk.IconLocation = Paths.Shipping(b.Dir) + ",0"; lnk.Save();
        return path;
    }

    /// <summary>Runs this exe elevated (UAC prompt) for the firewall commands.</summary>
    public static bool RunElevated(string args)
    {
        try
        {
            var p = Process.Start(new ProcessStartInfo(Environment.ProcessPath!, args) { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden });
            p!.WaitForExit();
            return p.ExitCode == 0;
        }
        catch { return false; } // UAC cancelled
    }
}

/// <summary>
/// One main window per Windows session. Every copy of the launcher (installed, or a dev tree such as
/// S:\Fortnite\FortniteBuilds\launcher) shares the same session-local mutex, so opening it again (Start menu, a pinned icon,
/// a double-click on the exe) never stacks another window + WebView2 on top of the first one: the new process signals
/// the named event, the open window restores itself and comes to the front, and the new process exits.
/// </summary>
static class SingleInstance
{
    const string MutexName = @"Local\RadarniteLauncher.MainWindow", ShowEventName = @"Local\RadarniteLauncher.ShowMainWindow";
    static Mutex? _mutex;
    static EventWaitHandle? _show;

    [DllImport("user32")] static extern bool AllowSetForegroundWindow(int pid);
    [DllImport("user32")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32")] static extern bool SetForegroundWindow(IntPtr h);

    /// <summary>True when this process owns the main window. Otherwise the open one is asked to come to the front.</summary>
    public static bool Claim()
    {
        try
        {
            _mutex = new Mutex(true, MutexName, out var created);
            if (created) return true;
            // An open main window listens on the event: ask it to come to the front and step aside.
            if (EventWaitHandle.TryOpenExisting(ShowEventName, out var ev))
            {
                using (ev)
                {
                    try { AllowSetForegroundWindow(-1 /*ASFW_ANY*/); } catch { } // we were just started by the user: hand the foreground over
                    ev.Set();
                }
                _mutex.Dispose(); _mutex = null;
                return false;
            }
            // No listener: the owner is exiting (e.g. closing for an update) or still starting. Wait for it briefly.
            try { if (_mutex.WaitOne(TimeSpan.FromSeconds(3))) return true; }
            catch (AbandonedMutexException) { return true; } // the previous owner died without releasing it: it is ours now
            _mutex.Dispose(); _mutex = null;
            return false;
        }
        catch { _mutex = null; return true; } // never block the launcher on a mutex problem
    }

    /// <summary>Owner side: restore and focus <paramref name="main"/> (or the form it handed over to) when signalled.</summary>
    public static void Listen(Form main)
    {
        if (_mutex == null) return;
        try { _show = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName); } catch { return; }
        var ev = _show;
        new Thread(() =>
        {
            try
            {
                while (ev.WaitOne())
                {
                    if (main.IsDisposed) return;
                    try { main.BeginInvoke(() => BringToFront(main)); } catch (InvalidOperationException) { } // handle not created yet
                }
            }
            catch (ObjectDisposedException) { }
        }) { IsBackground = true, Name = "single-instance" }.Start();
    }

    static void BringToFront(Form main)
    {
        // WebHost hands over to the plain WinForms window when WebView2 fails: focus whichever one is showing.
        var f = Application.OpenForms.Cast<Form>().LastOrDefault(x => x.Visible && x.ShowInTaskbar) ?? main;
        if (!f.Visible) f.Show();
        if (IsIconic(f.Handle)) ShowWindow(f.Handle, 9 /*SW_RESTORE: back to normal or maximized, as it was*/);
        f.Activate();
        SetForegroundWindow(f.Handle);
    }

    /// <summary>Called when the main window's message loop ends, so a new launcher can start right away.</summary>
    public static void Release()
    {
        try { _show?.Dispose(); } catch { }
        _show = null;
        try { _mutex?.ReleaseMutex(); } catch { }
        try { _mutex?.Dispose(); } catch { }
        _mutex = null;
    }
}

/// <summary>The play window of a desktop shortcut: status log; closing it stops everything.</summary>
sealed class PlayForm : Form
{
    readonly TextBox _log = new() { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 9) };
    readonly BuildInfo _b;
    Session? _s;

    public PlayForm(BuildInfo b)
    {
        _b = b;
        Text = $"Fortnite {b.Version} Offline";
        Width = 720; Height = 360;
        var stop = new Button { Text = "Stop everything", Dock = DockStyle.Bottom, Height = 34 };
        stop.Click += (_, _) => Close();
        Controls.Add(_log); Controls.Add(stop);
        Shown += async (_, _) =>
        {
            var c = Config.Load();
            Log($"Fortnite {b.Version} (CL {b.Cl}) - mode {c.Mode}" + (c.Mode == "bots" ? $", {c.Bots} bots" : "") + $" - player \"{c.DisplayName}\"");
            _s = new Session(b, c, Log);
            if (!await _s.StartAsync()) { Log("You can close this window."); return; }
            WindowState = FormWindowState.Minimized;
            await _s.WaitClientAsync();
            Close();
        };
        FormClosing += (_, _) => { _s?.Stop(); Session.StopAll(_ => { }); };
    }

    void Log(string s)
    {
        if (InvokeRequired) { BeginInvoke(() => Log(s)); return; }
        _log.AppendText($"{DateTime.Now:HH:mm:ss} {s}\r\n");
        try { File.AppendAllText(Path.Combine(Paths.Offline(_b.Dir), "launcher.log"), $"{DateTime.Now:O} {s}\r\n"); } catch { }
    }
}

/// <summary>Main window: the builds under the root, and what can be done with each.</summary>
sealed class MainForm : Form
{
    readonly ListView _list = new() { View = View.Details, FullRowSelect = true, Dock = DockStyle.Fill, MultiSelect = false };
    readonly TextBox _log = new() { Multiline = true, ReadOnly = true, Dock = DockStyle.Bottom, Height = 170, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 9) };
    readonly ComboBox _mode = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 150 };
    readonly NumericUpDown _bots = new() { Minimum = 0, Maximum = 99, Width = 60 };
    readonly TextBox _name = new() { Width = 150 };
    Config _c = Config.Load();

    public MainForm()
    {
        Text = "Fortnite Offline Builds";
        Width = 980; Height = 620;
        foreach (var (h, w) in new[] { ("Build", 90), ("Version", 80), ("CL", 90), ("Engine", 60), ("Lobby", 110), ("Local match", 120), ("Cosmetics", 90), ("Firewall", 260) })
            _list.Columns.Add(h, w);

        var bar = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 74, Padding = new Padding(4) };
        Button B(string t, Func<Task> f) { var b = new Button { Text = t, AutoSize = true, Height = 30 }; b.Click += async (_, _) => { Enabled = false; try { await f(); } catch (Exception e) { Log("Error: " + e.Message); } finally { Enabled = true; Refill(); } }; bar.Controls.Add(b); return b; }
        B("▶ Play", Play);
        B("■ Stop everything", () => { Session.StopAll(Log); return Task.CompletedTask; });
        B("Add build (zip)…", AddZip);
        B("Add build (folder)…", AddFolder);
        B("Generate cosmetics", async () => { if (Sel() is { } b) Log((await BuildData.GenerateCosmeticsAsync(b, Log)) ? "Cosmetics ready." : "Could not generate the cosmetics."); });
        B("Create firewall rules", () => { if (Sel() is { } b) Log(Program.RunElevated($"--firewall \"{b.Name}\"") ? "Firewall rules created." : "The rules were not created (was the administrator prompt cancelled?)."); return Task.CompletedTask; });
        B("Create desktop shortcut", () => { if (Sel() is { } b) Shortcut(b); return Task.CompletedTask; });
        B("Open config.json", () => { Process.Start(new ProcessStartInfo("notepad.exe", $"\"{Paths.Config}\"")); return Task.CompletedTask; });
        B("Check for updates", async () => { if (await Updater.RunInteractiveAsync(this, manual: true, canInstall: true, (t, _) => Log(t))) Close(); });

        _mode.Items.AddRange(["lobby", "practice", "bots"]);
        _mode.SelectedItem = _c.Mode; _bots.Value = Math.Clamp(_c.Bots, 0, 99); _name.Text = _c.DisplayName;
        bar.Controls.Add(new Label { Text = "Mode:", AutoSize = true, Padding = new Padding(0, 8, 0, 0) }); bar.Controls.Add(_mode);
        bar.Controls.Add(new Label { Text = "Bots:", AutoSize = true, Padding = new Padding(0, 8, 0, 0) }); bar.Controls.Add(_bots);
        bar.Controls.Add(new Label { Text = "Name:", AutoSize = true, Padding = new Padding(0, 8, 0, 0) }); bar.Controls.Add(_name);
        _mode.SelectedIndexChanged += (_, _) => SaveCfg(); _bots.ValueChanged += (_, _) => SaveCfg(); _name.Leave += (_, _) => SaveCfg();

        Controls.Add(_list); Controls.Add(_log); Controls.Add(bar);
        Shown += async (_, _) =>
        {
            Refill();
            Log($"Builds in {Paths.Root}. Pick the mode in the game (lobby -> CHANGE).");
            if (await Updater.RunInteractiveAsync(this, manual: false, canInstall: true, (t, _) => Log(t))) Close();
        };
    }

    void SaveCfg() { _c = Config.Load(); _c.Mode = (string)_mode.SelectedItem!; _c.Bots = (int)_bots.Value; if (!string.IsNullOrWhiteSpace(_name.Text)) _c.DisplayName = _name.Text.Trim(); _c.Save(); }

    BuildInfo? Sel()
    {
        if (_list.SelectedItems.Count == 0) { Log("Pick a build from the list."); return null; }
        return (BuildInfo)_list.SelectedItems[0].Tag!;
    }

    void Refill()
    {
        _list.Items.Clear();
        foreach (var b in BuildInfo.All())
        {
            string fw; try { fw = Firewall.Check(b.Dir) ?? "OK"; } catch (Exception e) { fw = e.Message; }
            var it = new ListViewItem([b.Name, b.Version, b.Cl.ToString(), b.Engine, b.Lobby.s.ToString(), File.Exists(Paths.GameServerDll) ? b.Match.s.ToString() : "match server missing",
                File.Exists(Path.Combine(b.Dir, "offline", "cosmetics.json")) ? "yes" : "no", fw]) { Tag = b, ToolTipText = $"{b.Lobby.why}\n{b.Match.why}" };
            _list.Items.Add(it);
        }
        if (_list.Items.Count > 0 && _list.SelectedItems.Count == 0) _list.Items[0].Selected = true;
    }

    async Task Play()
    {
        if (Sel() is not { } b) return;
        SaveCfg();
        if (Firewall.Check(b.Dir) is { } fw)
        {
            Log("Firewall: " + fw);
            if (MessageBox.Show("The firewall rules that keep this build off the internet are missing. Create them now? (asks for administrator permission)", "Firewall", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
            if (!Program.RunElevated($"--firewall \"{b.Name}\"")) { Log("Not starting without the firewall rules."); return; }
        }
        Process.Start(new ProcessStartInfo(Environment.ProcessPath!, $"--play \"{b.Name}\"") { UseShellExecute = false });
        await Task.CompletedTask;
    }

    async Task AddZip()
    {
        using var d = new OpenFileDialog { Filter = "Fortnite build (*.zip)|*.zip", Title = "Pick the build zip" };
        if (d.ShowDialog() != DialogResult.OK) return;
        Log($"Extracting {d.FileName} into {Paths.Root} (this can take a long time)...");
        if (await BuildData.AddZipAsync(d.FileName, Log) is { } b) await AfterAdd(b);
    }

    async Task AddFolder()
    {
        using var d = new FolderBrowserDialog { Description = "Build folder (the one that contains FortniteGame)" };
        if (d.ShowDialog() != DialogResult.OK) return;
        if (await BuildData.AddFolderAsync(d.SelectedPath, Log) is { } b) await AfterAdd(b);
    }

    async Task AfterAdd(BuildInfo b)
    {
        Log($"Fortnite {b.Version}: lobby {b.Lobby.s} ({b.Lobby.why}); local match {b.Match.s} ({b.Match.why}).");
        if (b.Lobby.s == Support.No) return;
        Log("Generating the cosmetics list from the build's own data...");
        Log(await BuildData.GenerateCosmeticsAsync(b, Log) ? "Cosmetics ready." : "Could not generate the cosmetics (see above).");
        if (Firewall.Check(b.Dir) != null && MessageBox.Show($"Create the firewall rules for {b.Version} now? (asks for administrator permission)", "Firewall", MessageBoxButtons.YesNo) == DialogResult.Yes)
            Program.RunElevated($"--firewall \"{b.Name}\"");
    }

    void Shortcut(BuildInfo b)
    {
        var desk = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        var path = Path.Combine(desk, $"Fortnite {b.Name} Offline.lnk");
        dynamic sh = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell")!)!;
        dynamic lnk = sh.CreateShortcut(path);
        lnk.TargetPath = Environment.ProcessPath; lnk.Arguments = $"--play \"{b.Name}\""; lnk.WorkingDirectory = Paths.LauncherDir;
        var icon = Paths.Shipping(b.Dir); lnk.IconLocation = icon + ",0"; lnk.Save();
        Log($"Shortcut created: {path}");
    }

    void Log(string s)
    {
        if (InvokeRequired) { BeginInvoke(() => Log(s)); return; }
        _log.AppendText($"{DateTime.Now:HH:mm:ss} {s}\r\n");
    }
}
