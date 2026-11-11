using System.Drawing.Drawing2D;
using System.Drawing.Text;

namespace UFC3Launcher.Tabs;

// LAUNCHER-UI: window chrome. Left sidebar navigation (replaces the TabControl), page titles, the Play banner and the
// dark message box. Gradients, shapes and text are drawn at runtime; the glove art is two embedded PNGs (Art/).

/// <summary>Sidebar + page area. SelectedIndex / SelectedIndexChanged work like the old TabControl's.</summary>
public sealed class NavHost : Control
{
    public const int SideW = 204;
    readonly SidePanel side = new();
    readonly Panel content = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly List<NavItem> items = new();
    readonly List<Control> pages = new();
    int sel = -1;

    public event EventHandler? SelectedIndexChanged;

    public NavHost()
    {
        Dock = DockStyle.Fill;
        BackColor = Theme.Bg;
        side.Width = Theme.S(SideW);
        side.Layout += (_, _) => LayoutItems();
        Controls.Add(content);
        Controls.Add(side);
    }

    public int Count => pages.Count;

    public int SelectedIndex
    {
        get => sel;
        set
        {
            if (value < 0 || value >= pages.Count || value == sel) return;
            content.SuspendLayout();
            pages[value].Visible = true;
            pages[value].BringToFront();
            for (int i = 0; i < pages.Count; i++) if (i != value) pages[i].Visible = false;
            content.ResumeLayout();
            sel = value;
            for (int i = 0; i < items.Count; i++) items[i].Selected = i == value;
            SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <param name="glyph">A Segoe Fluent Icons / MDL2 code point.</param>
    /// <param name="pageTitle">Draw the page title above the view (Play and Attire have their own headers).</param>
    public void Add(string title, string glyph, Control view, bool pageTitle)
    {
        var page = new Panel { Dock = DockStyle.Fill, Visible = false, BackColor = Theme.Bg };
        view.Dock = DockStyle.Fill;
        page.Controls.Add(view);
        if (pageTitle) page.Controls.Add(new PageTitle(title));
        content.Controls.Add(page);
        pages.Add(page);
        int idx = items.Count;
        var item = new NavItem(glyph, title);
        item.Click += (_, _) => SelectedIndex = idx;
        items.Add(item);
        side.Controls.Add(item);
        LayoutItems();
    }

    void LayoutItems()
    {
        int y = Theme.S(SidePanel.BrandH), h = Theme.S(46);
        foreach (var it in items)
        {
            it.SetBounds(Theme.S(10), y, side.Width - Theme.S(20), h);
            y += h + Theme.S(4);
        }
    }

    public void Next(int step)
    {
        if (pages.Count > 0) SelectedIndex = ((sel < 0 ? 0 : sel) + step + pages.Count) % pages.Count;
    }

    sealed class SidePanel : Panel
    {
        public const int BrandH = 104;

        public SidePanel()
        {
            Dock = DockStyle.Left;
            BackColor = Theme.Side;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.ResizeRedraw, true);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            // brand: a small glove mark + the name
            float cx = Theme.S(34), cy = Theme.S(46);
            Art.Draw(g, Art.Side, new PointF(cx, cy), Theme.S(42));
            using var sf = new StringFormat(StringFormat.GenericTypographic);
            var f1 = Theme.Heavy(11.5f, FontStyle.Italic);
            var f1b = Theme.Heavy(8.5f, FontStyle.Italic);
            float tx = Theme.S(64);
            using (var b = new LinearGradientBrush(new RectangleF(tx, Theme.S(26), Theme.S(140), Theme.S(24)), Color.White, Theme.AccentText, LinearGradientMode.Vertical))
                g.DrawString("UNDISPUTED", f1, b, tx, Theme.S(27), sf);
            using (var b = new SolidBrush(Theme.AccentText)) g.DrawString("ULTIMATE EDITION", f1b, b, tx, Theme.S(47), sf);
            using (var p = new Pen(Theme.Line)) g.DrawLine(p, Width - 1, 0, Width - 1, Height);
            using (var lb = new LinearGradientBrush(new RectangleF(Theme.S(16), Theme.S(86), Width - Theme.S(32), 1), Color.FromArgb(150, Theme.Gold), Color.FromArgb(0, Theme.Gold), LinearGradientMode.Horizontal))
                g.FillRectangle(lb, Theme.S(16), Theme.S(86), Width - Theme.S(32), Math.Max(1, Theme.S(1)));
            // footer
            var ver = $"Launcher v{AppVersion.Launcher}";
            TextRenderer.DrawText(g, ver, Theme.Font(8.5f), new Rectangle(Theme.S(18), Height - Theme.S(34), Width - Theme.S(24), Theme.S(20)), Theme.Muted,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }
    }
}

/// <summary>One sidebar entry: icon + label; selected = red bar with a glow.</summary>
sealed class NavItem : Control
{
    readonly string glyph;
    bool hover, selected;

    public NavItem(string glyph, string title)
    {
        this.glyph = glyph;
        Text = title;
        AccessibleName = title;
        AccessibleRole = AccessibleRole.PageTab;
        TabStop = true;
        Cursor = Cursors.Hand;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                 ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
    }

    public bool Selected { get => selected; set { if (selected != value) { selected = value; Invalidate(); } } }

    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
    protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
    protected override void OnMouseDown(MouseEventArgs e) { Focus(); base.OnMouseDown(e); }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode is Keys.Enter or Keys.Space) { OnClick(EventArgs.Empty); e.Handled = true; }
        base.OnKeyDown(e);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Side);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
        if (selected || hover)
        {
            using var path = Theme.Round(r, Theme.S(9));
            if (selected)
            {
                using var b = new LinearGradientBrush(r, Color.FromArgb(120, Theme.Primary), Color.FromArgb(30, Theme.Accent), LinearGradientMode.Horizontal);
                g.FillPath(b, path);
            }
            else using (var b = new SolidBrush(Theme.Surface)) g.FillPath(b, path);
        }
        if (selected)
        {
            float bw = Theme.S(4), bh = Height * 0.56f, by = (Height - bh) / 2;
            for (int i = 4; i >= 1; i--)
            {
                using var gp = Theme.Round(new RectangleF(-i * 1.5f, by - i, bw + i * 3, bh + i * 2), bw);
                using var gb = new SolidBrush(Color.FromArgb(28, Theme.Accent));
                g.FillPath(gb, gp);
            }
            using var bar = Theme.Round(new RectangleF(0, by, bw, bh), bw / 2);
            using var bb = new SolidBrush(Theme.Accent);
            g.FillPath(bb, bar);
        }
        if (Focused && ShowFocusCues)
        {
            using var fp = Theme.Round(RectangleF.Inflate(r, -1, -1), Theme.S(9));
            using var pen = new Pen(Color.FromArgb(150, Theme.Ink), 1) { DashStyle = DashStyle.Dot };
            g.DrawPath(pen, fp);
        }
        var fore = selected ? Color.White : hover ? Theme.Ink : Theme.Muted;
        var icon = selected ? Theme.AccentText : hover ? Theme.Ink : Theme.Muted;
        int ix = Theme.S(16), iw = Theme.S(24);
        TextRenderer.DrawText(g, glyph, Theme.Icon(12.5f), new Rectangle(ix, 0, iw, Height), icon,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
        TextRenderer.DrawText(g, Text, selected ? Theme.Font(10.5f, true) : Theme.Font(10.5f), new Rectangle(ix + iw + Theme.S(12), 0, Width - ix - iw - Theme.S(14), Height), fore,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
    }
}

/// <summary>Big page title with a short red underline.</summary>
sealed class PageTitle : Control
{
    public PageTitle(string title)
    {
        Text = title;
        Dock = DockStyle.Top;
        Height = Theme.S(74);
        BackColor = Theme.Bg;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        var f = Theme.Display(20f);
        var sz = TextRenderer.MeasureText(Text, f);
        int x = Theme.S(26), y = Theme.S(16);
        TextRenderer.DrawText(g, Text, f, new Point(x, y), Theme.Ink, TextFormatFlags.NoPadding);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var line = new RectangleF(x + Theme.S(1), y + sz.Height + Theme.S(2), Theme.S(56), Theme.S(3));
        using var b = new LinearGradientBrush(new RectangleF(line.X - 1, line.Y, line.Width + 2, line.Height), Theme.Accent, Theme.Primary, LinearGradientMode.Horizontal);
        using var p = Theme.Round(line, line.Height / 2);
        g.FillPath(b, p);
    }
}

/// <summary>Glove artwork shared by the banner and the sidebar mark: two PNGs embedded in the exe (ComfyUI-made, transparent background).</summary>
static class Art
{
    static Bitmap? banner, side;
    static Bitmap Load(string name)
    {
        using var s = typeof(Art).Assembly.GetManifestResourceStream(name)!;
        using var raw = new Bitmap(s);
        return new Bitmap(raw);   // detach from the stream
    }
    /// <summary>Two raised fists, for the Play banner.</summary>
    public static Bitmap Banner => banner ??= Load("Art.banner-gloves.png");
    /// <summary>One glove, for the sidebar mark.</summary>
    public static Bitmap Side => side ??= Load("Art.sidebar-glove.png");

    /// <summary>Draws bmp centred on c, scaled to height h (width keeps the aspect), high-quality interpolation.</summary>
    public static void Draw(Graphics g, Bitmap bmp, PointF c, float h)
    {
        float w = h * bmp.Width / bmp.Height;
        var st = g.Save();
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.CompositingQuality = CompositingQuality.HighQuality;
        using var ia = new System.Drawing.Imaging.ImageAttributes();
        ia.SetWrapMode(WrapMode.TileFlipXY);   // no edge fringe from the border pixels
        float x0 = c.X - w / 2, y0 = c.Y - h / 2;
        g.DrawImage(bmp, new[] { new PointF(x0, y0), new PointF(x0 + w, y0), new PointF(x0, y0 + h) }, new RectangleF(0, 0, bmp.Width, bmp.Height), GraphicsUnit.Pixel, ia);
        g.Restore(st);
    }
}

/// <summary>Header on the Play page: gradient, glow, cage fence mesh, the glove art and the title in heavy italic type.</summary>
public sealed class PlayBanner : Control
{
    public PlayBanner()
    {
        Dock = DockStyle.Top;
        Height = Theme.S(176);
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.AntiAlias;
        var all = new RectangleF(0, 0, Width, Height);
        if (Width < 2 || Height < 2) return;

        using (var bg = new LinearGradientBrush(all, Theme.Mix(Theme.Primary, Theme.Side, 0.72f), Theme.Side, 15f)) g.FillRectangle(bg, all);

        Glow(g, new PointF(Width * 0.80f, Height * 0.42f), Math.Max(Width * 0.34f, Theme.S(160)), Color.FromArgb(130, Theme.Accent));
        Glow(g, new PointF(Width * 0.10f, Height * 1.05f), Theme.S(180), Color.FromArgb(70, Theme.PrimaryDown));

        // cage fence: diagonal mesh across the right half
        var clip = g.Save();
        g.SetClip(new RectangleF(Width * 0.5f, 0, Width * 0.5f, Height));
        using (var mesh = new Pen(Color.FromArgb(24, Color.White), Math.Max(1f, Theme.S(1))))
        {
            float step = Theme.S(24);
            for (float x = Width * 0.5f - Height; x < Width + Height; x += step)
            {
                g.DrawLine(mesh, x, Height, x + Height, 0);
                g.DrawLine(mesh, x, 0, x + Height, Height);
            }
        }
        g.Restore(clip);

        // glove pair at the right
        Art.Draw(g, Art.Banner, new PointF(Width - Height * 0.80f, Height * 0.5f), Height * 0.86f);

        // title
        var fam = Theme.Heavy(10f, FontStyle.Italic).FontFamily;
        float em = Theme.S(40), emN = Theme.S(58);
        float x0 = Theme.S(28), y0t = Theme.S(30);
        using var sf = new StringFormat(StringFormat.GenericTypographic);
        using var t1 = new GraphicsPath();
        t1.AddString("UNDISPUTED", fam, (int)FontStyle.Italic, em, new PointF(x0, y0t - Theme.S(8)), sf);
        var b1 = t1.GetBounds();
        using var t2 = new GraphicsPath();
        t2.AddString("ULTIMATE EDITION", fam, (int)FontStyle.Italic, emN * 0.45f, new PointF(x0 + Theme.S(4), b1.Bottom + Theme.S(4)), sf);
        var b2 = t2.GetBounds();

        for (int i = 6; i >= 1; i--)   // red glow behind the letters
        {
            using var gp = new Pen(Color.FromArgb(16, Theme.Accent), Theme.S(i * 2.2f)) { LineJoin = LineJoin.Round };
            g.DrawPath(gp, t1);
            g.DrawPath(gp, t2);
        }
        using (var sh = new SolidBrush(Color.FromArgb(120, Theme.Side)))
        {
            g.TranslateTransform(Theme.S(2), Theme.S(3));
            g.FillPath(sh, t1); g.FillPath(sh, t2);
            g.ResetTransform();
        }
        using (var f1 = new LinearGradientBrush(new RectangleF(b1.X, b1.Y - 1, b1.Width, b1.Height + 2), Color.White, Theme.Mix(Theme.AccentText, Color.White, 0.35f), LinearGradientMode.Vertical))
            g.FillPath(f1, t1);
        using (var f2 = new LinearGradientBrush(new RectangleF(b2.X, b2.Y - 1, b2.Width, b2.Height + 2), Theme.Accent, Theme.PrimaryDown, LinearGradientMode.Vertical))
            g.FillPath(f2, t2);
        using (var o2 = new Pen(Color.White, Theme.S(1.6f)) { LineJoin = LineJoin.Round }) g.DrawPath(o2, t2);

        // tagline: letter-spaced small caps with a thin gold rule above
        float ty = Math.Max(b1.Bottom, b2.Bottom) + Theme.S(14);
        using (var rule = new SolidBrush(Theme.Gold)) g.FillRectangle(rule, x0 + Theme.S(2), ty - Theme.S(7), Theme.S(56), Math.Max(1, Theme.S(2)));
        var tag = "R E C O M P   ·   P C   L A U N C H E R";
        TextRenderer.DrawText(g, tag, Theme.Font(8.5f, true), new Point((int)x0 + Theme.S(2), (int)ty), Theme.Gold, TextFormatFlags.NoPadding);
    }

    static void Glow(Graphics g, PointF c, float r, Color col)
    {
        using var p = new GraphicsPath();
        p.AddEllipse(c.X - r, c.Y - r, r * 2, r * 2);
        using var b = new PathGradientBrush(p) { CenterColor = col, SurroundColors = new[] { Color.FromArgb(0, col) }, CenterPoint = c };
        g.FillPath(b, p);
    }
}

/// <summary>The launcher's own message box, in the theme. Same arguments and results as DarkBox.Show.</summary>
public static class DarkBox
{
    public static DialogResult Show(string text, string caption, MessageBoxButtons buttons = MessageBoxButtons.OK, MessageBoxIcon icon = MessageBoxIcon.None)
    {
        var owner = Form.ActiveForm is { Visible: true } a ? a : Application.OpenForms.Cast<Form>().FirstOrDefault(f => f.Visible);
        using var f = Create(text, caption, buttons, icon, owner != null);
        return f.ShowDialog(owner);
    }

    public static Form Create(string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon, bool hasOwner = true)
    {
        var f = new Form
        {
            Text = caption, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false,
            ShowInTaskbar = !hasOwner, StartPosition = hasOwner ? FormStartPosition.CenterParent : FormStartPosition.CenterScreen,
            KeyPreview = true, AutoScaleMode = AutoScaleMode.None, TopMost = !hasOwner,
        };
        Theme.DarkWindow(f);

        (string glyph, Color col) = icon switch
        {
            MessageBoxIcon.Question => ("\uE9CE", Theme.Gold),
            MessageBoxIcon.Warning => ("\uE7BA", Theme.AccentText),
            MessageBoxIcon.Error => ("\uEA39", Theme.AccentText),
            MessageBoxIcon.Information => ("\uE946", Theme.Gold),
            _ => ("", Theme.Ink),
        };
        int pad = Theme.S(24), iconW = glyph.Length > 0 ? Theme.S(44) : 0, gap = glyph.Length > 0 ? Theme.S(16) : 0;
        int maxW = Theme.S(560), maxH = Theme.S(420);
        var flags = TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl | TextFormatFlags.NoPrefix;
        var sz = TextRenderer.MeasureText(text, Theme.Body, new Size(maxW, int.MaxValue), flags);
        int msgW = Math.Max(sz.Width + Theme.S(4), Theme.S(240)), msgH = sz.Height;
        bool scroll = msgH > maxH;

        var msg = new Label { Text = text, AutoSize = false, UseMnemonic = false, ForeColor = Theme.Ink, Font = Theme.Body, Size = new Size(msgW, msgH) };
        Control msgHost = msg;
        if (scroll)
        {
            var sp = new Panel { AutoScroll = true, Size = new Size(msgW + SystemInformation.VerticalScrollBarWidth + Theme.S(4), maxH), BackColor = Theme.Bg };
            sp.Controls.Add(msg);
            Theme.DarkScroll(sp);
            msgHost = sp;
            msgH = maxH;
        }
        int bodyH = Math.Max(msgH, iconW) + pad * 2;
        msgHost.Location = new Point(pad + iconW + gap, pad);
        f.Controls.Add(msgHost);
        if (glyph.Length > 0)
            f.Controls.Add(new GlyphBadge(glyph, col) { Location = new Point(pad, pad - Theme.S(2)), Size = new Size(iconW, iconW) });

        var foot = new Panel { Dock = DockStyle.Bottom, Height = Theme.S(62), BackColor = Theme.Side };
        var row = new FlowLayoutPanel { Dock = DockStyle.Right, FlowDirection = FlowDirection.RightToLeft, AutoSize = true, WrapContents = false,
                                        Padding = new Padding(0, Theme.S(13), Theme.S(16), 0), BackColor = Theme.Side };
        var spec = buttons switch
        {
            MessageBoxButtons.OKCancel => new[] { ("OK", DialogResult.OK), ("Cancel", DialogResult.Cancel) },
            MessageBoxButtons.YesNo => new[] { ("Yes", DialogResult.Yes), ("No", DialogResult.No) },
            MessageBoxButtons.YesNoCancel => new[] { ("Yes", DialogResult.Yes), ("No", DialogResult.No), ("Cancel", DialogResult.Cancel) },
            MessageBoxButtons.RetryCancel => new[] { ("Retry", DialogResult.Retry), ("Cancel", DialogResult.Cancel) },
            _ => new[] { ("OK", DialogResult.OK) },
        };
        var made = new List<FlatButton>();
        foreach (var (label, res) in spec)
        {
            var b = new FlatButton
            {
                Text = label, DialogResult = res, Kind = made.Count == 0 ? BtnKind.Primary : BtnKind.Normal,
                Size = new Size(Theme.S(96), Theme.S(36)), Margin = new Padding(Theme.S(8), 0, 0, 0),
            };
            made.Add(b);
        }
        for (int i = made.Count - 1; i >= 0; i--) row.Controls.Add(made[i]);   // RightToLeft: last added = leftmost
        foot.Controls.Add(row);
        f.Controls.Add(foot);
        f.AcceptButton = made[0];
        f.CancelButton = made.Count > 1 ? made[^1] : made[0];
        f.ActiveControl = made[0];
        int btnW = made.Count * Theme.S(104) + Theme.S(32);
        f.ClientSize = new Size(Math.Max(pad + iconW + gap + msgHost.Width + pad, btnW), bodyH + foot.Height);
        f.KeyDown += (_, e) =>
        {
            if (e.Control && e.KeyCode == Keys.C) { try { Clipboard.SetText(caption + "\r\n\r\n" + text); } catch { } e.Handled = true; }
        };
        return f;
    }

    sealed class GlyphBadge : Control
    {
        readonly string glyph; readonly Color col;
        public GlyphBadge(string glyph, Color col)
        {
            this.glyph = glyph; this.col = col;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Theme.Bg);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var b = new SolidBrush(Color.FromArgb(40, col))) g.FillEllipse(b, 0, 0, Width - 1, Height - 1);
            using (var p = new Pen(Color.FromArgb(120, col), Theme.S(1.2f))) g.DrawEllipse(p, 0.5f, 0.5f, Width - 2, Height - 2);
            TextRenderer.DrawText(g, glyph, Theme.Icon(16f), new Rectangle(0, 0, Width, Height), col,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
        }
    }
}
