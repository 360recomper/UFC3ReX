using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;

namespace UFC3Launcher;

/// <summary>
/// Optional DLSS 5 (neural rendering) at 4K60, the Rumble Roses recipe: Vulkan + ReShade 6.8.0 as a Vulkan layer for
/// this one process + DLSS5-Feeder 1.19.0 helper add-on + 64-bit host with OptiScaler-NR 0.8.92 + nvngx_dlssnr 310.9.16.
/// Everything lives in dlss5\ next to ufc3.exe. The SAME ufc3.exe is started, no copy:
///  - the layer comes from VK_ADD_IMPLICIT_LAYER_PATH=dlss5\ReShade64.json (set for the game process only);
///  - ReShade 6.8 as a layer only starts in an exe whose folder has a ReShade.ini, and uses that folder as its base
///    path (a ReShade.ini next to ReShade64.dll is ignored). So ReShade.ini goes next to ufc3.exe, with absolute
///    paths into dlss5\ for the preset, shaders and add-ons (AddonPath). ReShade.log lands next to ufc3.exe too.
///    RESHADE_BASE_PATH_OVERRIDE would also work, but the game passes its environment on to the DLSS 5 host, whose
///    own ReShade (host64\dxgi.dll) must keep using host64\ReShade.ini;
///  - the helper add-on finds dlss5-feed.cfg and host64\ next to itself.
/// ReShade.ini, ReShadePreset.ini and dlss5-feed.cfg come from dlss5\templates only when missing (ReShade.ini gets
/// its absolute paths and fps counter set at every launch); after that they keep what the player saved.
/// Normal Play (D3D12) does not load ReShade, so the ini next to ufc3.exe does nothing there.
/// </summary>
public static class Dlss5
{
    public static string Dir => Path.Combine(Paths.Root, "dlss5");
    static string Templates => Path.Combine(Dir, "templates");
    static string Host64 => Path.Combine(Dir, "host64");

    /// <summary>Files setup copies in (from the DLSS 5 stack folder); dlss5\ is set up when all exist.</summary>
    static readonly string[] StackFiles =
    {
        "ReShade64.dll", "ReShade64.json", "dlss5-feed-helper.addon64",
        @"host64\dlss5-feed-host64.exe", @"host64\dxgi.dll", @"host64\winmm.dll", @"host64\OptiScaler.ini",
        @"host64\nvngx_dlss.dll", @"host64\nvngx_dlssnr.dll",
    };
    static readonly string[] StackDirs = { @"host64\OptiScaler", "shaders", "licenses" };
    static readonly string[] Needed = StackFiles.Concat(new[] { @"shaders\Shaders\DLSS5_Feed.fx", @"shaders\Shaders\lumenite_Kernel.fx", @"shaders\Shaders\ReShade.fxh" }).ToArray();
    /// <summary>Shipped with the launcher (package.ps1), not by setup.</summary>
    static readonly string[] Scripts =
    {
        @"templates\ReShade.template.ini", @"templates\ReShadePreset.ini", @"templates\dlss5-feed.template.cfg", "align-dlss5-panel.ps1",
    };

    /// <summary>Installs the DLSS 5 add-on zip (UFC3-DLSS5-Addon-*.zip from the release): its dlss5\ folder goes next to
    /// ufc3.exe. Nothing outside dlss5\ is written. The stack (nvngx_dlssnr.dll etc.) is still set up with "Set up DLSS 5".</summary>
    public static List<string> InstallAddon(string zipPath)
    {
        var log = new List<string>();
        using var zip = System.IO.Compression.ZipFile.OpenRead(zipPath);
        const string Anchor = "dlss5/align-dlss5-panel.ps1";
        var anchor = zip.Entries.FirstOrDefault(e => e.FullName.Replace('\\', '/').EndsWith(Anchor, StringComparison.OrdinalIgnoreCase))
            ?? throw new InvalidDataException("Not the DLSS 5 add-on zip (no " + Anchor + ").");
        var prefix = anchor.FullName.Replace('\\', '/')[..^Anchor.Length];
        foreach (var e in zip.Entries)
        {
            var full = e.FullName.Replace('\\', '/');
            if (full.EndsWith('/') || !full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
            var rel = full[prefix.Length..];
            if (rel.Split('/').Contains("..") || !rel.StartsWith("dlss5/", StringComparison.OrdinalIgnoreCase)) { log.Add("skipped " + rel); continue; }
            var dest = Path.Combine(Paths.Root, rel.Replace('/', Path.DirectorySeparatorChar));
            // the player's own look/feed/preset files are theirs once they exist
            if (File.Exists(dest) && (rel.EndsWith("OptiScaler.ini", StringComparison.OrdinalIgnoreCase) || rel.EndsWith("dlss5-feed.cfg", StringComparison.OrdinalIgnoreCase)
                || rel.EndsWith("ReShadePreset.ini", StringComparison.OrdinalIgnoreCase) && !rel.Contains("/templates/"))) { log.Add("kept yours: " + rel); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            e.ExtractToFile(dest, true);
            log.Add(rel);
        }
        log.Add($"DLSS 5 add-on installed ({log.Count} entries).");
        return log;
    }

    /// <summary>Downloads the add-on zip of the latest release (SHA-256 checked) and installs it.</summary>
    public static async Task<List<string>> DownloadAddon(IProgress<double>? progress, CancellationToken ct)
    {
        var rel = await Updater.FetchLatest(ct);
        var a = rel.Dlss5Addon ?? throw new InvalidDataException($"Release {rel.Tag} has no DLSS 5 add-on zip. Use the release page.");
        var zip = await Updater.Download(a, progress, ct);
        var log = InstallAddon(zip);
        Updater.CleanDownloads();
        log.Insert(0, $"{a.Name} from {rel.Tag}, SHA-256 ok");
        return log;
    }

    public static List<string> Missing() => Needed.Concat(Scripts).Where(f => !File.Exists(Path.Combine(Dir, f))).ToList();
    public static bool ScriptsPresent => Scripts.All(f => File.Exists(Path.Combine(Dir, f)));
    public static bool IsSetUp => Missing().Count == 0;

    /// <summary>Optional start folder for the setup folder dialog (UFC3_DLSS5_SOURCE); empty = the game folder.</summary>
    public static readonly string DefaultSource = Environment.GetEnvironmentVariable("UFC3_DLSS5_SOURCE") ?? "";

    public static bool GameRunning() => Process.GetProcessesByName("ufc3").Length > 0;

    public static Process Launch(Config c)
    {
        if (!Paths.IsInstalled) throw new InvalidOperationException("ufc3.exe is not next to the launcher.");
        if (!IsSetUp) throw new InvalidOperationException("DLSS 5 is not set up yet (DLSS 5 tab). Missing: " + string.Join(", ", Missing().Take(4)));
        if (GameRunning()) throw new InvalidOperationException("The game is already running.");

        WriteReShadeIni(c);
        // The preset and feed settings are the player's once they exist (ReShade saves its overlay changes into the
        // preset, the feeder its panel size/corner/key into dlss5-feed.cfg): only missing ones come from templates.
        // "Reset DLSS 5 settings" on the DLSS 5 tab copies the templates back.
        CopyIfMissing("ReShadePreset.ini", "ReShadePreset.ini");
        CopyIfMissing("dlss5-feed.template.cfg", "dlss5-feed.cfg");
        // F10 panel drawn by the game's ReShade from the host's own frame copy (cast_mode=1), not as a desktop
        // compositor thumbnail of the host window (0, borderless only).
        var feed = Ini.Load(Path.Combine(Dir, "dlss5-feed.cfg"));
        feed.Set("", "cast_mode", "1");
        // F10 only when the live panel is switched on; otherwise F24 (135, on no keyboard), so nothing opens it.
        feed.Set("", "cast_key", c.Dlss5LivePanel ? "121" : "135");
        // The host's own window is only needed for the live panel. With it (host_window=0) the host's D3D12 device hung
        // (DEVICE_HUNG during a present) after 1-15 min, idle or not, and DLSS 5 stayed off for the rest of the session;
        // without it (2) 25 + 36 min soaks ran clean. The DLSS 5 look is the same either way.
        feed.Set("", "host_window", c.Dlss5LivePanel ? "0" : "2");
        feed.Save();
        Dlss5Look.BackupDefault();

        // Live panel: keeps the host window over the in-game DLSS 5 panel so its clicks land; exits with the game.
        if (c.Dlss5LivePanel) StartAlign();

        var psi = new ProcessStartInfo(Paths.GameExe) { WorkingDirectory = Paths.Root, UseShellExecute = false };
        foreach (var a in new[] { "--gpu_backend=vulkan", $"--resolution_scale={Math.Clamp(c.Dlss5Scale, 1, 4)}", "--vulkan_shared_memory_hoist_uploads=true" })
            psi.ArgumentList.Add(a);
        psi.Environment["VK_ADD_IMPLICIT_LAYER_PATH"] = Path.Combine(Dir, "ReShade64.json");
        return Process.Start(psi) ?? throw new InvalidOperationException("ufc3.exe did not start");
    }

    static void StartAlign()
    {
        var align = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Dir };
        foreach (var a in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", Path.Combine(Dir, "align-dlss5-panel.ps1") })
            align.ArgumentList.Add(a);
        try { Process.Start(align); } catch { /* the panel still works with the keyboard */ }
    }

    /// <summary>ReShade.ini next to ufc3.exe: created from the template the first time; after that only the keys the
    /// launcher owns (paths into dlss5\, fps counter) are set, so what ReShade saved there itself stays.</summary>
    static void WriteReShadeIni(Config c)
    {
        var dest = Path.Combine(Paths.Root, "ReShade.ini");
        if (!File.Exists(dest))
        {
            var ini = File.ReadAllText(Path.Combine(Templates, "ReShade.template.ini"))
                .Replace("@DLSS5@", Dir).Replace("@SHOWFPS@", c.Dlss5ShowFps ? "1" : "0");
            File.WriteAllText(dest, ini, Encoding.ASCII);
            return;
        }
        var r = Ini.Load(dest);
        r.Set("GENERAL", "EffectSearchPaths", Dir + @"\shaders\Shaders\**");
        r.Set("GENERAL", "TextureSearchPaths", Dir + @"\shaders\Textures\**");
        r.Set("GENERAL", "PresetPath", Dir + @"\ReShadePreset.ini");
        r.Set("ADDON", "AddonPath", Dir);
        r.Set("OVERLAY", "ShowFPS", c.Dlss5ShowFps ? "1" : "0");
        r.Save();
    }

    static void CopyIfMissing(string template, string name)
    {
        var dest = Path.Combine(Dir, name);
        if (!File.Exists(dest)) File.Copy(Path.Combine(Templates, template), dest);
    }

    /// <summary>Back to the shipped DLSS 5 settings: ReShade preset, feeder settings and OptiScaler.ini as setup left it.</summary>
    public static void ResetSettings()
    {
        if (GameRunning()) throw new InvalidOperationException("Close the game first.");
        File.Copy(Path.Combine(Templates, "ReShadePreset.ini"), Path.Combine(Dir, "ReShadePreset.ini"), true);
        File.Copy(Path.Combine(Templates, "dlss5-feed.template.cfg"), Path.Combine(Dir, "dlss5-feed.cfg"), true);
        var rs = Path.Combine(Paths.Root, "ReShade.ini");
        if (File.Exists(rs)) File.Delete(rs);
        Dlss5Look.ResetToDefault();
    }

    /// <summary>Play from the launcher window: the window hides (no second window next to the game) and the launcher
    /// stays in the background until the game exits, to keep the F10 panel's settings, then quits.</summary>
    public static void PlayFrom(Control from, Config c)
    {
        var start = DateTime.Now.AddSeconds(-2);
        var game = Launch(c);
        var form = from.FindForm();
        form?.Hide();
        new Thread(() =>
        {
            WaitAndKeepSettings(game, start);
            if (form != null && form.IsHandleCreated) form.BeginInvoke(Application.Exit); else Application.Exit();
        }) { IsBackground = false, Name = "dlss5-wait" }.Start();
    }

    /// <summary>Waits for a DLSS 5 run to end, then keeps the model settings changed in the F10 panel
    /// (<see cref="Dlss5Look.KeepInGameChanges"/>). Logged to dlss5\launcher-dlss5.log.</summary>
    public static void WaitAndKeepSettings(Process game, DateTime start)
    {
        string result;
        try
        {
            game.WaitForExit();
            // The host (where OptiScaler runs and logs) exits with the game; give it a moment to close its log.
            foreach (var h in Process.GetProcessesByName("dlss5-feed-host64")) { try { h.WaitForExit(10000); } catch { } }
            result = Dlss5Look.KeepInGameChanges(start);
        }
        catch (Exception ex) { result = "ERROR: " + ex.Message; }
        try { File.AppendAllText(Path.Combine(Dir, "launcher-dlss5.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} run from {start:HH:mm:ss}: {result}{Environment.NewLine}"); }
        catch { }
    }

    /// <summary>The folder inside <paramref name="src"/> that holds ReShade64.dll + host64\ (the staged test's vk\,
    /// a Rumble Roses dlss5\ folder, or the folder itself), or null.</summary>
    public static string? FindStack(string src)
    {
        foreach (var d in new[] { src, Path.Combine(src, "vk"), Path.Combine(src, "dlss5") })
            if (File.Exists(Path.Combine(d, "ReShade64.dll")) && File.Exists(Path.Combine(d, @"host64\dlss5-feed-host64.exe"))) return d;
        return null;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateHardLink(string newFile, string existingFile, IntPtr sa);

    /// <summary>Binaries: hardlink (same drive, no extra space) or copy. Text files (OptiScaler.ini, shaders...) are
    /// copied, so settings saved in game do not change the source folder.</summary>
    static void Put(string from, string to)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
        if (File.Exists(to)) File.Delete(to);
        bool binary = Path.GetExtension(from).ToLowerInvariant() is ".dll" or ".exe" or ".addon64";
        if (!binary || !CreateHardLink(to, from, IntPtr.Zero)) File.Copy(from, to);
    }

    /// <summary>Fills dlss5\ from a folder holding the DLSS 5 stack. Logs, ReShade.ini and game files are not taken.</summary>
    public static List<string> Setup(string src)
    {
        var log = new List<string>();
        if (GameRunning()) throw new InvalidOperationException("Close the game first.");
        var stack = FindStack(src);
        if (stack == null) return SetupLoose(src);
        if (Path.GetFullPath(stack).TrimEnd('\\').Equals(Path.GetFullPath(Dir).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            return SetupLoose(stack);
        log.Add("Stack: " + stack);
        foreach (var f in StackFiles)
        {
            var s = Path.Combine(stack, f);
            if (!File.Exists(s)) throw new FileNotFoundException("missing in the stack folder: " + f);
            var to = Path.Combine(Dir, f);
            // OptiScaler.ini holds the player's DLSS 5 look: keep it; the stack's copy becomes the "Reset" default.
            if (f == @"host64\OptiScaler.ini" && File.Exists(to)) { Put(s, Dlss5Look.DefaultIniPath); log.Add("  " + f + " (yours kept; new default saved)"); continue; }
            Put(s, to);
            log.Add("  " + f);
        }
        foreach (var d in StackDirs)
        {
            // shaders\ sits next to the stack in the staged test (UFC-DLSS5\shaders) and inside it in a Rumble Roses dlss5\.
            var s = Path.Combine(stack, d);
            if (!Directory.Exists(s) && d == "shaders") s = Path.Combine(Path.GetDirectoryName(stack)!, d);
            if (!Directory.Exists(s)) { if (d == "licenses") continue; throw new DirectoryNotFoundException("missing in the stack folder: " + d); }
            int n = 0;
            foreach (var file in Directory.EnumerateFiles(s, "*", SearchOption.AllDirectories))
            {
                if (file.EndsWith(".log", StringComparison.OrdinalIgnoreCase)) continue;
                Put(file, Path.Combine(Dir, d, Path.GetRelativePath(s, file)));
                n++;
            }
            log.Add($"  {d}\\ ({n} files)");
        }
        var missing = Missing();
        log.Add(missing.Count == 0 ? "DLSS 5 is set up." : "Still missing: " + string.Join(", ", missing));
        return log;
    }

    /// <summary>Where each needed file comes from, for the "missing" message and the README.</summary>
    public static string Source(string f) => f switch
    {
        "ReShade64.dll" or "ReShade64.json" => "ReShade 6.8.0 with add-on support (reshade.me)",
        @"host64\dxgi.dll" => "a copy of ReShade64.dll, renamed",
        "dlss5-feed-helper.addon64" or @"host64\dlss5-feed-host64.exe" => "DLSS5-Feeder 1.19.0",
        @"host64\winmm.dll" => "OptiScaler-NR 0.8.92's OptiScaler.dll, renamed",
        @"host64\OptiScaler.ini" => "OptiScaler-NR 0.8.92",
        @"host64\nvngx_dlss.dll" => "NVIDIA DLSS 310.9.16",
        @"host64\nvngx_dlssnr.dll" => "the DLSS 5 neural rendering runtime 310.9.16 (RTX 20/30/40: the compatibility build)",
        @"shaders\Shaders\DLSS5_Feed.fx" => @"DLSS5-Feeder, reshade-shaders\Shaders",
        @"shaders\Shaders\lumenite_Kernel.fx" => "LumeniteFX (github.com/umar-afzaal/LumeniteFX)",
        @"shaders\Shaders\ReShade.fxh" => "crosire/reshade-shaders, slim branch",
        _ => "the UFC3 Recomp zip / DLSS 5 add-on zip",
    };

    /// <summary>
    /// Setup from a folder of loose downloads (what a player has, not our staged stack): every needed file is found
    /// by name anywhere under <paramref name="src"/> and put where the stack expects it -- ReShade64.dll also as
    /// host64\dxgi.dll, OptiScaler.dll (or winmm.dll) as host64\winmm.dll, the OptiScaler\ folder, NGX dlls and
    /// OptiScaler.ini into host64\, every Shaders\ and Textures\ folder (also reshade-shaders\Shaders) into shaders\.
    /// Works on the launcher's own dlss5\ folder too, for files dropped in the wrong place. A stock OptiScaler.ini
    /// gets the settings DLSS 5 needs (DLSS upscaler, neural pass on, working scale 0.5).
    /// </summary>
    public static List<string> SetupLoose(string src)
    {
        var log = new List<string> { "Loose files in " + src };
        var files = Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories)
            .Where(f => !f.EndsWith(".log", StringComparison.OrdinalIgnoreCase)
                && !f.StartsWith(Templates + "\\", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f.Count(c => c == '\\')).ToList();
        string? Find(params string[] names) => files.FirstOrDefault(f => names.Any(n => Path.GetFileName(f).Equals(n, StringComparison.OrdinalIgnoreCase)));
        bool Same(string a, string b) => Path.GetFullPath(a).TrimEnd('\\').Equals(Path.GetFullPath(b).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);
        void Place(string? from, string rel)
        {
            if (from == null) return;
            var to = Path.Combine(Dir, rel);
            if (Same(from, to)) { log.Add("  " + rel + " (already in place)"); return; }
            Put(from, to);
            log.Add("  " + rel + "  <- " + Path.GetRelativePath(src, from));
        }

        var reshade = Find("ReShade64.dll");
        Place(reshade, "ReShade64.dll");
        Place(reshade, @"host64\dxgi.dll");
        Place(Find("ReShade64.json"), "ReShade64.json");
        Place(Find("dlss5-feed-helper.addon64"), "dlss5-feed-helper.addon64");
        Place(Find("dlss5-feed-host64.exe"), @"host64\dlss5-feed-host64.exe");
        Place(Find("winmm.dll", "OptiScaler.dll"), @"host64\winmm.dll");
        Place(Find("nvngx_dlss.dll"), @"host64\nvngx_dlss.dll");
        Place(Find("nvngx_dlssnr.dll"), @"host64\nvngx_dlssnr.dll");

        var ini = Find("OptiScaler.ini");
        var iniTo = Path.Combine(Dir, @"host64\OptiScaler.ini");
        if (ini != null)
        {
            if (!Same(ini, iniTo) && File.Exists(iniTo)) { Put(ini, Dlss5Look.DefaultIniPath); log.Add(@"  host64\OptiScaler.ini (yours kept; new default saved)"); }
            else
            {
                Place(ini, @"host64\OptiScaler.ini");
                var o = Ini.Load(iniTo);
                if (o.Get("DlssNr", "WorkingScale") is null or "auto")
                {
                    o.Set("Upscalers", "Dx12Upscaler", "dlss");
                    o.Set("Menu", "Scale", "1.0");
                    foreach (var (k, v) in new[] { ("Enabled", "true"), ("RunBeforeSR", "true"), ("Passes", "1"), ("TransferStrength", "1.5"), ("WorkingScale", "0.5") })
                        o.Set("DlssNr", k, v);
                    o.Save();
                    log.Add("    set for DLSS 5: Dx12Upscaler=dlss, neural pass on, working scale 0.5");
                }
            }
        }

        // OptiScaler's own folder of upscaler dlls (from its zip), and the shader folders.
        foreach (var dir in Directory.EnumerateDirectories(src, "*", SearchOption.AllDirectories).ToList())
        {
            // Never read from where files are being put (dlss5\shaders is itself named like a source).
            if (new[] { "shaders", @"host64\OptiScaler" }.Any(d => Same(dir, Path.Combine(Dir, d))
                || Path.GetFullPath(dir).StartsWith(Path.GetFullPath(Path.Combine(Dir, d)) + "\\", StringComparison.OrdinalIgnoreCase))) continue;
            var name = Path.GetFileName(dir.TrimEnd('\\'));
            // OptiScaler's dll folder by name or by its contents (players rename it, e.g. "FOUND OptiScaler").
            bool opti = name.Equals("OptiScaler", StringComparison.OrdinalIgnoreCase)
                || File.Exists(Path.Combine(dir, "amd_fidelityfx_loader_dx12.dll")) || File.Exists(Path.Combine(dir, "libxess.dll"));
            string? rel = opti ? @"host64\OptiScaler"
                : name.Equals("Shaders", StringComparison.OrdinalIgnoreCase) ? @"shaders\Shaders"
                : name.Equals("Textures", StringComparison.OrdinalIgnoreCase) ? @"shaders\Textures" : null;
            if (rel == null) continue;
            var dest = Path.Combine(Dir, rel);
            if (Same(dir, dest)) continue;
            int n = 0;
            foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            {
                if (f.EndsWith(".log", StringComparison.OrdinalIgnoreCase)) continue;
                // Only the effects DLSS 5 uses: ReShade compiles every .fx it finds (LumeniteFX ships eight).
                if (rel == @"shaders\Shaders" && f.EndsWith(".fx", StringComparison.OrdinalIgnoreCase)
                    && !new[] { "DLSS5_Feed.fx", "lumenite_Kernel.fx" }.Contains(Path.GetFileName(f), StringComparer.OrdinalIgnoreCase)) continue;
                var to = Path.Combine(dest, Path.GetRelativePath(dir, f));
                if (Same(f, to)) continue;
                Put(f, to);
                n++;
            }
            if (n > 0) log.Add($"  {rel}\\ ({n} files)  <- {Path.GetRelativePath(src, dir)}");
        }
        // Shader includes that may sit loose (ReShade.fxh from reshade-shaders).
        foreach (var fxh in new[] { "DLSS5_Feed.fx", "ReShade.fxh", "ReShadeUI.fxh", "DrawText.fxh" })
            if (!File.Exists(Path.Combine(Dir, @"shaders\Shaders", fxh))) Place(Find(fxh), @"shaders\Shaders\" + fxh);

        var missing = Missing();
        log.Add(missing.Count == 0 ? "DLSS 5 is set up." : "Still missing:");
        foreach (var m in missing) log.Add($"  {m}  ({Source(m)})");
        return log;
    }

    public static int SetupCli(string src)
    {
        var sb = new StringBuilder($"{DateTime.Now:yyyy-MM-dd HH:mm:ss} --dlss5-setup {src}{Environment.NewLine}");
        int rc;
        try { foreach (var l in Setup(src)) sb.AppendLine(l); rc = IsSetUp ? 0 : 1; }
        catch (Exception ex) { sb.AppendLine("ERROR: " + ex.Message); rc = 1; }
        File.AppendAllText(Path.Combine(Paths.Root, "dlss5-cli-log.txt"), sb.ToString());
        return rc;
    }

    /// <summary>Brings the DLSS 5 settings window (the hidden host) to the front, visible.</summary>
    public static void ShowSettingsWindow()
    {
        var ps = Path.Combine(Dir, "show-dlss5-settings.ps1");
        if (!File.Exists(ps)) return;
        var psi = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true };
        foreach (var a in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps }) psi.ArgumentList.Add(a);
        Process.Start(psi);
    }
}
