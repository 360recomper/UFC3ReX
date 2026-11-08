using System.Diagnostics;
using System.Text;

namespace UFC3Launcher;

/// <summary>The experimental native renderer (ufc3_renderer in ufc3.toml). Only builds made with
/// `build.bat native` have it; the launcher looks for its startup log line in ufc3.exe.</summary>
public static class NativeRenderer
{
    /// <summary>ufc3.exe exits with this when the native device can't be created (src/ufc3_app.h).</summary>
    public const int DeviceFailedExitCode = 3;

    // Logged only by builds with the native hooks (UFC3_NATIVE_HOOKS, src/ufc3_app.h LatchRenderer).
    static readonly byte[] Marker = Encoding.ASCII.GetBytes("native: renderer on (experimental");

    static string? stamp;
    static bool available;

    /// <summary>True when ufc3.exe has the native renderer compiled in. Scans the exe once per size + date.</summary>
    public static bool Available
    {
        get
        {
            try
            {
                var fi = new FileInfo(Paths.GameExe);
                if (!fi.Exists) return false;
                var s = $"{fi.Length}:{fi.LastWriteTimeUtc.Ticks}";
                if (s != stamp) { available = ExeHasMarker(fi.FullName); stamp = s; }
                return available;
            }
            catch { return false; }
        }
    }

    static bool ExeHasMarker(string exe)
    {
        using var f = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16);
        var buf = new byte[4 << 20];
        int keep = 0, n;
        while ((n = f.Read(buf, keep, buf.Length - keep)) > 0)
        {
            int len = keep + n;
            if (buf.AsSpan(0, len).IndexOf(Marker) >= 0) return true;
            // Keep the tail so a marker split across two reads is still found.
            keep = Math.Min(Marker.Length - 1, len);
            Array.Copy(buf, len - keep, buf, 0, keep);
        }
        return false;
    }

    /// <summary>Renderer = Native in ufc3.toml and this exe has it.</summary>
    public static bool Selected =>
        new Toml().GetStr("ufc3_renderer", "native").Equals("native", StringComparison.OrdinalIgnoreCase) && Available;

    /// <summary>Makes sure the shader cache is there for a native run (builds it if not). On failure the renderer goes
    /// back to Emulated in ufc3.toml and the reason comes back; null = ready. Blocks: call off the UI thread.</summary>
    public static string? PrepareCache()
    {
        var r = ShaderCache.EnsurePresent(Paths.Assets);
        if (r.Ok) return null;
        UseEmulated();
        return r.Message;
    }

    /// <summary>ufc3_renderer = emulated in ufc3.toml. A file that can't be written only matters to later runs: this
    /// run gets the renderer on its command line (<see cref="LaunchEmulated"/>).</summary>
    public static void UseEmulated()
    {
        try { var t = new Toml(); t.SetStr("ufc3_renderer", "emulated"); t.Save(); Log("renderer set back to emulated in ufc3.toml"); }
        catch (Exception ex) { Log("ERROR: could not write ufc3.toml: " + ex.Message); }
    }

    /// <summary>What the player reads when a cache build failed and the game falls back.</summary>
    public static string FallbackText(string reason) =>
        "The native renderer's shader cache could not be built, so the renderer is set back to Emulated.\n\n" +
        reason + "\n\nThe game still runs normally on Emulated. Details: launcher-native.log";

    /// <summary>Starts a build that has native code on the emulated renderer, whatever ufc3.toml says.</summary>
    public static Process LaunchEmulated()
    {
        var psi = new ProcessStartInfo(Paths.GameExe) { WorkingDirectory = Paths.Root, UseShellExecute = false };
        psi.ArgumentList.Add("--ufc3_renderer=emulated");
        return Process.Start(psi) ?? throw new InvalidOperationException("ufc3.exe did not start");
    }

    /// <summary>Starts the game on the native renderer and waits for it with the launcher hidden. When the native
    /// device can't be created the game exits with <see cref="DeviceFailedExitCode"/> and is started again on the
    /// emulated renderer for that run only (ufc3.toml is not changed). Logged to launcher-native.log.</summary>
    public static void PlayFrom(Control from)
    {
        var game = Launch();
        var form = from.FindForm();
        form?.Hide();
        new Thread(() =>
        {
            WaitAndFallBack(game);
            if (form != null && form.IsHandleCreated) form.BeginInvoke(Application.Exit); else Application.Exit();
        }) { IsBackground = false, Name = "native-wait" }.Start();
    }

    public static Process Launch() =>
        Process.Start(new ProcessStartInfo(Paths.GameExe) { WorkingDirectory = Paths.Root, UseShellExecute = false })
        ?? throw new InvalidOperationException("ufc3.exe did not start");

    /// <summary>Waits for a native run; on <see cref="DeviceFailedExitCode"/> starts the game on the emulated renderer
    /// (not waited for).</summary>
    public static void WaitAndFallBack(Process game)
    {
        try
        {
            game.WaitForExit();
            if (game.ExitCode != DeviceFailedExitCode) return;
            Log("native device failed (exit code 3): restarted on the emulated renderer");
            LaunchEmulated();
        }
        catch (Exception ex) { Log("ERROR: " + ex.Message); }
    }

    internal static void Log(string line)
    {
        try { File.AppendAllText(Path.Combine(Paths.Root, "launcher-native.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}"); }
        catch { }
    }
}
