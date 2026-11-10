namespace UFC3Launcher.Tabs;

/// <summary>Whole-song waveform with a time ruler and ONE fixed-width window (the walkout chunk the game will play).
/// Grab anywhere in the window and drag it; click outside it to start the window there. The window is bright, the
/// rest dimmed, with a "Walkout ends" label at its right edge, and a playhead while playing.</summary>
public sealed class WaveformBar : Control
{
    Music.Peaks? peaks;
    double start, play = -1, grab;
    bool dragging, moved;
    string message = "";

    public double Duration => peaks?.Seconds ?? 0;
    /// <summary>Length of the window: the walkout, or the whole song when it is shorter.</summary>
    public double Window => Math.Min(Music.WalkoutSeconds, Duration);
    public double Start { get => start; set { start = Math.Clamp(value, 0, Math.Max(0, Duration - Window)); Invalidate(); } }
    public double Playhead { get => play; set { if (value != play) { play = value; Invalidate(); } } }
    public string Message { get => message; set { message = value; Invalidate(); } }

    /// <summary>The user began moving the window (stop any preview).</summary>
    public event Action? DragStarted;
    /// <summary>The window was moved and let go (play it).</summary>
    public event Action? Dropped;
    /// <summary>The window was clicked without moving it (toggle the preview).</summary>
    public event Action? WindowClicked;

    public WaveformBar()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Surface;
        Height = Theme.S(100);
        TabStop = false;
    }

    public void SetPeaks(Music.Peaks? p) { peaks = p; play = -1; start = 0; Invalidate(); }

    int RulerH => Theme.S(22);
    int Pad => Theme.S(10);
    RectangleF Strip => new(Pad, Theme.S(6), Width - 2 * Pad, Height - RulerH - Theme.S(8));
    float X(double sec) => Strip.X + (float)(sec / Math.Max(0.001, Duration)) * Strip.Width;
    double T(float x) => (x - Strip.X) / Strip.Width * Duration;
    bool InWindow(float x) => x >= X(start) && x <= X(start + Window);

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (peaks == null || e.Button != MouseButtons.Left) return;
        moved = false; dragging = true; Capture = true;
        if (InWindow(e.X)) grab = T(e.X) - start;
        else { grab = 0; DragStarted?.Invoke(); moved = true; Start = Math.Round(T(e.X) * 10) / 10; }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (peaks == null) return;
        if (!dragging) { Cursor = InWindow(e.X) ? Cursors.SizeWE : Cursors.Hand; return; }
        double s = Math.Round((T(e.X) - grab) * 10) / 10;
        if (!moved) { if (Math.Abs(s - start) < 0.3) return; moved = true; DragStarted?.Invoke(); }
        Start = s;
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (!dragging) return;
        dragging = false; Capture = false;
        if (moved) Dropped?.Invoke(); else WindowClicked?.Invoke();
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
        double cut = start + Window;
        float xs = X(start), xc = X(cut);
        using (var b = new SolidBrush(Color.FromArgb(48, Theme.Accent))) g.FillRectangle(b, xs, strip.Y, xc - xs, strip.Height);

        int w = (int)strip.Width; float mid = strip.Y + strip.Height / 2, half = strip.Height / 2 - Theme.S(2);
        float norm = 0.001f;
        for (int i = 0; i < peaks.Max.Length; i++) norm = Math.Max(norm, Math.Max(peaks.Max[i], -peaks.Min[i]));
        double dim = Duration / w;
        using var outside = new Pen(Color.FromArgb(0x55, 0x55, 0x5E));
        using var inside = new Pen(Theme.Accent);
        using var played = new Pen(Color.FromArgb(255, 150, 150));
        for (int px = 0; px < w; px++)
        {
            double t0 = px * dim, t1 = (px + 1) * dim;
            int i0 = (int)(t0 / Music.Peaks.Step), i1 = Math.Max(i0, (int)(t1 / Music.Peaks.Step));
            float lo = 0, hi = 0;
            for (int i = i0; i <= i1 && i < peaks.Max.Length; i++) { lo = Math.Min(lo, peaks.Min[i]); hi = Math.Max(hi, peaks.Max[i]); }
            float x = strip.X + px + 0.5f;
            bool inRange = t1 > start && t0 < cut;
            var pen = !inRange ? outside : (play >= 0 && t1 <= play ? played : inside);
            float y0 = mid - hi / norm * half, y1 = mid - lo / norm * half;
            if (y1 - y0 < 1) { y0 = mid - 0.5f; y1 = mid + 0.5f; }
            g.DrawLine(pen, x, y0, x, y1);
        }

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

        // the window's edges: gold start line, dashed cut-off line with its label
        using (var gp = new Pen(Theme.Gold, Math.Max(2, Theme.S(2)))) g.DrawLine(gp, xs, strip.Y, xs, strip.Bottom);
        using (var cp = new Pen(Theme.AccentText, Math.Max(2, Theme.S(2))) { DashStyle = System.Drawing.Drawing2D.DashStyle.Dash }) g.DrawLine(cp, xc, strip.Y, xc, strip.Bottom);
        var lab = "Walkout ends " + Music.FormatTime(cut);
        var lf = Theme.Font(8.5f);
        var ls = TextRenderer.MeasureText(lab, lf, new Size(1000, 100), TextFormatFlags.NoPadding);
        float lx = xc - Theme.S(4) - ls.Width - Theme.S(4);   // inside the window, at its right edge
        if (lx < xs + Theme.S(3)) lx = xc + Theme.S(4);
        if (lx + ls.Width + Theme.S(4) > strip.Right) lx = strip.Right - ls.Width - Theme.S(4);
        var lr = new RectangleF(lx, strip.Y + Theme.S(4), ls.Width + Theme.S(4), ls.Height + Theme.S(2));
        using (var bb = new SolidBrush(Color.FromArgb(225, Theme.Bg))) g.FillRectangle(bb, lr);
        TextRenderer.DrawText(g, lab, lf, new Point((int)lr.X + Theme.S(2), (int)lr.Y + 1), Theme.AccentText, TextFormatFlags.NoPadding);

        if (play >= 0 && play <= Duration)
        {
            using var pp = new Pen(Theme.Ink, Math.Max(2, Theme.S(2)));
            float x = X(play);
            g.DrawLine(pp, x, strip.Y, x, strip.Bottom);
        }
    }
}
