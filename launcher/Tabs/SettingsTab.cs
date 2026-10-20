namespace UFC3Launcher.Tabs;

/// <summary>Every game option that lives in ufc3.toml. Each change rewrites only its own line.</summary>
public class SettingsTab : ILauncherTab
{
    readonly FlowLayoutPanel col = Ui.Column();
    readonly Label status = Ui.Text("");
    bool loading;

    readonly ComboBox scale = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    readonly ComboBox backend = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    readonly NumericUpDown cap = new() { Minimum = 0, Maximum = 4096, Width = 80 };
    readonly Dictionary<string, CheckBox> checks = new();

    public string Title => "Settings";
    public Control View => col;

    static readonly string[] ScaleNames = { "1x (native 720p)", "2x (1440p)", "3x (4K)", "4x (5K+)" };

    public SettingsTab()
    {
        col.Controls.Add(Ui.Text("Game settings", true));
        col.Controls.Add(Ui.Text("Saved to ufc3.toml next to the game. Other settings in that file are left alone. Takes effect the next time you start the game."));

        col.Controls.Add(Ui.Text("Graphics", true));
        scale.Items.AddRange(ScaleNames);
        scale.SelectedIndexChanged += (_, _) => { if (!loading) Write(t => t.Set("resolution_scale", scale.SelectedIndex + 1)); };
        col.Controls.Add(Row("Render scale", scale));
        backend.Items.AddRange(new object[] { "Direct3D 12 (default)", "Vulkan" });
        backend.SelectedIndexChanged += (_, _) => { if (!loading) Write(t => t.SetStr("gpu_backend", backend.SelectedIndex == 1 ? "vulkan" : "d3d12")); };
        col.Controls.Add(Row("Graphics backend", backend));
        col.Controls.Add(Check("fullscreen", "Fullscreen (borderless)", false, "Also F11 in game."));
        col.Controls.Add(Check("ufc3_no_bloom", "Bloom off", false, "Removes the glow around bright areas."));
        col.Controls.Add(Check("ufc3_no_dof", "Depth of field off", false, "Removes the background blur."));
        col.Controls.Add(Check("ufc3_cutscene_60", "60 fps walkouts, intros and replays", false, "The game runs these at 30 fps. Experimental."));

        col.Controls.Add(Ui.Text("Unlocks", true));
        col.Controls.Add(Check("ufc3_unlock_all", "Unlock everything", false, "Every fighter and unlock-shop item counts as unlocked. Your save is not changed."));
        col.Controls.Add(Check("ufc3_free_unlock_points", "Unlock shop items are free", false, "Buying does not spend unlock points."));

        col.Controls.Add(Ui.Text("Mods", true));
        cap.ValueChanged += (_, _) => { if (!loading) Write(t => t.Set("ufc3_roster_cap", (int)cap.Value)); };
        col.Controls.Add(Row("Roster size (fighters)", cap));
        col.Controls.Add(Ui.Text("Room for added fighters. Default 1024; 0 = off (the game's own 600)."));
        col.Controls.Add(Check("ufc3_fighter_music", "Fighter walkout music", false,
            "Plays your own music files for fighters' walkouts. Put the files in the music folder and list them in fighter_music.toml there (fighter number = file name)."));
        var music = Ui.AutoBtn("Open music folder", (_, _) => Ui.OpenFolder(MusicDir()));
        col.Controls.Add(music);

        col.Controls.Add(status);
    }

    static string MusicDir()
    {
        var d = new Toml().Get("ufc3_fighter_music_dir");
        return string.IsNullOrWhiteSpace(d) ? Path.Combine(Paths.Mods, "music") : d;
    }

    static Control Row(string label, Control c)
    {
        var p = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, Margin = new Padding(0, 2, 0, 2) };
        p.Controls.Add(new Label { Text = label, AutoSize = false, Width = 150, Height = 24, TextAlign = ContentAlignment.MiddleLeft });
        p.Controls.Add(c);
        return p;
    }

    Control Check(string key, string text, bool def, string hint)
    {
        var cb = new CheckBox { Text = text, AutoSize = true, Tag = def };
        cb.CheckedChanged += (_, _) => { if (!loading) Write(t => t.Set(key, cb.Checked)); };
        checks[key] = cb;
        var p = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = new Padding(0, 2, 0, 4) };
        p.Controls.Add(cb);
        var h = Ui.Text(hint); h.ForeColor = SystemColors.GrayText; h.Margin = new Padding(20, 0, 0, 0);
        p.Controls.Add(h);
        return p;
    }

    void Write(Action<Toml> edit)
    {
        try
        {
            if (Dlss5.GameRunning()) { status.Text = "The game is running: changes apply at the next start."; }
            var t = new Toml(); edit(t); t.Save();
            if (!Dlss5.GameRunning()) status.Text = "Saved.";
        }
        catch (Exception ex) { status.Text = "Could not save ufc3.toml: " + ex.Message; }
    }

    public void OnShown()
    {
        loading = true;
        var t = new Toml();
        foreach (var (k, cb) in checks) cb.Checked = t.GetBool(k, (bool)cb.Tag!);
        scale.SelectedIndex = Math.Clamp(t.GetInt("resolution_scale", 1), 1, 4) - 1;
        backend.SelectedIndex = t.GetStr("gpu_backend", "vulkan").Equals("vulkan", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
        cap.Value = Math.Clamp(t.GetInt("ufc3_roster_cap", 1024), 0, 4096);
        status.Text = File.Exists(Paths.Toml) ? "" : "ufc3.toml does not exist yet: it is created when you change something here.";
        loading = false;
    }
}
