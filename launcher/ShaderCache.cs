using System.ComponentModel;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace UFC3Launcher;

/// <summary>The native renderer's shader cache, built on the player's machine from their own disc files by
/// ufc3_shader_cache.exe (tools/native/shader_extract). The game reads shader_cache.bin + shader_cache_linked.bin
/// from where the cvar ufc3_shader_cache points; a relative path means beside the exe (guest_shaders.cpp).
/// Only one build runs at a time in this launcher: a second caller waits for the first.</summary>
public static class ShaderCache
{
    public const string ToolName = "ufc3_shader_cache.exe";
    // What makes the cache: an update that changes any of these changes the shaders (the bloom / DOF patterns are
    // built into the first one).
    static readonly string[] Makers = { ToolName, "XenosRecomp.exe", "ufc3_prelink.exe" };
    // Takes seconds on the machines tried. This only stops a stuck tool from holding up Play forever.
    static readonly TimeSpan Limit = TimeSpan.FromMinutes(15);

    static readonly object gate = new();
    static Process? running;

    static ShaderCache()
    {
        // The launcher closing mid-build takes the tool with it (the tool's job object takes its children).
        AppDomain.CurrentDomain.ProcessExit += (_, _) =>
        {
            try { running?.Kill(true); } catch { }
        };
    }

    public sealed record Result(bool Ok, string Message);

    public static string Tool => Path.Combine(Paths.Root, ToolName);

    public static string CacheFile
    {
        get
        {
            var set = new Toml().Get("ufc3_shader_cache");
            return Path.GetFullPath(string.IsNullOrWhiteSpace(set) ? "shader_cache.bin" : set, Paths.Root);
        }
    }

    public static string LinkedFile
    {
        get
        {
            var c = CacheFile;
            return Path.Combine(Path.GetDirectoryName(c)!, Path.GetFileNameWithoutExtension(c) + "_linked" + Path.GetExtension(c));
        }
    }

    /// <summary>True when both files are there. Whether they belong together is the game's check (it refuses a
    /// pair that doesn't match).</summary>
    public static bool Present
    {
        get
        {
            try
            {
                var a = new FileInfo(CacheFile); var b = new FileInfo(LinkedFile);
                return a.Exists && a.Length > 0 && b.Exists && b.Length > 0;
            }
            catch { return false; }
        }
    }

    /// <summary>Beside the cache: the hash of the tools that built it, written after a good build.</summary>
    public static string StampFile => Path.ChangeExtension(CacheFile, ".stamp");

    /// <summary>SHA-256 of each tool in the game folder ("-" when missing), one line each.</summary>
    static string ToolStamp()
    {
        var sb = new StringBuilder();
        foreach (var n in Makers)
        {
            var f = Path.Combine(Paths.Root, n);
            string h;
            try { using var s = File.OpenRead(f); h = Convert.ToHexString(SHA256.HashData(s)); }
            catch { h = "-"; }
            sb.Append(n).Append(' ').Append(h).Append('\n');
        }
        return sb.ToString();
    }

    /// <summary>Present and built by the tools in the game folder now. A cache from before an update (or with no
    /// stamp, from before stamps) is rebuilt.</summary>
    public static bool Current
    {
        get
        {
            if (!Present) return false;
            try { return File.ReadAllText(StampFile) == ToolStamp(); }
            catch { return false; }
        }
    }

    /// <summary>Builds the cache unless a current one is already there.</summary>
    public static Result EnsurePresent(string gameData, IProgress<string>? progress = null, CancellationToken ct = default)
    {
        lock (gate)
        {
            if (Current) return new Result(true, "The shader cache is already there.");
            if (Present) NativeRenderer.Log("shader cache: built by other tools than the ones here (update?), rebuilding");
            return Build(gameData, progress, ct);
        }
    }

    /// <summary>Runs the tool and waits. A failure is returned, not thrown; only cancelling throws. A failed run leaves
    /// an older cache in place (the tool moves new files in at the very end).</summary>
    public static Result Build(string gameData, IProgress<string>? progress = null, CancellationToken ct = default)
    {
        lock (gate)
        {
            NativeRenderer.Log($"shader cache build: \"{gameData}\" -> \"{CacheFile}\"");
            var r = Run(gameData, progress, ct);
            NativeRenderer.Log(r.Ok ? "shader cache build: done" : "shader cache build FAILED: " + r.Message);
            return r;
        }
    }

    static Result Run(string gameData, IProgress<string>? progress, CancellationToken ct)
    {
        var tool = Tool;
        if (!File.Exists(tool)) return new Result(false, $"{ToolName} is not in the game folder ({Paths.Root}).");
        var psi = new ProcessStartInfo(tool)
        {
            WorkingDirectory = Paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        psi.ArgumentList.Add(gameData);
        psi.ArgumentList.Add(Path.GetDirectoryName(CacheFile)!);

        var err = new List<string>();
        using var p = new Process { StartInfo = psi };
        p.OutputDataReceived += (_, e) =>
        {
            if (string.IsNullOrEmpty(e.Data)) return;
            NativeRenderer.Log("  " + e.Data);
            progress?.Report(e.Data);
        };
        p.ErrorDataReceived += (_, e) =>
        {
            if (string.IsNullOrEmpty(e.Data)) return;
            lock (err) err.Add(e.Data);
        };
        try { p.Start(); }
        catch (Exception ex) when (ex is Win32Exception or InvalidOperationException)
        {
            return new Result(false, $"{ToolName} could not start: {ex.Message}");
        }
        running = p;
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        try
        {
            var sw = Stopwatch.StartNew();
            while (!p.WaitForExit(200))
            {
                if (ct.IsCancellationRequested)
                {
                    Stop(p);
                    ct.ThrowIfCancellationRequested();
                }
                if (sw.Elapsed > Limit)
                {
                    Stop(p);
                    return new Result(false, $"{ToolName} was still running after {Limit.TotalMinutes:0} minutes and was stopped.");
                }
            }
            p.WaitForExit(); // the last output lines
        }
        finally { running = null; }

        string[] errLines;
        lock (err) errLines = err.ToArray();
        foreach (var l in errLines.Where(l => l.Contains(": error: ", StringComparison.Ordinal))) NativeRenderer.Log("  " + l);

        if (p.ExitCode != 0) return new Result(false, Explain(p.ExitCode, errLines, gameData));
        if (!Present) return new Result(false, $"{ToolName} finished, but {Path.GetFileName(CacheFile)} and its linked file are not in {Path.GetDirectoryName(CacheFile)}.");
        // Without a stamp the next Play only builds it again.
        try { File.WriteAllText(StampFile, ToolStamp()); }
        catch (Exception ex) { NativeRenderer.Log("shader cache: could not write " + StampFile + ": " + ex.Message); }
        return new Result(true, "Shader cache built.");
    }

    static void Stop(Process p)
    {
        try { p.Kill(true); p.WaitForExit(5000); } catch { }
    }

    /// <summary>Exit codes: FINDINGS-native-renderer.md "NR-7c", tools/native/shader_extract/main.cpp.</summary>
    static string Explain(int code, string[] err, string gameData)
    {
        var what = code switch
        {
            1 => $"{ToolName} rejected the launcher's arguments (launcher bug).",
            2 => $"The game files in {gameData} are missing or can't be used.",
            3 => $"The shader cache folder can't be written, or another shader cache build is running there.",
            4 => "XenosRecomp.exe, which translates the shaders, is missing or failed.",
            5 => "ufc3_prelink.exe, which links the shaders, is missing or failed.",
            _ => $"{ToolName} stopped unexpectedly (exit code {code}).",
        };
        const string Prefix = "error: ";
        var line = err.FirstOrDefault(l => l.Contains(": " + Prefix, StringComparison.Ordinal));
        if (line == null) return what;
        return what + "\n" + line[(line.IndexOf(": " + Prefix, StringComparison.Ordinal) + 2 + Prefix.Length)..];
    }
}
