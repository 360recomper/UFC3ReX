namespace UFC3Launcher.Tabs;

public class Dlss5Tab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill };
    readonly Label status = Ui.Text("", true);
    readonly Button setupBtn, playBtn, folderBtn;
    readonly NumericUpDown scale = new() { Minimum = 1, Maximum = 4, Width = 50, Margin = new Padding(0, 6, 8, 4) };
    readonly CheckBox fps = new() { Text = "Show ReShade's fps counter", AutoSize = true };
    readonly CheckBox livePanel = new() { Text = "Advanced: live DLSS 5 panel on F10 in game (opens OptiScaler's own window over the game while it is open)", AutoSize = true, MaximumSize = new Size(560, 0) };
    readonly ComboBox style = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 130, Margin = new Padding(0, 4, 8, 4) };
    readonly ComboBox preset = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 130, Margin = new Padding(0, 4, 8, 4) };
    readonly List<(Dlss5Look.Setting s, NumericUpDown n)> sliders = new();
    readonly Label lookState = Ui.Text("");
    readonly FlowLayoutPanel lookBox = new() { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = new Padding(0) };
    readonly TextBox output = new()
    {
        Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = true, Width = 560, Height = 130,
        Font = new Font(FontFamily.GenericMonospace, 8.5f), BackColor = SystemColors.Window, Visible = false,
    };
    readonly FlowLayoutPanel addonRow = new() { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
    readonly Button dlBtn, zipBtn;
    bool loading;

    public string Title => "DLSS 5";
    public Control View => root;

    public Dlss5Tab()
    {
        var col = Ui.Column();
        col.Controls.Add(Ui.Text("Optional: NVIDIA DLSS 5 (neural rendering), 4K at 60 fps. Needs an NVIDIA GeForce RTX card " +
            "(RTX 40/50 recommended). Starts the same ufc3.exe with Vulkan and ReShade loaded for this game only; normal Play is not changed."));
        col.Controls.Add(status);

        setupBtn = Ui.AutoBtn("Set up DLSS 5...", (_, _) => Setup());
        playBtn = Ui.AutoBtn("Play with DLSS 5", (_, _) => Play());
        folderBtn = Ui.AutoBtn("Open dlss5 folder", (_, _) => Ui.OpenFolder(Dlss5.Dir));
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        row.Controls.AddRange(new Control[] { setupBtn, playBtn, folderBtn });
        col.Controls.Add(row);
        dlBtn = Ui.AutoBtn("Download add-on", async (_, _) => await DownloadAddon());
        zipBtn = Ui.AutoBtn("Install add-on from zip...", (_, _) => InstallZip());
        addonRow.Controls.AddRange(new Control[] { dlBtn, zipBtn, Ui.AutoBtn("Release page", (_, _) => Open(Updater.ReleasesPage)) });
        col.Controls.Add(addonRow);

        col.Controls.Add(Ui.Text("Settings", true));
        var scaleRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        scaleRow.Controls.Add(Ui.Text("Resolution scale for DLSS 5 runs:", false, 220));
        scaleRow.Controls.Add(scale);
        scaleRow.Controls.Add(Ui.Text("(2 = 2560x1440 rendered, 4K out; holds 60 fps)", false, 300));
        col.Controls.Add(scaleRow);
        col.Controls.Add(fps);
        scale.ValueChanged += (_, _) => { if (!loading) { var c = Config.Load(); c.Dlss5Scale = (int)scale.Value; c.Save(); } };
        fps.CheckedChanged += (_, _) => { if (!loading) { var c = Config.Load(); c.Dlss5ShowFps = fps.Checked; c.Save(); } };
        livePanel.CheckedChanged += (_, _) => { if (!loading) { var c = Config.Load(); c.Dlss5LivePanel = livePanel.Checked; c.Save(); } };

        // The look: OptiScaler.ini [DlssNr], written on every change (that file is where it is saved).
        col.Controls.Add(Ui.Text("DLSS 5 look", true));
        col.Controls.Add(Ui.Text("Set it here: saved as soon as you change it, used every time the game starts with DLSS 5. " +
            "Nothing to save in game."));
        style.Items.AddRange(Dlss5Look.Styles);
        preset.Items.AddRange(Dlss5Look.Presets);
        lookBox.Controls.Add(Row("Style:", style, "Standard is the safe pick; Cinematic is stronger."));
        lookBox.Controls.Add(Row("Model preset:", preset, ""));
        style.SelectedIndexChanged += (_, _) => { if (!loading) Save(() => Dlss5Look.SetStyle(style.SelectedIndex)); };
        preset.SelectedIndexChanged += (_, _) => { if (!loading) Save(() => Dlss5Look.SetPreset(preset.SelectedIndex)); };
        foreach (var st in Dlss5Look.Sliders)
        {
            var n = new NumericUpDown { Minimum = st.Min, Maximum = st.Max, Increment = st.Step, DecimalPlaces = st.Step >= 1 ? 0 : 2, Width = 70, Margin = new Padding(0, 4, 8, 4) };
            var cur = st;
            n.ValueChanged += (_, _) => { if (!loading) Save(() => Dlss5Look.Set(cur, n.Value)); };
            sliders.Add((st, n));
            lookBox.Controls.Add(Row(st.Label + ":", n, st.Help));
        }
        col.Controls.Add(lookBox);
        var lookRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        lookRow.Controls.Add(Ui.AutoBtn("Reset DLSS 5 settings", (_, _) => Reset()));
        lookRow.Controls.Add(lookState);
        col.Controls.Add(lookRow);
        col.Controls.Add(livePanel);

        col.Controls.Add(output);
        col.Controls.Add(Ui.Text("In game: F9 = DLSS 5 on/off (F10 = live panel when switched on above). NVIDIA Smooth Motion must be off. " +
            "To start with DLSS 5 from the Play tab, tick \"Start with DLSS 5\" there. Undo: delete the dlss5 folder."));
        root.Controls.Add(col);
    }

    public void OnShown()
    {
        loading = true;
        var c = Config.Load();
        scale.Value = Math.Clamp(c.Dlss5Scale, 1, 4);
        fps.Checked = c.Dlss5ShowFps;
        livePanel.Checked = c.Dlss5LivePanel;
        LoadLook();
        loading = false;
        Refresh();
    }

    static FlowLayoutPanel Row(string label, Control input, string help)
    {
        var r = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        r.Controls.Add(Ui.Text(label, false, 130));
        r.Controls[0].MinimumSize = new Size(130, 0);
        r.Controls.Add(input);
        if (help != "") r.Controls.Add(Ui.Text(help, false, 330));
        return r;
    }

    void LoadLook()
    {
        bool ok = Dlss5Look.Present;
        lookBox.Enabled = ok;
        if (!ok) { lookState.Text = "Set up DLSS 5 first."; return; }
        style.SelectedIndex = Dlss5Look.Style;
        preset.SelectedIndex = Dlss5Look.Preset;
        foreach (var (st, n) in sliders) n.Value = Dlss5Look.Get(st);
        lookState.Text = "";
    }

    void Save(Action write)
    {
        try { write(); lookState.Text = Dlss5.GameRunning() ? "Saved (used the next time the game starts)." : "Saved."; }
        catch (Exception ex) { lookState.Text = "Could not save: " + ex.Message; }
    }

    void Reset()
    {
        if (MessageBox.Show("Put the DLSS 5 look, ReShade preset and F10 panel settings back to how they were after setup?",
                "UFC3 Launcher", MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        try { Dlss5.ResetSettings(); loading = true; LoadLook(); loading = false; lookState.Text = "Reset to the setup defaults."; }
        catch (Exception ex) { lookState.Text = "Could not reset: " + ex.Message; }
    }

    void Refresh()
    {
        var missing = Dlss5.Missing();
        status.Text = !Paths.IsInstalled ? "ufc3.exe is not next to this launcher."
            : !Dlss5.ScriptsPresent ? "DLSS 5 add-on: not installed. Download it here, or install the add-on zip from the release page."
            : missing.Count > 0 ? $"DLSS 5: not set up ({missing.Count} files missing). Press \"Set up DLSS 5...\" and pick the folder with the DLSS 5 files."
            : "DLSS 5: set up and ready.";
        addonRow.Visible = !Dlss5.ScriptsPresent;
        addonRow.Visible = !Dlss5.ScriptsPresent;
        setupBtn.Text = missing.Count == 0 ? "Set up again..." : "Set up DLSS 5...";
        setupBtn.Enabled = Paths.IsInstalled && Dlss5.ScriptsPresent;
        playBtn.Enabled = Paths.IsInstalled && missing.Count == 0;
    }

    void Append(string s)
    {
        output.Visible = true;
        output.AppendText(s + Environment.NewLine);
    }

    async Task DownloadAddon()
    {
        output.Clear(); dlBtn.Enabled = zipBtn.Enabled = false;
        Append("Downloading the DLSS 5 add-on from the latest release...");
        try { foreach (var l in await Dlss5.DownloadAddon(null, CancellationToken.None)) Append(l); }
        catch (Exception ex) { Append("Error: " + ex.Message); }
        dlBtn.Enabled = zipBtn.Enabled = true;
        Refresh();
    }

    void InstallZip()
    {
        using var dlg = new OpenFileDialog { Title = "DLSS 5 add-on zip (UFC3-DLSS5-Addon-....zip)", Filter = "DLSS 5 add-on|UFC3-DLSS5-Addon-*.zip|Zip files|*.zip" };
        if (dlg.ShowDialog() != DialogResult.OK) return;
        output.Clear();
        try { foreach (var l in Dlss5.InstallAddon(dlg.FileName)) Append(l); }
        catch (Exception ex) { Append("Error: " + ex.Message); }
        Refresh();
    }

    static void Open(string url) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });

    void Setup()
    {
        using var dlg = new FolderBrowserDialog
        {
            Description = "Folder with the DLSS 5 files (ReShade64.dll, dlss5-feed-helper.addon64, host64\\ with nvngx_dlssnr.dll, shaders\\): " +
                "the staged UFC-DLSS5 test, or a Rumble Roses dlss5 folder",
            UseDescriptionForTitle = true,
            InitialDirectory = Directory.Exists(Dlss5.DefaultSource) ? Dlss5.DefaultSource : Paths.Root,
        };
        if (dlg.ShowDialog() != DialogResult.OK) return;
        output.Clear();
        try { foreach (var l in Dlss5.Setup(dlg.SelectedPath)) Append(l); }
        catch (Exception ex) { Append("Error: " + ex.Message); }
        Dlss5Look.BackupDefault();
        loading = true; LoadLook(); loading = false;
        Refresh();
    }

    void Play()
    {
        try { Dlss5.PlayFrom(root, Config.Load()); }
        catch (Exception ex) { Append("Could not start: " + ex.Message); }
    }
}
