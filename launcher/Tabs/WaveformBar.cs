namespace UFC3Launcher.Tabs;

/// <summary>Whole-song waveform with a time ruler, a START and an END handle (the part between them is the walkout
/// song, highlighted) and a playhead. Drag a handle, or click the strip to move the nearest one; while playing a
/// click jumps the playhead instead.</summary>
public sealed class WaveformBar : Control
{
    Music.Peaks? peaks;
    double start, end = 0, play = -1;
    int drag;   // 0 none, 1 start, 2 end
    string message = "";

    public double EndSetting => end;
    public double Duration => peaks?.Seconds ?? 0;
    public double Start { get => start; set { start = Math.Clamp(value, 0, Math.Max(0, Duration)); Invalidate(); } }
    /// <summary>The end handle; 0 means "the end of the song".</summary>
    public double End { get => end <= 0 || end > Duration ? Duration : end; set { end = value; Invalidate(); } }
    public double Playhead { get => play; set { if (value != play) { play = value; Invalidate(); } } }
    /// <summary>While true a click jumps the playhead (listening) instead of moving a handle.</summary>
    public bool SeekMode { get; set; }
    public string Message { get => message; set { message = value; Invalidate(); } }
    public bool HasSong => peaks != null;

    /// <summary>The user moved a handle (true = start).</summary>
    public event Action<bool>? HandleMoved;
    public event Action<double>? SeekRequested;

    public WaveformBar()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Surface;
        Height = Theme.S(112);
        TabStop = false;
    }

    public void SetPeaks(Music.Peaks? p) { peaks = p; play = -1; start = 0; end = 0; Invalidate(); }

    int RulerH => Theme.S(22);
    int Pad => Theme.S(10);
    RectangleF Strip => new(Pad, Theme.S(6), Width - 2 * Pad, Height - RulerH - Theme.S(8));
    float X(double sec) => Strip.X + (float)(sec / Math.Max(0.001, Duration)) * Strip.Width;
    double T(float x) => Math.Clamp((x - Strip.X) / Strip.Width * Duration, 0, Duration);

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (peaks == null || e.Button != MouseButtons.Left) return;
        float grab = Theme.S(9);
        bool onStart = Math.Abs(e.X - X(start)) <= grab, onEnd = Math.Abs(e.X - X(End)) <= grab;
        if (onStart && onEnd) drag = e.X < X(start) ? 1 : 2;
        else if (onStart) drag = 1;
        else if (onEnd) drag = 2;
        else if (SeekMode) { SeekRequested?.Invoke(T(e.X)); return; }
        else drag = Math.Abs(e.X - X(start)) <= Math.Abs(e.X - X(End)) ? 1 : 2;
        Capture = true;
        Move(e.X);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (drag != 0) { Move(e.X); return; }
        if (peaks == null) return;
        float grab = Theme.S(9);
        Cursor = Math.Abs(e.X - X(start)) <= grab || Math.Abs(e.X - X(End)) <= grab ? Cursors.SizeWE : Cursors.Default;
    }

    protected override void OnMouseUp(MouseEventArgs e) { base.OnMouseUp(e); drag = 0; Capture = false; }

    void Move(float x)
    {
        double t = Math.Round(T(x) * 10) / 10, gap = Math.Min(1, Duration / 2);
        if (drag == 1) { start = Math.Clamp(t, 0, End - gap); HandleMoved?.Invoke(true); }
        else { end = Math.Clamp(t, start + gap, Duration); if (end >= Duration - 0.05) end = 0; HandleMoved?.Invoke(false); }
        Invalidate();
    }

    static double Step(double pxPerSec, float minPx)
    {
        foreach (var s in new double[] { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 }) if (s * pxPerSec >= minPx) return s;
        return 900;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Surface);
        var strip = Strip;
        using (var b = new SolidBrush(Theme.Bg)) g.FillRectangle(b, strip);
        if (peaks == null || Duration <= 0)
        {
            TextRenderer.DrawText(g, message, Theme.Body, Rectangle.Round(strip), Theme.Muted, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
            using var pn = new Pen(Theme.Line); g.DrawRectangle(pn, Rectangle.Round(strip));
            return;
        }
        float xs = X(start), xe = X(End);
        double cut = Math.Min(End, start + Music.WalkoutSeconds);   // where the game stops the walkout music
        float xc = X(cut);
        using (var b = new SolidBrush(Color.FromArgb(48, Theme.Accent))) g.FillRectangle(b, xs, strip.Y, xc - xs, strip.Height);
        using (var b = new SolidBrush(Color.FromArgb(22, Theme.Muted))) g.FillRectangle(b, xc, strip.Y, xe - xc, strip.Height);

        // peaks, one vertical line per pixel column
        int w = (int)strip.Width; float mid = strip.Y + strip.Height / 2, half = strip.Height / 2 - Theme.S(2);
        float norm = 0.001f;
        for (int i = 0; i < peaks.Max.Length; i++) norm = Math.Max(norm, Math.Max(peaks.Max[i], -peaks.Min[i]));
        double dim = Duration / w;
        using var outside = new Pen(Color.FromArgb(0x55, 0x55, 0x5E));
        using var inside = new Pen(Theme.Accent);
        using var played = new Pen(Color.FromArgb(255, 150, 150));
        using var kept = new Pen(Color.FromArgb(0x84, 0x84, 0x8E));
        for (int px = 0; px < w; px++)
        {
            double t0 = px * dim, t1 = (px + 1) * dim;
            int i0 = (int)(t0 / Music.Peaks.Step), i1 = Math.Max(i0, (int)(t1 / Music.Peaks.Step));
            float lo = 0, hi = 0;
            for (int i = i0; i <= i1 && i < peaks.Max.Length; i++) { lo = Math.Min(lo, peaks.Min[i]); hi = Math.Max(hi, peaks.Max[i]); }
            float x = strip.X + px + 0.5f;
            bool inRange = t1 > start && t0 < End;
            var pen = !inRange ? outside : t0 >= cut ? kept : (play >= 0 && t1 <= play && play >= start ? played : inside);
            float y0 = mid - hi / norm * half, y1 = mid - lo / norm * half;
            if (y1 - y0 < 1) { y0 = mid - 0.5f; y1 = mid + 0.5f; }
            g.DrawLine(pen, x, y0, x, y1);
        }

        // ruler
        double step = Step(strip.Width / Duration, Theme.S(54));
        var rf = Theme.Font(8.5f);
        using (var tick = new Pen(Theme.Line))
            for (double t = 0; t <= Duration + 0.001; t += step)
            {
                float x = X(t);
                g.DrawLine(tick, x, strip.Bottom, x, strip.Bottom + Theme.S(4));
                var txt = Music.FormatTime(t);
                var sz = TextRenderer.MeasureText(txt, rf, new Size(1000, 100), TextFormatFlags.NoPadding);
                float tx = Math.Clamp(x - sz.Width / 2f, 0, Width - sz.Width);
                TextRenderer.DrawText(g, txt, rf, new Point((int)tx, (int)strip.Bottom + Theme.S(5)), Theme.Muted, TextFormatFlags.NoPadding);
            }
        using (var pn = new Pen(Theme.Line)) g.DrawRectangle(pn, Rectangle.Round(strip));

        if (start + Music.WalkoutSeconds <= Duration + 0.01)
        {
            using var cp = new Pen(Theme.AccentText, Math.Max(2, Theme.S(2))) { DashStyle = System.Drawing.Drawing2D.DashStyle.Dash };
            g.DrawLine(cp, xc, strip.Y, xc, strip.Bottom);
            var lab = "Walkout ends ~" + Music.FormatTime(start + Music.WalkoutSeconds);
            var lf = Theme.Font(8.5f);
            var sz = TextRenderer.MeasureText(lab, lf, new Size(1000, 100), TextFormatFlags.NoPadding);
            float lx = xc + Theme.S(4) + sz.Width + Theme.S(6) <= strip.Right ? xc + Theme.S(4) : xc - Theme.S(4) - sz.Width - Theme.S(4);
            var lr = new RectangleF(lx, strip.Bottom - sz.Height - Theme.S(8), sz.Width + Theme.S(4), sz.Height + Theme.S(2));
            using (var bb = new SolidBrush(Color.FromArgb(220, Theme.Bg))) g.FillRectangle(bb, lr);
            TextRenderer.DrawText(g, lab, lf, new Point((int)lr.X + Theme.S(2), (int)lr.Y + 1), Theme.AccentText, TextFormatFlags.NoPadding);
        }
        Handle(g, xs, true, strip);
        Handle(g, xe, false, strip);

        if (play >= 0 && play <= Duration)
        {
            using var pp = new Pen(Theme.Ink, Math.Max(2, Theme.S(2)));
            float x = X(play);
            g.DrawLine(pp, x, strip.Y, x, strip.Bottom);
        }
    }

    void Handle(Graphics g, float x, bool isStart, RectangleF strip)
    {
        using var p = new Pen(Theme.Gold, Math.Max(2, Theme.S(2)));
        g.DrawLine(p, x, strip.Y, x, strip.Bottom);
        float fw = Theme.S(14), fh = Theme.S(16);
        var r = isStart ? new RectangleF(x, strip.Y, fw, fh) : new RectangleF(x - fw, strip.Y, fw, fh);
        using (var b = new SolidBrush(Theme.Gold)) g.FillRectangle(b, r);
        using var a = new SolidBrush(Theme.Bg);
        float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, s = Theme.S(3.5f);
        var pts = isStart ? new[] { new PointF(cx - s, cy - s), new PointF(cx + s, cy), new PointF(cx - s, cy + s) }
                          : new[] { new PointF(cx + s, cy - s), new PointF(cx - s, cy), new PointF(cx + s, cy + s) };
        g.FillPolygon(a, pts);
    }
}
