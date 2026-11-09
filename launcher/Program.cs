using UFC3Launcher.Tabs;

namespace UFC3Launcher;

static class Program
{
    /// <summary>
    ///   (no args)              launcher window
    ///   --modpack-install &lt;zip&gt; / --modpack-uninstall x   headless pack install (tests); log in modpack-cli-log.txt
    ///   --update [--check-only] [--force] [--zip f --sha h --tag t]   headless update; log in update-backups\cli-log.txt
    ///   --setup-iso &lt;iso&gt; [--dest dir]   headless disc extract (default dest: assets\); log in setup-cli-log.txt
    ///   --setup-dlc &lt;zip|pkg|folder&gt; [--userdata dir]   headless DLC install
    ///   --updated              (internal) started by the launcher after it replaced itself
    ///   --dlss5                start the game with DLSS 5 right away (tests)
    ///   --shader-cache [--dest dir]   headless shader cache build, the step after --setup-iso (tests); log in setup-cli-log.txt
    ///   --native               start the game as Play does with Renderer = Native: builds a missing shader cache, starts on
    ///                          emulated if that fails or after exit code 3 (tests)
    ///   --screenshot-all &lt;dir&gt; [percent]   write a PNG of every tab at that display scale, never starts the game (tests)
    ///   --dlss5-setup &lt;dir&gt;  fill dlss5\ from a folder holding the DLSS 5 stack; log in dlss5-cli-log.txt
    /// </summary>
    [STAThread]
    static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        int si = Array.IndexOf(args, "--screenshot-all");
        int? simPct = si >= 0 && si + 2 < args.Length && int.TryParse(args[si + 2], out var sp) ? sp : null;
        Theme.Init(simPct);
        if (si >= 0 && si + 1 < args.Length) { Environment.Exit(Shots.Run(args[si + 1], simPct ?? 100)); return; }
        if (args.Contains("--setup-iso") || args.Contains("--setup-dlc") || args.Contains("--shader-cache")) { Environment.Exit(SetupCli(args)); return; }
        if (args.Length == 2 && args[0] == "--dlss5-setup") { Environment.Exit(Dlss5.SetupCli(args[1])); return; }
        if (args.Length == 2 && (args[0] == "--modpack-install" || args[0] == "--modpack-uninstall"))
        { Environment.Exit(ModPackCli(args[0], args[1])); return; }
        if (args.Length > 0 && args[0] == "--update") { Environment.Exit(Updater.Cli(args)); return; }
        if (args.Contains("--dlss5"))
        {
            try { var start = DateTime.Now.AddSeconds(-2); Dlss5.WaitAndKeepSettings(Dlss5.Launch(Config.Load()), start); }
            catch (Exception ex) { DarkBox.Show("Could not start with DLSS 5: " + ex.Message, "UFC3 Launcher"); }
            return;
        }
        if (args.Contains("--native"))
        {
            try
            {
                if (NativeRenderer.PrepareCache() != null) NativeRenderer.LaunchEmulated(); // reason is in launcher-native.log
                else NativeRenderer.WaitAndFallBack(NativeRenderer.Launch());
            }
            catch (Exception ex) { DarkBox.Show("Could not start the game: " + ex.Message, "UFC3 Launcher"); }
            return;
        }
        Application.Run(new MainForm(args.Contains("--updated")));
    }

    static string? Arg(string[] a, string name) { int i = Array.IndexOf(a, name); return i >= 0 && i + 1 < a.Length ? a[i + 1] : null; }

    static int SetupCli(string[] args)
    {
        var log = new List<string> { $"{DateTime.Now:s} {string.Join(' ', args)}" };
        int code = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            var none = new Progress<(string, double)>(_ => { });
            if (Arg(args, "--setup-iso") is { } iso)
            {
                var dest = Arg(args, "--dest") ?? Paths.Assets;
                Setup.ImportIso(iso, dest, none, CancellationToken.None);
                log.Add($"iso extracted to {dest} in {sw.Elapsed.TotalSeconds:0}s");
                log.Add(Setup.FinishImport(dest, none, CancellationToken.None));
            }
            else if (args.Contains("--shader-cache"))
                log.Add(Setup.FinishImport(Arg(args, "--dest") ?? Paths.Assets, none, CancellationToken.None));
            if (Arg(args, "--setup-dlc") is { } dlc)
            {
                var ud = Arg(args, "--userdata") ?? Paths.UserData;
                log.AddRange(Setup.ImportDlc(dlc, ud, none, CancellationToken.None));
            }
            log.Add("OK");
        }
        catch (Exception ex) { log.Add("FAILED: " + ex.Message); code = 1; }
        File.AppendAllLines(Path.Combine(Paths.Root, "setup-cli-log.txt"), log);
        return code;
    }

    static int ModPackCli(string cmd, string arg)
    {
        string msg; int code = 0;
        try
        {
            msg = cmd == "--modpack-install" ? ModPack.Install(arg, (_, _) => { }, CancellationToken.None) : ModPack.Uninstall((_, _) => { });
        }
        catch (Exception ex) { msg = "FAILED: " + ex.Message; code = 1; }
        File.AppendAllText(Path.Combine(Paths.Root, "modpack-cli-log.txt"), $"{DateTime.Now:s} {cmd} {msg}{Environment.NewLine}");
        return code;
    }
}

class MainForm : Form
{
    readonly List<ILauncherTab> tabs = new();
    readonly NavHost tc = new();
    readonly bool preview;

    /// <summary>--screenshot-all: never take the focus from the user.</summary>
    protected override bool ShowWithoutActivation => preview;

    internal NavHost Nav => tc;
    internal IReadOnlyList<ILauncherTab> Tabs => tabs;

    /// <param name="preview">--screenshot-all: no update check, no messages.</param>
    public MainForm(bool updated = false, bool preview = false)
    {
        this.preview = preview;
        Text = "UFC Undisputed 3 Recomp - Launcher " + AppVersion.Launcher;
        AutoScaleMode = AutoScaleMode.None;   // sizes are scaled by Theme.S
        Theme.DarkWindow(this);
        ClientSize = new Size(Theme.S(940), Theme.S(680));
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = SizeFromClientSize(new Size(Theme.S(780), Theme.S(540)));

        var play = new PlayTab();
        var setup = new SetupTab();
        tabs.AddRange(new ILauncherTab[] { play, setup, new SettingsTab(), new ModPackTab(), new Dlss5Tab() });
        const string Title = "UFC Undisputed 3 Recomp - Launcher " + AppVersion.Launcher;
        play.UpdateFound += r => Text = $"{Title}  (update available: v{r.Version}, see Play)";
        play.Updated += () => Text = Title;
        foreach (var t in tabs) tc.Add(t.Title, Glyph(t.Title), t.View, t is not PlayTab);
        tc.SelectedIndexChanged += (_, _) => tabs[tc.SelectedIndex].OnShown();
        Controls.Add(tc);
        Theme.DarkScroll(this);
        Shown += (_, _) =>
        {
            if (preview) return;
            tc.SelectedIndex = Paths.AssetsReady ? 0 : tabs.IndexOf(setup);   // raises OnShown
            if (updated) DarkBox.Show($"Updated to v{AppVersion.Installed}. The replaced files are in the update-backups folder.", "UFC3 Launcher");
            else if (Paths.IsInstalled && Config.Load().CheckForUpdates) _ = play.CheckForUpdates(false);
        };
    }

    /// <summary>Sidebar icons (Segoe Fluent Icons / MDL2 Assets, both ship with Windows).</summary>
    static string Glyph(string title) => title switch
    {
        "Play" => "",
        "Setup" => "",
        "Settings" => "",
        "Mod Pack" => "",
        "DLSS 5" => "",
        _ => "",
    };

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (keyData == (Keys.Control | Keys.Tab)) { tc.Next(1); return true; }
        if (keyData == (Keys.Control | Keys.Shift | Keys.Tab)) { tc.Next(-1); return true; }
        return base.ProcessCmdKey(ref msg, keyData);
    }
}
