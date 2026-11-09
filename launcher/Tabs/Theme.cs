using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace UFC3Launcher.Tabs;

/// <summary>
/// LAUNCHER-UI look: black + red. Colours, fonts, DPI scale and the Win32 bits (dark title bar, dark
/// scrollbars). Every pixel size in the launcher goes through <see cref="S"/>; fonts through <see cref="Font"/>.
/// Body text colours meet WCAG AA (4.5:1) on <see cref="Bg"/> and <see cref="Surface"/>.
/// </summary>
static class Theme
{
    static Color C(string hex) => ColorTranslator.FromHtml(hex);

    public static readonly Color
        Bg = C("#0B0B0D"),          // page background
        Side = C("#050506"),        // sidebar, title bar, dialog footers
        Surface = C("#17171B"),     // inputs, lists, cards
        Surface2 = C("#24242A"),    // hover
        Line = C("#3A3A42"),        // borders
        Ink = C("#F4F4F6"),         // body text            (~17:1 on Bg)
        Muted = C("#B4B4BE"),       // secondary text       (~9:1 on Bg)
        Faint = C("#6E6E78"),       // disabled text only
        Accent = C("#FF2A2A"),      // accent: bars, focus, hover border
        AccentText = C("#FF5C5C"),  // red text on dark     (~6:1 on Bg)
        Primary = C("#D01212"),     // primary button fill  (white text ~5.9:1)
        PrimaryHot = C("#E62222"),
        PrimaryDown = C("#A30E0E"),
        Gold = C("#C9A24B");        // rare highlight: thin rules, tagline (~8:1 on Bg)      // gradient partner of Primary

    /// <summary>System DPI (the launcher is SystemAware).</summary>
    public static float Dpi { get; private set; } = 96;
    /// <summary>--screenshot-all: target DPI / real DPI, so 100% and 150% shots come out of one PC. 1 otherwise.</summary>
    public static float Sim { get; private set; } = 1;
    public static float Scale => Dpi / 96f * Sim;
    public static int S(float px) => (int)Math.Round(px * Scale);

    /// <param name="simPercent">Only for --screenshot-all: render as if Windows display scale were this.</param>
    public static void Init(int? simPercent = null)
    {
        try { using var g = Graphics.FromHwnd(IntPtr.Zero); Dpi = g.DpiX; } catch { Dpi = 96; }
        if (simPercent is int p and > 0) Sim = p * 96f / 100f / Dpi;
    }

    // ---------------------------------------------------------------- fonts

    static readonly HashSet<string> Families = FontFamily.Families.Select(f => f.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
    static string Pick(params string[] names) => names.FirstOrDefault(Families.Contains) ?? "Segoe UI";

    static readonly string TextFamily = Pick("Segoe UI Variable Text", "Segoe UI");
    static readonly string SemiFamily = Pick("Segoe UI Variable Text Semibold", "Segoe UI Semibold");
    static readonly string DisplayFamily = Pick("Segoe UI Variable Display Semib", "Segoe UI Semibold");
    static readonly string BlackFamily = Pick("Segoe UI Black", "Arial Black", "Segoe UI");
    public static readonly string IconFamily = Pick("Segoe Fluent Icons", "Segoe MDL2 Assets");

    static readonly Dictionary<(string, float, FontStyle), Font> fonts = new();
    static Font Get(string fam, float pt, FontStyle st = FontStyle.Regular)
    {
        var key = (fam, pt, st);
        if (!fonts.TryGetValue(key, out var f)) fonts[key] = f = new Font(fam, pt * Sim, st);
        return f;
    }

    /// <summary>Body text (point size before the screenshot simulation).</summary>
    public static Font Font(float pt = 10f, bool semibold = false) => Get(semibold ? SemiFamily : TextFamily, pt);
    public static Font Body => Font();
    public static Font Semi => Font(10f, true);
    public static Font Display(float pt) => Get(DisplayFamily, pt);
    public static Font Heavy(float pt, FontStyle st = FontStyle.Regular) => Get(BlackFamily, pt, st);
    public static Font Icon(float pt) => Get(IconFamily, pt);
    public static Font Mono(float pt) => Get(Pick("Cascadia Mono", "Consolas"), pt);

    // ---------------------------------------------------------------- drawing helpers

    public static GraphicsPath Round(RectangleF r, float rad)
    {
        var p = new GraphicsPath();
        float d = Math.Min(rad * 2, Math.Min(r.Width, r.Height));
        if (d <= 0.5f) { p.AddRectangle(r); return p; }
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    public static Color Mix(Color a, Color b, float t) => Color.FromArgb(
        (int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));

    /// <summary>The colour behind a control (walks up past transparent parents).</summary>
    public static Color BackOf(Control c)
    {
        for (var p = c.Parent; p != null; p = p.Parent)
            if (p.BackColor.A == 255) return p.BackColor;
        return Bg;
    }

    // ---------------------------------------------------------------- Win32

    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr hwnd, string? app, string? idList);

    static int Ref(Color c) => c.R | (c.G << 8) | (c.B << 16);

    /// <summary>Dark title bar (Windows 10 2004+), and on Windows 11 a black caption, light caption text and a red border.</summary>
    public static void DarkWindow(Form f)
    {
        f.BackColor = Bg;
        f.ForeColor = Ink;
        f.Font = Body;
        void Apply()
        {
            try
            {
                int on = 1;
                if (DwmSetWindowAttribute(f.Handle, 20, ref on, 4) != 0) DwmSetWindowAttribute(f.Handle, 19, ref on, 4);
                int cap = Ref(Side), txt = Ref(Ink), border = Ref(Primary);
                DwmSetWindowAttribute(f.Handle, 35, ref cap, 4);
                DwmSetWindowAttribute(f.Handle, 36, ref txt, 4);
                DwmSetWindowAttribute(f.Handle, 34, ref border, 4);
            }
            catch { /* older Windows: default title bar */ }
        }
        if (f.IsHandleCreated) Apply(); else f.HandleCreated += (_, _) => Apply();
    }

    /// <summary>Dark scrollbars on a control and on every scrollable child added now or later.</summary>
    public static void DarkScroll(Control root)
    {
        void One(Control c)
        {
            if (c is ScrollableControl or ListBox or ComboBox or TextBoxBase)
            {
                string cls = c is ComboBox ? "DarkMode_CFD" : "DarkMode_Explorer";
                if (c.IsHandleCreated) SetWindowTheme(c.Handle, cls, null);
                else c.HandleCreated += (_, _) => SetWindowTheme(c.Handle, cls, null);
            }
            foreach (Control k in c.Controls) One(k);
            c.ControlAdded += (_, e) => { if (e.Control != null) One(e.Control); };
        }
        One(root);
    }
}
