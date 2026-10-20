namespace UFC3Launcher.Tabs;

public class ModPackTab : ILauncherTab
{
    readonly FlowLayoutPanel col = Ui.Column();
    readonly Label installed = Ui.Text("");
    readonly Label picked = Ui.Text("");
    readonly Label status = Ui.Text("");
    readonly Button install, uninstall, choose;
    readonly ProgressBar bar = new() { Width = 540, Height = 16, Visible = false };
    string? zip;
    bool busy;

    public string Title => "Mod Pack";
    public Control View => col;

    public ModPackTab()
    {
        col.Controls.Add(Ui.Text("Mod pack", true));
        col.Controls.Add(Ui.Text("Download the mod pack (.zip) from the link you were given, then choose it here or drag the zip onto this window. " +
            "Don't unzip it. Installing adds its files to the game's mods folder; Uninstall removes exactly those files again."));
        col.Controls.Add(Ui.Text("Installed", true));
        col.Controls.Add(installed);
        uninstall = Ui.Btn("Uninstall", (_, _) => Uninstall(), 120);
        col.Controls.Add(uninstall);

        col.Controls.Add(Ui.Text("Install a pack", true));
        choose = Ui.Btn("Choose mod pack zip...", (_, _) => Choose(), 200);
        col.Controls.Add(choose);
        col.Controls.Add(picked);
        install = Ui.Btn("Install", (_, _) => Install(), 120);
        col.Controls.Add(install);
        col.Controls.Add(bar);
        col.Controls.Add(status);
        col.Controls.Add(Ui.Btn("Open mods folder", (_, _) => Ui.OpenFolder(Paths.Mods), 150));

        col.AllowDrop = true;
        col.DragEnter += (_, e) => { if (e.Data?.GetDataPresent(DataFormats.FileDrop) == true) e.Effect = DragDropEffects.Copy; };
        col.DragDrop += (_, e) =>
        {
            if (e.Data?.GetData(DataFormats.FileDrop) is string[] f && f.Length > 0) Pick(f[0]);
        };
    }

    public void OnShown()
    {
        var r = ModPack.Installed();
        installed.Text = r == null ? "No mod pack installed." : $"{r.Name}  {r.Version}  ({r.Files.Count} files, installed {r.Installed})";
        uninstall.Enabled = r != null && !busy;
        install.Enabled = zip != null && !busy;
        install.Text = r != null && zip != null ? "Reinstall / update" : "Install";
        choose.Enabled = !busy;
    }

    void Choose()
    {
        using var d = new OpenFileDialog { Title = "Choose the mod pack", Filter = "Mod pack (*.zip)|*.zip|All files|*.*" };
        if (d.ShowDialog() == DialogResult.OK) Pick(d.FileName);
    }

    void Pick(string path)
    {
        zip = null; status.Text = "";
        try
        {
            var (info, files, warns) = ModPack.Inspect(path);
            zip = path;
            picked.Text = $"{info.Name}  {info.Version}  ({files} files)\n{info.Description}";
            if (warns.Count > 0) status.Text = string.Join("\n", warns);
        }
        catch (Exception ex) { picked.Text = ""; status.Text = ex.Message; }
        OnShown();
    }

    bool GameOpen()
    {
        if (!Dlss5.GameRunning()) return false;
        status.Text = "The game is running: close it first.";
        return true;
    }

    async void Install()
    {
        if (zip == null || busy || GameOpen()) return;
        var prev = ModPack.Installed();
        if (prev != null && MessageBox.Show($"\"{prev.Name}\" is installed. It is removed first and replaced by this pack. Continue?",
                "UFC3 Launcher", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
        await Run(() => ModPack.Install(zip, Progress, CancellationToken.None));
    }

    async void Uninstall()
    {
        if (busy || GameOpen()) return;
        await Run(() => ModPack.Uninstall(Progress));
    }

    void Progress(string text, double frac)
    {
        if (col.IsHandleCreated) col.BeginInvoke(() => { bar.Value = (int)Math.Clamp(frac * 100, 0, 100); status.Text = text; });
    }

    async Task Run(Func<string> work)
    {
        busy = true; bar.Value = 0; bar.Visible = true; OnShown();
        try { var msg = await Task.Run(work); status.Text = msg; }
        catch (Exception ex) { status.Text = "Failed: " + ex.Message + "\nUninstall removes whatever was already written."; }
        busy = false; bar.Visible = false; OnShown();
    }
}
