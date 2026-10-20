using System.Diagnostics;

namespace UFC3Launcher.Tabs;

public class PlayTab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill };
    readonly Button play;
    readonly CheckBox dlss = new() { Text = "Start with DLSS 5", AutoSize = true };
    readonly Label dlssHint = Ui.Text("");
    readonly Label info = Ui.Text("");
    readonly Label header = Ui.Text("", true);
    readonly FlowLayoutPanel updRow = new() { AutoSize = true, WrapContents = false, Visible = false };
    readonly Label updText = Ui.Text("", true, 330);
    readonly Button updBtn;
    readonly ProgressBar updBar = new() { Width = 540, Height = 14, Maximum = 1000, Visible = false };
    readonly Label updInfo = Ui.Text("");
    readonly CheckBox checkUpd = new() { Text = "Check for updates when the launcher starts", AutoSize = true };
    Updater.Release? latest;
    bool loading, updating;

    /// <summary>Raised (UI thread) when the check finds a newer release.</summary>
    public event Action<Updater.Release>? UpdateFound;
    /// <summary>Raised after an update that did not need a restart.</summary>
    public event Action? Updated;

    public string Title => "Play";
    public Control View => root;

    public PlayTab()
    {
        var col = Ui.Column();
        header.Text = "UFC Undisputed 3 Recomp   v" + AppVersion.Installed + "  (launcher " + AppVersion.Launcher + ")";
        col.Controls.Add(header);
        play = new Button { Text = "Play", Width = 220, Height = 54, Font = new Font(SystemFonts.DefaultFont.FontFamily, 14, FontStyle.Bold), Margin = new Padding(0, 12, 0, 12) };
        play.Click += (_, _) => Launch();
        col.Controls.Add(play);
        dlss.CheckedChanged += (_, _) => { if (!loading) { var c = Config.Load(); c.PlayWithDlss5 = dlss.Checked; c.Save(); UpdatePlayText(); } };
        col.Controls.Add(dlss);
        col.Controls.Add(dlssHint);
        col.Controls.Add(info);
        checkUpd.CheckedChanged += (_, _) => { if (!loading) { var c = Config.Load(); c.CheckForUpdates = checkUpd.Checked; c.Save(); } };
        updBtn = Ui.AutoBtn("Update", async (_, _) => await RunUpdate());
        updRow.Controls.AddRange(new Control[] { updText, updBtn, Ui.AutoBtn("What's new", (_, _) => OpenUrl(latest?.Page ?? Updater.ReleasesPage)) });
        col.Controls.Add(updRow);
        col.Controls.Add(updBar);
        col.Controls.Add(updInfo);
        col.Controls.Add(checkUpd);
        col.Controls.Add(Ui.AutoBtn("Check for updates now", async (_, _) => await CheckForUpdates(true)));
        col.Controls.Add(Ui.Text("Normal Play uses your settings in ufc3.toml (F4 in game). DLSS 5 runs use the same exe, settings and saves, " +
            "with Vulkan and the DLSS 5 resolution scale for that run only."));
        root.Controls.Add(col);
    }

    public void OnShown()
    {
        loading = true;
        var cfg = Config.Load();
        dlss.Checked = cfg.PlayWithDlss5;
        checkUpd.Checked = cfg.CheckForUpdates;
        loading = false;
        dlss.Enabled = Dlss5.IsSetUp;
        dlssHint.Text = Dlss5.IsSetUp ? "4K at 60 fps with NVIDIA DLSS 5. Its look is set in the DLSS 5 tab. In game: F9 = DLSS 5 on/off."
            : "DLSS 5 is not set up yet: see the DLSS 5 tab.";
        play.Enabled = Paths.IsInstalled && Paths.AssetsReady && !updating;
        info.Text = !Paths.IsInstalled ? "ufc3.exe is not next to this launcher. Put the launcher in the play folder."
            : !Paths.AssetsReady ? "The game files are not set up yet. Open the Setup tab and choose your UFC Undisputed 3 .iso."
            : "";
        UpdatePlayText();
    }

    void UpdatePlayText() => play.Text = UseDlss5 ? "Play (DLSS 5)" : "Play";

    static bool UseDlss5 => Config.Load().PlayWithDlss5 && Dlss5.IsSetUp;

    /// <summary>Asks GitHub for the latest release; shows the update row when it is newer. Quiet on errors unless asked.</summary>
    public async Task CheckForUpdates(bool manual)
    {
        if (updating) return;
        if (manual) updInfo.Text = "Checking...";
        try
        {
            latest = await Updater.FetchLatest();
            if (latest.IsNewer)
            {
                updText.Text = $"Update available: v{latest.Version} (you have v{AppVersion.Installed})";
                updRow.Visible = true;
                updInfo.Text = latest.Game == null ? "This release has no game zip; use \"What's new\"." : "";
                updBtn.Enabled = latest.Game != null;
                UpdateFound?.Invoke(latest);
            }
            else if (manual) updInfo.Text = $"Up to date (v{AppVersion.Installed}; latest release v{latest.Version}).";
        }
        catch (Exception ex) { if (manual) updInfo.Text = "Could not check for updates: " + ex.Message; }
    }

    async Task RunUpdate()
    {
        var rel = latest;
        if (rel?.Game == null) return;
        if (Updater.GameRunning()) { updInfo.Text = "Close the game first."; return; }
        if (MessageBox.Show($"Download v{rel.Version} ({rel.Game.Size / 1048576.0:0} MB) and replace the changed program files?\n\n" +
                "Your settings (ufc3.toml), saves, mods and game files are not touched. Replaced files are kept in update-backups.",
                "Update", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        updating = true; updBtn.Enabled = play.Enabled = false; updBar.Visible = true; updBar.Value = 0;
        try
        {
            updInfo.Text = $"Downloading {rel.Game.Name}...";
            var zip = await Updater.Download(rel.Game, new Progress<double>(f => updBar.Value = (int)Math.Clamp(f * 1000, 0, 1000)), CancellationToken.None);
            updInfo.Text = "SHA-256 ok. Replacing files...";
            var res = await Task.Run(() => Updater.Apply(zip, rel.Tag));
            Updater.CleanDownloads();
            updRow.Visible = false;
            if (res.RestartExe != null)
            {
                Process.Start(new ProcessStartInfo(res.RestartExe, "--updated") { WorkingDirectory = Paths.Root, UseShellExecute = false });
                Application.Exit();
                return;
            }
            updInfo.Text = res.Log.LastOrDefault() ?? $"Updated to v{rel.Version}.";
            header.Text = "UFC Undisputed 3 Recomp   v" + AppVersion.Installed + "  (launcher " + AppVersion.Launcher + ")";
            Updated?.Invoke();
        }
        catch (Exception ex) { updInfo.Text = "Update failed: " + ex.Message; updBtn.Enabled = true; }
        finally { updating = false; updBar.Visible = false; play.Enabled = Paths.IsInstalled && Paths.AssetsReady; }
    }

    static void OpenUrl(string url) => Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });

    void Launch()
    {
        try
        {
            if (Dlss5.GameRunning()) { info.Text = "The game is already running."; return; }
            if (UseDlss5) { Dlss5.PlayFrom(root, Config.Load()); return; }
            Process.Start(new ProcessStartInfo(Paths.GameExe) { WorkingDirectory = Paths.Root, UseShellExecute = false });
            Application.Exit();
        }
        catch (Exception ex) { info.Text = "Could not start the game: " + ex.Message; }
    }
}
