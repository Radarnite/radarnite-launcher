using System.Runtime.InteropServices;

namespace FnOffline;

/// <summary>
/// Undoes a mute that Windows kept for the game exe. Windows stores each app's session volume and mute per exe path and
/// restores them on the next run, so a test tool that once muted FortniteClient-Win64-Shipping.exe (Core Audio
/// ISimpleAudioVolume::SetMute) left every later game of that build silent. Session.StartClientAsync runs Restore for
/// the client it started (never for the match server, never for a -nosound test run).
///
/// Core Audio is called through raw vtable slots (IUnknown = slots 0-2), not [ComImport] interfaces: radarnite-play.exe
/// is trimmed, and trimming turns the runtime's built-in COM interop off. Slots, from the Windows SDK headers
/// (mmdeviceapi.h, audiopolicy.h):
///   IMMDeviceEnumerator   3 EnumAudioEndpoints
///   IMMDeviceCollection   3 GetCount, 4 Item
///   IMMDevice             3 Activate
///   IAudioSessionManager2 5 GetSessionEnumerator (3-4 are IAudioSessionManager's)
///   IAudioSessionEnumerator 3 GetCount, 4 GetSession
///   IAudioSessionControl2 14 GetProcessId (3-11 IAudioSessionControl, 12 GetSessionIdentifier, 13 GetSessionInstanceIdentifier)
///   ISimpleAudioVolume    3 SetMasterVolume, 4 GetMasterVolume, 5 SetMute, 6 GetMute
/// </summary>
public static unsafe class Audio
{
    static readonly Guid ClsidEnumerator = new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    static readonly Guid IidEnumerator = new("A95664D2-9614-4F35-A746-DE8DB63617E6");
    static readonly Guid IidSessionManager2 = new("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F");
    static readonly Guid IidSessionControl2 = new("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d");
    static readonly Guid IidSimpleVolume = new("87CE5498-68D6-44E5-9215-6DA47EF883D8");
    const int ERender = 0, DeviceStateActive = 1, ClsctxAll = 0x17, CoinitMultithreaded = 0;

    [DllImport("ole32")] static extern int CoInitializeEx(IntPtr reserved, int coinit);
    [DllImport("ole32")] static extern void CoUninitialize();
    [DllImport("ole32")] static extern int CoCreateInstance(in Guid clsid, IntPtr outer, int ctx, in Guid iid, out IntPtr ppv);

    static void** Vt(IntPtr p) => *(void***)p;

    /// <summary>One audio session of a process on an active render device.</summary>
    public readonly record struct State(uint Pid, bool Muted, float Volume);

    /// <summary>Calls <paramref name="each"/> with the ISimpleAudioVolume of every session of <paramref name="pids"/> on every
    /// active render device. The caller's thread must be in a COM apartment (Restore and the selftest use MTA threads).</summary>
    static void ForEachSession(IReadOnlyCollection<uint> pids, Action<uint, IntPtr> each)
    {
        if (CoCreateInstance(ClsidEnumerator, IntPtr.Zero, ClsctxAll, IidEnumerator, out var en) != 0 || en == IntPtr.Zero) return;
        try
        {
            IntPtr devs;
            if (((delegate* unmanaged[Stdcall]<IntPtr, int, int, IntPtr*, int>)Vt(en)[3])(en, ERender, DeviceStateActive, &devs) != 0 || devs == IntPtr.Zero) return;
            try
            {
                uint nd;
                if (((delegate* unmanaged[Stdcall]<IntPtr, uint*, int>)Vt(devs)[3])(devs, &nd) != 0) return;
                for (uint d = 0; d < nd; d++)
                {
                    IntPtr dev;
                    if (((delegate* unmanaged[Stdcall]<IntPtr, uint, IntPtr*, int>)Vt(devs)[4])(devs, d, &dev) != 0 || dev == IntPtr.Zero) continue;
                    try { DeviceSessions(dev, pids, each); }
                    finally { Marshal.Release(dev); }
                }
            }
            finally { Marshal.Release(devs); }
        }
        finally { Marshal.Release(en); }
    }

    static void DeviceSessions(IntPtr dev, IReadOnlyCollection<uint> pids, Action<uint, IntPtr> each)
    {
        IntPtr mgr, se;
        var iid = IidSessionManager2;
        if (((delegate* unmanaged[Stdcall]<IntPtr, Guid*, int, IntPtr, IntPtr*, int>)Vt(dev)[3])(dev, &iid, ClsctxAll, IntPtr.Zero, &mgr) != 0 || mgr == IntPtr.Zero) return;
        try
        {
            if (((delegate* unmanaged[Stdcall]<IntPtr, IntPtr*, int>)Vt(mgr)[5])(mgr, &se) != 0 || se == IntPtr.Zero) return;
            try
            {
                int n;
                if (((delegate* unmanaged[Stdcall]<IntPtr, int*, int>)Vt(se)[3])(se, &n) != 0) return;
                for (var i = 0; i < n; i++)
                {
                    IntPtr ctl;
                    if (((delegate* unmanaged[Stdcall]<IntPtr, int, IntPtr*, int>)Vt(se)[4])(se, i, &ctl) != 0 || ctl == IntPtr.Zero) continue;
                    try
                    {
                        if (Marshal.QueryInterface(ctl, in IidSessionControl2, out var ctl2) != 0 || ctl2 == IntPtr.Zero) continue;
                        uint pid = 0;
                        try { if (((delegate* unmanaged[Stdcall]<IntPtr, uint*, int>)Vt(ctl2)[14])(ctl2, &pid) != 0) continue; }
                        finally { Marshal.Release(ctl2); }
                        if (!pids.Contains(pid)) continue;
                        if (Marshal.QueryInterface(ctl, in IidSimpleVolume, out var vol) != 0 || vol == IntPtr.Zero) continue;
                        try { each(pid, vol); }
                        finally { Marshal.Release(vol); }
                    }
                    finally { Marshal.Release(ctl); }
                }
            }
            finally { Marshal.Release(se); }
        }
        finally { Marshal.Release(mgr); }
    }

    static (bool muted, float volume) Read(IntPtr vol)
    {
        int m = 0; float v = 1f;
        ((delegate* unmanaged[Stdcall]<IntPtr, int*, int>)Vt(vol)[6])(vol, &m);
        ((delegate* unmanaged[Stdcall]<IntPtr, float*, int>)Vt(vol)[4])(vol, &v);
        return (m != 0, v);
    }

    /// <summary>Unmutes every session of <paramref name="pids"/> and puts a master volume of 0 back to 1; returns what it
    /// changed (the state before).</summary>
    public static List<State> Unmute(IReadOnlyCollection<uint> pids)
    {
        var changed = new List<State>();
        ForEachSession(pids, (pid, vol) =>
        {
            var (m, v) = Read(vol);
            if (!m && v > 0f) return;
            if (m) ((delegate* unmanaged[Stdcall]<IntPtr, int, Guid*, int>)Vt(vol)[5])(vol, 0, null);
            if (v <= 0f) ((delegate* unmanaged[Stdcall]<IntPtr, float, Guid*, int>)Vt(vol)[3])(vol, 1f, null);
            changed.Add(new State(pid, m, v));
        });
        return changed;
    }

    /// <summary>Sets mute and master volume on every session of <paramref name="pids"/> (the selftest plays the test tool).</summary>
    public static int Set(IReadOnlyCollection<uint> pids, bool mute, float volume)
    {
        var n = 0;
        ForEachSession(pids, (_, vol) =>
        {
            ((delegate* unmanaged[Stdcall]<IntPtr, float, Guid*, int>)Vt(vol)[3])(vol, volume, null);
            ((delegate* unmanaged[Stdcall]<IntPtr, int, Guid*, int>)Vt(vol)[5])(vol, mute ? 1 : 0, null);
            n++;
        });
        return n;
    }

    /// <summary>The sessions of <paramref name="pids"/> as they are now.</summary>
    public static List<State> Sessions(IReadOnlyCollection<uint> pids)
    {
        var all = new List<State>();
        ForEachSession(pids, (pid, vol) => { var (m, v) = Read(vol); all.Add(new State(pid, m, v)); });
        return all;
    }

    /// <summary>Runs <paramref name="work"/> on its own MTA thread (Core Audio needs a COM apartment).</summary>
    public static Task<T> OnComThread<T>(Func<T> work)
    {
        var done = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        var t = new Thread(() =>
        {
            var hr = CoInitializeEx(IntPtr.Zero, CoinitMultithreaded);
            try { done.SetResult(work()); }
            catch (Exception e) { done.SetException(e); }
            finally { if (hr >= 0) CoUninitialize(); }
        }) { IsBackground = true, Name = "audio" };
        t.Start();
        return done.Task;
    }

    /// <summary>Watches the client's audio sessions while the game starts and unmutes any that Windows brought back muted
    /// (or at volume 0). The game opens its session during engine start, after a time that depends on the build: poll
    /// every second until the client exits, 30 s after its first session appeared, or 180 s in all. Never throws.</summary>
    public static Task Restore(System.Diagnostics.Process client, Action<string> log) => OnComThread(() =>
    {
        try
        {
            var pids = new[] { (uint)client.Id };
            var start = DateTime.UtcNow;
            DateTime? seen = null;
            while (DateTime.UtcNow - start < TimeSpan.FromSeconds(180) && (seen == null || DateTime.UtcNow - seen < TimeSpan.FromSeconds(30)))
            {
                if (client.HasExited) break;
                try
                {
                    foreach (var s in Unmute(pids))
                    {
                        var what = s.Muted && s.Volume <= 0f ? "muted and at volume 0" : s.Muted ? "muted" : "at volume 0";
                        log($"Game sound: Windows had kept the game {what} (volume mixer, from an earlier run); unmuted it.");
                    }
                    if (seen == null && Sessions(pids).Count > 0) seen = DateTime.UtcNow;
                }
                catch { }
                Thread.Sleep(1000);
            }
        }
        catch { }
        return true;
    });
}
