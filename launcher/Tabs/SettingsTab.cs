namespace UFC3Launcher.Tabs;

/// <summary>Every game option that lives in ufc3.toml. Each change rewrites only its own line.</summary>
public class SettingsTab : ILauncherTab
{
    readonly FlowLayoutPanel col = Ui.Column();
    readonly Label status = Ui.Text("");
    bool loading, buildingCache;

    readonly DarkCombo scale = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = Theme.S(260) };
    readonly DarkCombo backend = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = Theme.S(260) };
    readonly DarkCombo renderer = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = Theme.S(260) };
    readonly Label rendererHint = Ui.Text("");
    // NR-12c: graphics card. gpuValues[i] is what item i writes ("" = Automatic).
    readonly DarkCombo gpu = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = Theme.S(260) };
    readonly List<string> gpuValues = new();
    readonly NumericUpDown cap = new() { Minimum = 0, Maximum = 4096, Width = Theme.S(80), BackColor = Theme.Surface, ForeColor = Theme.Ink };
    readonly Dictionary<string, DarkCheck> checks = new();
    // merge7: the Gameplay rows only work with the mod pack (its marker file in the mods folder).
    readonly List<Control> packOnly = new();
    readonly Label packHint = Ui.Text("");

    public string Title => "Settings";
    public Control View => col;

    static readonly string[] ScaleNames = { "1x (native 720p)", "2x (1440p)", "3x (4K)", "4x (5K+)" };
    static readonly string[] GlovedNames = { "Stock", "Slightly more", "More", "Lots" };
    static readonly string[] GlovedValues = { "1.0", "1.5", "2.0", "4.0" };
    static readonly string[] BkNames = { "Stock", "Some", "More", "Lots", "Most" };
    static readonly string[] BkValues = { "1.0", "1.5", "2.0", "3.0", "5.0" };
    readonly Dictionary<string, (DarkCombo box, string[] values, string def)> choices = new();

    public SettingsTab()
    {
        col.Controls.Add(Ui.Head("Game settings"));
        col.Controls.Add(Ui.Text("Saved to ufc3.toml next to the game. Other settings in that file are left alone. Takes effect the next time you start the game."));

        col.Controls.Add(Ui.Head("Graphics"));
        scale.Items.AddRange(ScaleNames);
        scale.SelectedIndexChanged += (_, _) => { if (!loading) Write(t => t.Set("resolution_scale", scale.SelectedIndex + 1)); };
        col.Controls.Add(Row("Render scale", scale));
        backend.Items.AddRange(new object[] { "Direct3D 12", "Vulkan (default)" });
        backend.SelectedIndexChanged += (_, _) =>
        {
            if (!loading) Write(t => t.SetStr("gpu_backend", backend.SelectedIndex == 1 ? "vulkan" : "d3d12"));
        };
        col.Controls.Add(Row("Graphics backend", backend));
        renderer.Items.AddRange(new object[] { "Emulated (fallback)", "Native (default)" });
        renderer.SelectedIndexChanged += (_, _) =>
        {
            if (loading) return;
            Write(t => t.SetStr("ufc3_renderer", renderer.SelectedIndex == 1 ? "native" : "emulated"));
            if (renderer.SelectedIndex == 1) _ = BuildCacheIfNeeded();
        };
        col.Controls.Add(Row("Renderer", renderer));
        rendererHint.ForeColor = Theme.Muted;
        col.Controls.Add(rendererHint);
        gpu.SelectedIndexChanged += (_, _) =>
        {
            if (loading || gpu.SelectedIndex < 0) return;
            var v = gpuValues[gpu.SelectedIndex];
            Write(t => t.SetStr(Gpu.TomlKey, v));
        };
        col.Controls.Add(Row("Graphics card", gpu));
        var gpuHint = Ui.Text("Native renderer only. Automatic picks the dedicated (high-performance) graphics card on laptops that also have built-in graphics. Pick a card by name to override it; Windows' own per-app graphics setting can override Automatic too.");
        gpuHint.ForeColor = Theme.Muted;
        col.Controls.Add(gpuHint);
        // NR-13: present pass (ufc3_native_upscale); a picture as big as the window is never changed.
        col.Controls.Add(Choice("ufc3_native_upscale", "Upscaling", new[] { "Bilinear (default)", "AMD FSR 1" },
            new[] { "bilinear", "auto" }, "bilinear", raw: false));
        var upscaleHint = Ui.Text("Native renderer only. Bilinear is the plain scaling. AMD FSR 1 is optional: when the render scale gives a picture smaller than the window (1x on a 1080p or bigger screen), it scales it up sharper than bilinear. A picture as big as the window or bigger looks the same either way.");
        upscaleHint.ForeColor = Theme.Muted;
        col.Controls.Add(upscaleHint);
        col.Controls.Add(Check("fullscreen", "Fullscreen (borderless)", true, "Also F11 in game. Alt-tab is instant: the picture stays while another window has focus."));
        col.Controls.Add(Check("pause_on_focus_loss", "Mute while in the background", false, "Sound stops while another window has focus. The game keeps running either way; the controller is ignored until you come back."));
        col.Controls.Add(Check("ufc3_no_bloom", "Bloom off", false, "Has no visible effect: fights in this game don't use bloom."));
        col.Controls.Add(Check("ufc3_no_dof", "Depth of field off", false, "Removes the background blur."));
        col.Controls.Add(Check("ufc3_cutscene_60", "60 fps walkouts, intros and replays", false, "The game runs these at 30 fps. Experimental."));
        col.Controls.Add(Check("ufc3_steady_skin_light", "Steady skin lighting", true, "Skin keeps its stand-up tone on clinch and ground cameras. Off = the original game's darker top-down look."));

        col.Controls.Add(Ui.Head("Gameplay"));
        col.Controls.Add(Ui.Text("These change how fights play out, not only how they look. Stock = the original game."));
        packHint.ForeColor = Theme.Muted;
        col.Controls.Add(packHint);
        packOnly.Add(Choice("ufc3_cut_rate", "Gloved cuts", GlovedNames, GlovedValues, "1.5", raw: true));
        packOnly.Add(Choice("ufc3_bleed_rate", "Gloved bleeding", GlovedNames, GlovedValues, "1.5", raw: true));
        packOnly.Add(Choice("ufc3_bk_cut_rate", "Bare knuckle cuts", BkNames, BkValues, "3.0", raw: true));
        packOnly.Add(Choice("ufc3_bk_bleed_rate", "Bare knuckle bleeding", BkNames, BkValues, "1.5", raw: true));
        foreach (var c in packOnly) col.Controls.Add(c);

        col.Controls.Add(Ui.Head("Unlocks"));
        col.Controls.Add(Check("ufc3_unlock_all", "Unlock everything", false, "Every fighter and unlock-shop item counts as unlocked. Your save is not changed."));
        col.Controls.Add(Check("ufc3_free_unlock_points", "Unlock shop items are free", false, "Buying does not spend unlock points."));
        col.Controls.Add(Check("ufc3_caf_max_points", "Max stats for created fighters", false, "Create-A-Fighter: raising attributes and skills costs no points, so every stat can go up to its limit for the weight class. Not in career mode. Your save stays valid."));

        col.Controls.Add(Ui.Head("Mods"));
        cap.ValueChanged += (_, _) => { if (!loading) Write(t => t.Set("ufc3_roster_cap", (int)cap.Value)); };
        col.Controls.Add(Row("Roster size (fighters)", cap));
        col.Controls.Add(Ui.Text("Room for added fighters. Default 1024; 0 = off (the game's own 600)."));
        col.Controls.Add(Check("ufc3_fighter_music", "Fighter walkout music", false,
            "Plays your own music files for fighters' walkouts. Put the files in the music folder and list them in fighter_music.toml there (fighter number = file name)."));
        var music = Ui.AutoBtn("Open music folder", (_, _) => Ui.OpenFolder(MusicDir()));
        col.Controls.Add(music);

        col.Controls.Add(status);
    }

    /// <summary>Native was just picked. Without game files there's nothing to build from yet: Setup builds the cache
    /// after extracting them. A failed build puts the dropdown back on Emulated.</summary>
    async Task BuildCacheIfNeeded()
    {
        if (ShaderCache.Current) return;
        if (!Paths.AssetsReady) { status.Text = "Saved. The native renderer's shader cache gets built when you set up the game files (Setup tab)."; return; }
        buildingCache = true;
        renderer.Enabled = false;
        rendererHint.Text = "Building the native renderer's shader cache from your game files. This only happens once...";
        ShaderCache.Result r;
        try { r = await Task.Run(() => ShaderCache.EnsurePresent(Paths.Assets)); }
        catch (Exception ex) { r = new ShaderCache.Result(false, ex.Message); }
        buildingCache = false;
        if (r.Ok) status.Text = "Saved. Shader cache built.";
        else
        {
            NativeRenderer.UseEmulated();
            DarkBox.Show(NativeRenderer.FallbackText(r.Message), "Undisputed Launcher", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            status.Text = "Renderer set back to Emulated: the shader cache could not be built.";
        }
        var keep = status.Text;
        OnShown();
        status.Text = keep;
    }

    /// <summary>Automatic, then every card Windows lists. A saved name that isn't listed (another PC, a
    /// removed card) is kept as its own item, so opening Settings never changes it.</summary>
    void LoadGpus(Toml t)
    {
        var saved = t.GetStr(Gpu.TomlKey, "");
        gpu.Items.Clear();
        gpuValues.Clear();
        gpu.Items.Add("Automatic (default)");
        gpuValues.Add("");
        foreach (var n in Gpu.AdapterNames()) { gpu.Items.Add(n); gpuValues.Add(n); }
        if (saved.Length > 0 && !gpuValues.Contains(saved)) { gpu.Items.Add(saved + " (not found)"); gpuValues.Add(saved); }
        gpu.SelectedIndex = Math.Max(0, gpuValues.IndexOf(saved));
    }

    static string MusicDir()
    {
        var d = new Toml().Get("ufc3_fighter_music_dir");
        return string.IsNullOrWhiteSpace(d) ? Path.Combine(Paths.Mods, "music") : d;
    }

    static Control Row(string label, Control c)
    {
        var p = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, Margin = new Padding(0, Theme.S(2), 0, Theme.S(2)) };
        p.Controls.Add(new DimLabel { Text = label, AutoSize = false, Width = Theme.S(150), Height = Theme.S(24), ForeColor = Theme.Ink, TextAlign = ContentAlignment.MiddleLeft });
        p.Controls.Add(c);
        return p;
    }

    Control Check(string key, string text, bool def, string hint)
    {
        var cb = new DarkCheck { Text = text, AutoSize = true, Tag = def };
        cb.CheckedChanged += (_, _) => { if (!loading) Write(t => t.Set(key, cb.Checked)); };
        checks[key] = cb;
        var p = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = new Padding(0, Theme.S(2), 0, Theme.S(4)) };
        p.Controls.Add(cb);
        var h = Ui.Text(hint); h.ForeColor = Theme.Muted; h.Margin = new Padding(Theme.S(24), 0, 0, 0);
        p.Controls.Add(h);
        return p;
    }

    Control Choice(string key, string label, string[] names, string[] values, string def, bool raw)
    {
        var box = new DarkCombo { DropDownStyle = ComboBoxStyle.DropDownList, Width = Theme.S(260) };
        box.Items.AddRange(names);
        box.SelectedIndexChanged += (_, _) =>
        {
            if (loading) return;
            var v = values[box.SelectedIndex];
            Write(t => { if (raw) t.Set(key, v); else t.SetStr(key, v); });
        };
        choices[key] = (box, values, def);
        return Row(label, box);
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
        bool native = NativeRenderer.Available;
        renderer.Enabled = native && !buildingCache;
        renderer.SelectedIndex = native && t.GetStr("ufc3_renderer", "native").Equals("native", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
        if (!buildingCache)
            rendererHint.Text = native
                ? "Native draws the game directly with Direct3D 12 (ignores Graphics backend). Picking it builds a shader cache from your game files once. If it can't start, the game restarts on Emulated. Not used in DLSS 5 runs."
                : "This build has no native renderer.";
        LoadGpus(t);
        foreach (var (k, c) in choices)
        {
            int i = Array.IndexOf(c.values, t.GetStr(k, c.def));
            c.box.SelectedIndex = i >= 0 ? i : Array.IndexOf(c.values, c.def);
        }
        bool pack = ModPack.MarkerPresent;
        foreach (var c in packOnly) c.Enabled = pack;
        packHint.Text = pack
            ? "Mod pack installed: bare knuckle (R3 on Select Fighter), the attire picker (L3) and Blood and Guts are on."
            : "Needs the mod pack (Mod Pack tab): without it the game plays stock, with stock cuts and blood, and no bare knuckle or attire picker.";
        cap.Value = Math.Clamp(t.GetInt("ufc3_roster_cap", 1024), 0, 4096);
        status.Text = File.Exists(Paths.Toml) ? "" : "ufc3.toml does not exist yet: it is created when you change something here.";
        loading = false;
    }
}
