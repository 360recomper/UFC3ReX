using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace UFC3Launcher.Tabs;

/// <summary>Walkout songs per fighter. Writes <music folder>\<name>.m4a and a [[track]] block in fighter_music.toml;
/// the game (ufc3_fighter_music) does the rest. Never copies, moves or deletes the user's original file.</summary>
public class MusicTab : ILauncherTab
{
    sealed class Row
    {
        public RosterFighter F = null!;
        public string? Song;   // file name the game will play for this fighter, or null
        public override string ToString() => F.Name + (Song != null ? "  ♪" : "");
    }

    /// <summary>One list slot per promotion: game ids 300-399 are the PRIDE versions, the rest below 1000 the UFC ones.
    /// Ultimate Fights mode versions (1000+) are left alone (they keep the game's music).</summary>
    // Extra versions the mod pack adds under the same name (legacy roster): each gets its own slot.
    static readonly Dictionary<int, string> VersionNames = new() {
        [141] = "Classic", [193] = "Classic", [194] = "Prime", [195] = "Prime", [197] = "Prime", [199] = "Rookie" };

    internal static IEnumerable<RosterFighter> Slots(RosterFighter f)
    {
        var ufc = f.Ids.Where(i => i < 1000 && (i < 300 || i > 399)).OrderBy(i => i).ToList();
        var pride = f.Ids.Where(i => i >= 300 && i <= 399).ToList();
        var main = ufc.Where(i => !VersionNames.ContainsKey(i)).ToList();
        if (main.Count == 0 && ufc.Count > 0) main.Add(ufc[0]);   // a fighter that only exists as an added version
        int n = 1;
        foreach (var i in ufc.Where(i => !main.Contains(i)))
            yield return new RosterFighter($"{f.Name} ({(VersionNames.TryGetValue(i, out var v) ? v : "Version " + ++n)})", new() { i });
        if (main.Count > 0) yield return new RosterFighter(f.Name, main);
        if (pride.Count > 0) yield return new RosterFighter(f.Name + " (PRIDE)", pride);
    }

    readonly Panel root = new() { BackColor = Theme.Bg, Padding = new Padding(Theme.S(28), Theme.S(4), Theme.S(28), Theme.S(16)) };
    readonly TextBox search = new() { Dock = DockStyle.Top, BackColor = Theme.Surface, ForeColor = Theme.Ink, BorderStyle = BorderStyle.FixedSingle, Font = Theme.Body };
    readonly DarkCheck onlySongs = new() { Text = "Only fighters with a song", AutoSize = true, Dock = DockStyle.Bottom };
    readonly DarkList list = new() { Dock = DockStyle.Fill };
    readonly Label listNote = Ui.Text("", w: 260);

    readonly Label name = new DimLabel { AutoSize = true, Font = Theme.Font(14f, true), ForeColor = Theme.Ink, MaximumSize = new Size(Theme.S(340), 0), Margin = new Padding(0, 0, 0, Theme.S(2)) };
    readonly Label ids = Ui.Text("", w: 340);
    readonly Label current = Ui.Text("", w: 340);
    readonly Label picked = Ui.Text("", w: 340);
    readonly WaveformBar wave = new() { Margin = new Padding(0, Theme.S(8), 0, Theme.S(4)) };
    readonly Label info = Ui.Text("", w: 340);
    readonly System.Windows.Forms.Timer tick = new() { Interval = 40 };
    readonly FlatButton playBtn, stopBtn;
    double startSec;
    Music.Peaks? peaks; string? peaksPath; CancellationTokenSource? peaksCts;
    int dropVersion;
    IDisposable? playOwner; double playOffset; int bytesPerSec;
    readonly DarkCombo loud = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = Theme.S(180) };
    readonly FlatButton choose, save, remove;
    readonly Label status = Ui.Text("", w: 340);
    readonly FlowLayoutPanel detail = new() { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Padding = new Padding(Theme.S(20), 0, 0, 0), BackColor = Theme.Bg };

    static readonly string[] LoudNames = { "Quieter (70%)", "Normal (100%)", "Louder (130%)" };
    static readonly int[] LoudValues = { 70, 100, 130 };

    List<Row> rows = new();
    string? rosterError;
    string? srcPath;
    bool busy, loading, sample;
    WaveOutEvent? player;
    MusicClip? clipCache; (string, double, int) clipKey;

    public string Title => "Music";
    public Control View => root;

    public MusicTab()
    {
        var left = new Panel { Dock = DockStyle.Left, Width = Theme.S(300), BackColor = Theme.Bg, Padding = new Padding(0, 0, Theme.S(6), 0) };
        var listHost = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Surface, Padding = new Padding(1) };
        listHost.Controls.Add(list);
        var searchRow = new Panel { Dock = DockStyle.Top, Height = Theme.S(58), BackColor = Theme.Bg };
        searchRow.Controls.Add(search);
        searchRow.Controls.Add(new DimLabel { Text = "Search fighters", Dock = DockStyle.Top, Height = Theme.S(24), ForeColor = Theme.Muted });
        onlySongs.Dock = DockStyle.Bottom; onlySongs.Height = Theme.S(30);
        left.Controls.Add(listHost);
        left.Controls.Add(onlySongs);
        left.Controls.Add(searchRow);
        search.Margin = Padding.Empty;

        detail.Controls.Add(Ui.Head("Walkout song"));
        detail.Controls.Add(name);
        detail.Controls.Add(current);
        choose = Ui.Btn("Choose song...", (_, _) => ChooseSong(), 150);
        detail.Controls.Add(choose);
        detail.Controls.Add(picked);

        detail.Controls.Add(wave);
        detail.Controls.Add(info);
        wave.DragStarted += () => { dropVersion++; StopPlayer(); UpdateButtons(); };
        wave.Dropped += () => { startSec = wave.Start; UpdateInfo(); PlayWindow(); };
        wave.WindowClicked += () => { if (player != null) { StopPlayer(); UpdateButtons(); } else PlayWindow(); };
        tick.Tick += (_, _) => Tick();
        detail.SizeChanged += (_, _) => FitWidth();
        loud.Items.AddRange(LoudNames); loud.SelectedIndex = 1;
        detail.Controls.Add(FormRow("Loudness", loud, null));

        var btns = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = true, Margin = new Padding(0, Theme.S(4), 0, 0), MaximumSize = new Size(Theme.S(560), 0) };
        playBtn = Ui.Btn("Play whole song", (_, _) => PlayWhole(), 130);
        stopBtn = Ui.Btn("Stop", (_, _) => { StopPlayer(); UpdateButtons(); }, 70);
        save = Ui.Primary(Ui.Btn("Save", (_, _) => Save(), 90));
        remove = Ui.Btn("Remove song", (_, _) => RemoveSong(), 120);
        btns.Controls.AddRange(new Control[] { playBtn, stopBtn, save, remove });
        detail.Controls.Add(btns);
        detail.Controls.Add(status);
        detail.Controls.Add(Ui.AutoBtn("Open music folder", (_, _) => Ui.OpenFolder(Music.Dir())));

        detail.HorizontalScroll.Enabled = false; detail.HorizontalScroll.Visible = false;
        root.Controls.Add(detail);
        root.Controls.Add(left);

        search.TextChanged += (_, _) => Refill();
        onlySongs.CheckedChanged += (_, _) => Refill();
        list.SelectedIndexChanged += (_, _) => { if (!loading) ShowSelected(); };
        root.Disposed += (_, _) => { StopPlayer(); tick.Dispose(); };
        ShowSelected();
    }

    static Control FormRow(string label, Control c, string? hint)
    {
        var p = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, Margin = new Padding(0, Theme.S(6), 0, Theme.S(2)) };
        p.Controls.Add(new DimLabel { Text = label, AutoSize = false, Width = Theme.S(80), Height = Theme.S(26), ForeColor = Theme.Ink, TextAlign = ContentAlignment.MiddleLeft });
        c.Margin = new Padding(0, Theme.S(1), Theme.S(10), 0);
        p.Controls.Add(c);
        if (hint != null) p.Controls.Add(new DimLabel { Text = hint, AutoSize = false, Width = Theme.S(170), Height = Theme.S(26), ForeColor = Theme.Muted, TextAlign = ContentAlignment.MiddleLeft });
        return p;
    }

    // ------------------------------------------------------------------ data

    public void OnShown()
    {
        if (sample) return;
        rosterError = null;
        var keep = Selected?.F.Name;
        try
        {
            if (!Paths.AssetsReady) throw new FileNotFoundException("Set up the game files first (Setup tab): the fighter list is read from them.");
            var roster = Roster.Load(Paths.Assets, Paths.Mods);
            rows = roster.SelectMany(Slots).OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase).Select(f => new Row { F = f }).ToList();
        }
        catch (Exception ex) { rows = new(); rosterError = ex.Message; }
        Reload(keep);
    }

    /// <summary>Screenshots: a made-up fighter list with a few songs, nothing read or written.</summary>
    public void PreviewSample()
    {
        sample = true;
        string[] n = { "Anderson Silva", "Brock Lesnar", "Chuck Liddell", "Dan Henderson", "Fedor Emelianenko", "Forrest Griffin", "Frank Mir", "Georges St-Pierre",
            "Hector Lombard", "Jon Jones", "Kazushi Sakuraba", "Mark Coleman", "Mirko Cro Cop", "Quinton Jackson", "Randy Couture", "Rich Franklin", "Tito Ortiz", "Wanderlei Silva" };
        rows = n.SelectMany((s, i) => Slots(new RosterFighter(s, i % 3 == 0 ? new() { 10 + i, 310 + i, 1010 + i } : new() { 10 + i }))).Select((f, i) => new Row { F = f, Song = i % 4 == 1 ? "song.m4a" : null }).ToList();
        Reload("Georges St-Pierre");
        var r = Selected;
        if (r != null) { name.Text = r.F.Name; srcPath = @"C:\Music\Walkout.mp3"; picked.Text = "Song: Walkout.mp3"; startSec = 45; wave.SetPeaks(SamplePeaks()); wave.Start = 45; wave.Playhead = 62; UpdateInfo(); current.Text = "Now: Walkout.mp3"; status.Text = "Saved. Turned on Fighter walkout music in Settings."; UpdateButtons(); save.Enabled = remove.Enabled = true; }
    }

    Row? Selected => list.SelectedItem as Row;

    void Reload(string? keep)
    {
        if (!sample) ApplyMapping();
        Refill(keep);
    }

    /// <summary>Which fighters already play a song, as the game reads fighter_music.toml.</summary>
    void ApplyMapping()
    {
        var map = Music.Parse(Music.ReadLines(Music.TomlFile()));
        foreach (var r in rows) r.Song = r.F.Ids.Select(i => map.Tracks.GetValueOrDefault(i)).FirstOrDefault(s => s != null);
    }

    void Refill(string? keep = null)
    {
        keep ??= Selected?.F.Name;
        loading = true;
        var q = search.Text.Trim();
        list.BeginUpdate();
        list.Items.Clear();
        foreach (var r in rows)
        {
            if (onlySongs.Checked && r.Song == null) continue;
            if (q.Length > 0 && !(r.F.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || r.F.Ids.Any(i => i.ToString().Contains(q)))) continue;
            list.Items.Add(r);
        }
        list.EndUpdate();
        int idx = keep == null ? -1 : list.Items.Cast<Row>().ToList().FindIndex(r => r.F.Name == keep);
        if (idx >= 0) list.SelectedIndex = idx;
        loading = false;
        listNote.Text = "";
        ShowSelected();
    }

    void ShowSelected()
    {
        var r = Selected;
        bool has = r != null && !busy;
        if (r == null)
        {
            name.Text = rosterError ?? (rows.Count == 0 ? "No fighters found." : "Pick a fighter on the left.");
            ids.Text = ""; current.Text = ""; picked.Text = "";
        }
        else
        {
            name.Text = r.F.Name;
            ids.Text = "";
            var own = sample ? null : Music.OwnEntries(Music.ReadLines(Music.TomlFile())).FirstOrDefault(e => e.Name.Equals(r.F.Name, StringComparison.OrdinalIgnoreCase));
            current.Text = r.Song == null ? "Now: the game's normal walkout music."
                : "Now: " + (own != null && File.Exists(own.Source) ? Path.GetFileName(own.Source) : r.Song) + (own != null ? "" : " (set by hand)");
            if (own != null && !sample)
            {
                if (File.Exists(own.Source)) { srcPath = own.Source; picked.Text = "Song: " + Path.GetFileName(own.Source); }
                else { srcPath = null; picked.Text = "The original file was moved. Choose it again to change the song."; }
                startSec = own.Start;
                SelectLoud(own.Loudness);
            }
            else if (!sample) { srcPath = null; picked.Text = "No song chosen."; startSec = 0; SelectLoud(100); }
        }
        StopPlayer();
        if (!sample) LoadPeaks();
        UpdateButtons();
    }

    // Only the buttons; never touches the chosen song, start or loudness (Preview/Save finishing must not reset them).
    void UpdateButtons()
    {
        var r = Selected;
        bool has = r != null && !busy;
        choose.Enabled = has;
        playBtn.Enabled = has && srcPath != null;
        stopBtn.Enabled = player != null;
        save.Enabled = has && srcPath != null;
        remove.Enabled = has && r?.Song != null && !sample && Music.OwnEntries(Music.ReadLines(Music.TomlFile())).Any(e => e.Name.Equals(r.F.Name, StringComparison.OrdinalIgnoreCase));
    }

    void SelectLoud(int pct)
    {
        int i = Array.IndexOf(LoudValues, pct);
        loud.SelectedIndex = i >= 0 ? i : 1;
    }

    // ------------------------------------------------------------------ actions

    void ChooseSong()
    {
        using var d = new OpenFileDialog { Title = "Choose a walkout song", Filter = "Audio (*.mp3;*.m4a;*.flac;*.wav;*.wma)|*.mp3;*.m4a;*.flac;*.wav;*.wma|All files|*.*" };
        if (d.ShowDialog() != DialogResult.OK) return;
        srcPath = d.FileName;
        picked.Text = "Song: " + Path.GetFileName(srcPath);
        clipCache = null;
        status.Text = "";
        startSec = 0;
        StopPlayer();
        LoadPeaks();
        UpdateButtons();
    }

    bool ReadSettings(out double start, out int pct)
    {
        pct = LoudValues[Math.Max(0, loud.SelectedIndex)];
        start = startSec;
        return true;
    }

    void SetBusy(bool b)
    {
        busy = b;
        choose.Enabled = save.Enabled = remove.Enabled = !b; if (b) playBtn.Enabled = false;
        if (!b) UpdateButtons();
    }

    void ShowSelectedKeepStatus() { var s = status.Text; ShowSelected(); status.Text = s; }

    async Task<MusicClip?> GetClip(double startSec, int pct)
    {
        var key = (srcPath!, startSec, pct);
        if (clipCache != null && clipKey == key) return clipCache;
        var path = srcPath!;
        var clip = await Task.Run(() => Music.Decode(path, startSec, pct, Music.SaveSeconds));
        clipCache = clip; clipKey = key;
        return clip;
    }

    // ------------------------------------------------------------------ playing

    /// <summary>Plays exactly what the game will play: from the window start, levelled, up to the cut-off.</summary>
    async void PlayWindow()
    {
        if (srcPath == null || busy || !ReadSettings(out var s, out var pct)) return;
        int ver = ++dropVersion;
        try
        {
            SetBusy(true); status.Text = "Reading the song...";
            var clip = await GetClip(s, pct);
            SetBusy(false);
            if (clip == null || ver != dropVersion) return;   // the box was moved again meanwhile
            StopPlayer();
            var w = new WaveOutEvent();
            var prov = new SampleToWaveProvider16(new Music.ClipProvider(clip, Music.WalkoutSeconds));
            w.Init(prov);
            bytesPerSec = prov.WaveFormat.AverageBytesPerSecond; playOffset = s;
            w.PlaybackStopped += (_, _) => { if (player == w) { StopPlayer(); UpdateButtons(); } };
            player = w; w.Play(); tick.Start();
            status.Text = "Playing from " + Music.FormatTime(s) + " to the cut-off, as in the game.";
            UpdateButtons();
        }
        catch (Exception ex) { SetBusy(false); StopPlayer(); UpdateButtons(); status.Text = "Could not play that file: " + ex.Message; }
    }

    void PlayWhole()
    {
        if (srcPath == null || busy) return;
        try
        {
            StopPlayer();
            dropVersion++;
            var prov = Music.OpenStream(srcPath, 0, out var owner);
            var w = new WaveOutEvent();
            w.Init(prov);
            playOwner = owner; bytesPerSec = prov.WaveFormat.AverageBytesPerSecond; playOffset = 0;
            w.PlaybackStopped += (_, _) => { if (player == w) { StopPlayer(); UpdateButtons(); } };
            player = w; w.Play(); tick.Start();
            status.Text = "Playing the whole song.";
            UpdateButtons();
        }
        catch (Exception ex) { StopPlayer(); UpdateButtons(); status.Text = "Could not play that file: " + ex.Message; }
    }

    void Tick()
    {
        if (player == null || bytesPerSec == 0) return;
        double t = playOffset + player.GetPosition() / (double)bytesPerSec;
        if (wave.Duration > 0 && t > wave.Duration) { StopPlayer(); UpdateButtons(); return; }
        wave.Playhead = t;
    }

    // ------------------------------------------------------------------ waveform

    void FitWidth()
    {
        int w = Math.Clamp(detail.ClientSize.Width - detail.Padding.Left - Theme.S(26), Theme.S(300), Theme.S(620));
        wave.Width = w;
        info.MaximumSize = new Size(w, 0);
    }

    void LoadPeaks()
    {
        peaksCts?.Cancel();
        var path = srcPath;
        if (path == null) { peaks = null; peaksPath = null; wave.Message = "Choose a song to see it here."; wave.SetPeaks(null); UpdateInfo(); return; }
        if (peaks != null && peaksPath == path) { ApplyPeaks(peaks); return; }
        wave.Message = "Reading the song..."; wave.SetPeaks(null); UpdateInfo();
        var cts = peaksCts = new CancellationTokenSource();
        Task.Run(() => Music.ComputePeaks(path, cts.Token)).ContinueWith(t =>
        {
            if (cts.IsCancellationRequested || srcPath != path) return;
            if (t.IsFaulted) { wave.Message = "Could not read this file."; wave.SetPeaks(null); UpdateInfo(); return; }
            peaks = t.Result; peaksPath = path;
            ApplyPeaks(peaks);
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    // Only the waveform and the times: never the buttons or the chosen song (Preview/Save finishing must not reset them).
    void ApplyPeaks(Music.Peaks p)
    {
        wave.SetPeaks(p);
        wave.Start = startSec;
        startSec = wave.Start;
        UpdateInfo();
    }

    void UpdateInfo()
    {
        if (wave.Duration <= 0) { info.Text = srcPath == null ? "" : wave.Message; return; }
        info.Text = $"Drag the box to the part you want. It plays when you let go. Plays {Music.FormatTime(startSec)} to {Music.FormatTime(startSec + wave.Window)} (about {(int)Math.Round(wave.Window)} s).";
    }

    static Music.Peaks SamplePeaks()
    {
        int n = (int)(252 / Music.Peaks.Step);
        var p = new Music.Peaks { Min = new float[n], Max = new float[n], Seconds = 252 };
        var rnd = new Random(7);
        for (int i = 0; i < n; i++)
        {
            double t = i * Music.Peaks.Step;
            double env = 0.25 + 0.35 * Math.Abs(Math.Sin(t / 9.0)) + (t > 45 && t < 140 ? 0.25 : 0) + (t < 12 ? -0.15 : 0);
            p.Max[i] = (float)Math.Clamp(env * (0.7 + 0.3 * rnd.NextDouble()), 0.02, 1);
            p.Min[i] = -(float)Math.Clamp(env * (0.7 + 0.3 * rnd.NextDouble()), 0.02, 1);
        }
        return p;
    }

    void StopPlayer()
    {
        var p = player; player = null;
        tick.Stop();
        try { p?.Stop(); p?.Dispose(); } catch { }
        try { playOwner?.Dispose(); } catch { }
        playOwner = null;
        wave.Playhead = -1;
    }

    async void Save()
    {
        var r = Selected;
        if (r == null || srcPath == null || busy || !ReadSettings(out var s, out var pct)) return;
        StopPlayer();
        try
        {
            SetBusy(true); status.Text = "Cutting and converting...";
            var dir = Music.Dir();
            var file = Music.SafeName(r.F.Name);
            var outPath = Path.Combine(dir, file);
            var clip = await GetClip(s, pct);
            await Task.Run(() => Music.EncodeM4a(clip!, outPath));
            // an earlier m4a of ours with another name (renamed fighter) would be left behind: not possible, the name is derived from the fighter
            Music.SetEntry(Music.TomlFile(dir), new Music.Entry(r.F.Name, file, r.F.Ids.ToList(), s, pct, srcPath));
            bool turnedOn = Music.EnableInGame();
            status.Text = $"Saved {file} ({Music.FormatTime(clip!.Seconds)}, ends with a short fade)." + (turnedOn ? " \"Fighter walkout music\" was off: it is on now." : "");
            SetBusy(false);
            Reload(r.F.Name);
            ShowSelectedKeepStatus();
        }
        catch (Exception ex) { SetBusy(false); status.Text = "Could not save: " + ex.Message; }
    }

    void RemoveSong()
    {
        var r = Selected;
        if (r == null || busy) return;
        if (DarkBox.Show($"Remove the walkout song of {r.F.Name}? The game goes back to its own music for this fighter. Your original file is not touched.", "Undisputed Launcher", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        StopPlayer();
        try
        {
            var dir = Music.Dir();
            var gone = Music.RemoveEntry(Music.TomlFile(dir), r.F.Name);
            // only a file this tab made: <safe name>.m4a in the music folder, and no other entry still uses it
            var still = Music.Parse(Music.ReadLines(Music.TomlFile(dir)));
            if (!string.IsNullOrEmpty(gone) && gone == Music.SafeName(r.F.Name) && !still.Tracks.Values.Contains(gone) && still.Any != gone)
            {
                var p = Path.Combine(dir, gone);
                if (File.Exists(p)) File.Delete(p);
            }
            Reload(r.F.Name);
            status.Text = r.Song == null ? "Song removed." : $"Song removed here, but fighter_music.toml still maps this fighter to {r.Song} by hand.";
        }
        catch (Exception ex) { status.Text = "Could not remove: " + ex.Message; }
    }
}
