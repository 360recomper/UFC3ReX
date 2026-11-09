namespace UFC3Launcher.Tabs;

/// <summary>First-run setup: the user's own disc image -> assets\, optional DLC packages -> userdata\.</summary>
public class SetupTab : ILauncherTab
{
    readonly FlowLayoutPanel col = Ui.Column();
    readonly Label state = Ui.Text("", true);
    readonly Label status = Ui.Text("");
    readonly ProgressBar bar = new AccentBar { Width = Theme.S(540), Height = Theme.S(18), Maximum = 1000 };
    readonly Label current = Ui.Text("");
    readonly Button isoBtn, dlcFolderBtn, dlcFileBtn, cancelBtn;
    CancellationTokenSource? cts;

    public string Title => "Setup";
    public Control View => col;

    public SetupTab()
    {
        col.Controls.Add(Ui.Head("Game files"));
        col.Controls.Add(state);
        col.Controls.Add(Ui.Text("Choose your own UFC Undisputed 3 (USA) disc image (.iso). The launcher checks that it is the right game, " +
            "then copies the game files (about 6.5 GB) into the assets folder next to this launcher. This takes a few minutes. " +
            "If you cancel, run it again to continue where it stopped."));
        isoBtn = Ui.Primary(Ui.Btn("Choose game ISO...", async (_, _) => await RunIso(), 170));
        col.Controls.Add(isoBtn);

        col.Controls.Add(Ui.Head("DLC (optional)"));
        col.Controls.Add(Ui.Text("Choose a folder or a zip of your Xbox 360 DLC packages. They are unpacked into the userdata folder, " +
            "and the game then lists them as purchased."));
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        dlcFolderBtn = Ui.Btn("Choose DLC folder...", async (_, _) => await RunDlcFolder(), 170);
        dlcFileBtn = Ui.Btn("Choose DLC zip/package...", async (_, _) => await RunDlcFile(), 190);
        row.Controls.AddRange(new Control[] { dlcFolderBtn, dlcFileBtn });
        col.Controls.Add(row);

        cancelBtn = Ui.Btn("Cancel", (_, _) => cts?.Cancel(), 90);
        cancelBtn.Enabled = false;
        col.Controls.Add(bar);
        col.Controls.Add(current);
        col.Controls.Add(cancelBtn);
        col.Controls.Add(status);
    }

    /// <summary>--screenshot-all: the page as during an ISO import.</summary>
    internal void PreviewBusy()
    {
        isoBtn.Enabled = dlcFolderBtn.Enabled = dlcFileBtn.Enabled = false; cancelBtn.Enabled = true;
        bar.Value = 420; current.Text = @"42%  pac\_startup_usa.pac";
        status.Text = "Copying game files...";
    }

    public void OnShown() =>
        state.Text = Paths.AssetsReady ? "Installed: the game files are in " + Paths.Assets
            : "Not set up yet. Choose your ISO to begin; Play stays off until this is done.";

    async Task Run(Func<IProgress<(string, double)>, CancellationToken, string> work)
    {
        cts = new CancellationTokenSource();
        isoBtn.Enabled = dlcFolderBtn.Enabled = dlcFileBtn.Enabled = false; cancelBtn.Enabled = true;
        status.Text = "";
        var prog = new Progress<(string name, double frac)>(p =>
        {
            bar.Value = (int)Math.Clamp(p.frac * 1000, 0, 1000);
            // frac < 0: a step with no percentage to show.
            current.Text = p.name.Length == 0 ? "" : p.frac < 0 ? p.name : $"{p.frac * 100:0}%  {p.name}";
        });
        try { status.Text = await Task.Run(() => work(prog, cts.Token)); }
        catch (OperationCanceledException) { status.Text = "Cancelled. Choose the same file again to continue where it stopped."; }
        catch (Exception ex) { status.Text = "Error: " + ex.Message; }
        finally
        {
            isoBtn.Enabled = dlcFolderBtn.Enabled = dlcFileBtn.Enabled = true; cancelBtn.Enabled = false;
            bar.Value = 0; current.Text = "";
            OnShown();
        }
    }

    Task RunIso()
    {
        using var dlg = new OpenFileDialog { Title = "UFC Undisputed 3 disc image", Filter = "Disc image (*.iso)|*.iso|All files|*.*" };
        if (dlg.ShowDialog() != DialogResult.OK) return Task.CompletedTask;
        var iso = dlg.FileName;
        return Run((p, ct) =>
        {
            Setup.ImportIso(iso, Paths.Assets, p, ct);
            return Setup.FinishImport(Paths.Assets, p, ct);
        });
    }

    Task RunDlcFolder()
    {
        using var dlg = new FolderBrowserDialog { Description = "Folder with your Xbox 360 DLC packages (or zips of them)" };
        return dlg.ShowDialog() == DialogResult.OK ? RunDlc(dlg.SelectedPath) : Task.CompletedTask;
    }

    Task RunDlcFile()
    {
        using var dlg = new OpenFileDialog { Title = "DLC zip or package", Filter = "Zip or package|*.zip;*|All files|*.*" };
        return dlg.ShowDialog() == DialogResult.OK ? RunDlc(dlg.FileName) : Task.CompletedTask;
    }

    Task RunDlc(string src) =>
        Run((p, ct) => string.Join("\n", Setup.ImportDlc(src, Paths.UserData, p, ct)));
}
