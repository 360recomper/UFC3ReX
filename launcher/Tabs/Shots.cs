using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace UFC3Launcher.Tabs;

/// <summary>
/// Hidden option for UI checks: UFC3 Launcher.exe --screenshot-all &lt;dir&gt; [100|150]. Opens the launcher window,
/// shows every page and dialog in turn and saves a PNG of each (screen capture of the window). The number simulates
/// that Windows display scale on any PC (Theme.Sim). Never starts the game and writes nothing but the PNGs.
/// Everything is rendered off-screen (-32000,-32000), never activated, no taskbar button; PNGs come from PrintWindow.
/// </summary>
static class Shots
{
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out RECT r, int size);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern bool RedrawWindow(IntPtr hwnd, IntPtr rc, IntPtr rgn, uint flags);
    static readonly Point Off = new(-32000, -32000);

    public static int Run(string dir, int pct)
    {
        Directory.CreateDirectory(dir);
        int rc = 0;
        var form = new MainForm(false, preview: true) { StartPosition = FormStartPosition.Manual, ShowInTaskbar = false, Location = Off };
        form.Shown += async (_, _) =>
        {
            try { await Go(form, dir, pct); }
            catch (Exception ex) { File.WriteAllText(Path.Combine(dir, $"{pct}-error.txt"), ex.ToString()); rc = 1; }
            form.Close();
        };
        Application.Run(form);
        return rc;
    }

    static int n;

    static async Task Go(MainForm form, string dir, int pct)
    {
        string P(string name) => Path.Combine(dir, $"{pct}-{++n:00}-{name}.png");
        var nav = form.Nav;
        await Settle(); await Settle();
        Capture(form, Path.Combine(Path.GetTempPath(), "ufc3-shot-warmup.png")); await Settle();   // throw-away: the first PrintWindow of a window comes back unpainted
        for (int i = 0; i < form.Tabs.Count; i++)
        {
            var t = form.Tabs[i];
            nav.SelectedIndex = i;
            await Settle(); await Settle();
            var slug = t.Title.ToLowerInvariant().Replace(' ', '-');
            Capture(form, P(slug));
            if (Scroller(t.View) is { } sc)
            {
                sc.AutoScrollPosition = new Point(0, sc.DisplayRectangle.Height);
                await Settle();
                Capture(form, P(slug + "-bottom"));
                sc.AutoScrollPosition = Point.Empty;
            }
            if (t is PlayTab play) { play.PreviewUpdate(); await Settle(); Capture(form, P("play-update")); }
            if (t is SetupTab setup) { setup.PreviewBusy(); await Settle(); Capture(form, P("setup-importing")); }
        }

        var boxes = new (string Name, string Text, string Caption, MessageBoxButtons B, MessageBoxIcon I)[]
        {
            ("box-update", "Download v9.9 (212 MB) and replace the changed program files?\n\nYour settings (ufc3.toml), saves, mods and game files are not touched. Replaced files are kept in update-backups.", "Update", MessageBoxButtons.OKCancel, MessageBoxIcon.Question),
            ("box-cache-fallback", NativeRenderer.FallbackText("the shader cache could not be built."), "Undisputed Launcher", MessageBoxButtons.OK, MessageBoxIcon.Warning),
            ("box-replace-pack", "\"Sample pack\" is installed. It is removed first and replaced by this pack. Continue?", "Undisputed Launcher", MessageBoxButtons.YesNo, MessageBoxIcon.None),
            ("box-dlss-reset", "Put the DLSS 5 look, ReShade preset and F10 panel settings back to how they were after setup?", "Undisputed Launcher", MessageBoxButtons.OKCancel, MessageBoxIcon.None),
            ("box-updated", "Updated to v9.9. The replaced files are in the update-backups folder.", "Undisputed Launcher", MessageBoxButtons.OK, MessageBoxIcon.None),
            ("box-long", string.Join("\n\n", Enumerable.Range(1, 14).Select(k => $"Line {k}: a long message to check the scrolling message box with many lines of text in it.")),
                "Undisputed Launcher", MessageBoxButtons.OK, MessageBoxIcon.Information),
        };
        foreach (var b in boxes)
        {
            using var f = DarkBox.Create(b.Text, b.Caption, b.B, b.I);
            ShowOff(f);
            await Settle();
            Capture(f, P(b.Name));
            f.Close();
        }
    }

    /// <summary>Shown off-screen without taking focus from whatever the user is doing.</summary>
    static void ShowOff(Form d)
    {
        d.ShowInTaskbar = false;
        d.TopMost = false;
        d.StartPosition = FormStartPosition.Manual;
        d.Location = Off;
        ShowWindow(d.Handle, 4);   // SW_SHOWNOACTIVATE
    }

    static async Task Settle(int ms = 1000)
    {
        Application.DoEvents();
        await Task.Delay(ms);
        Application.DoEvents();
    }

    static IEnumerable<Control> All(Control c)
    {
        foreach (Control k in c.Controls) { yield return k; foreach (var x in All(k)) yield return x; }
    }

    /// <summary>The page's scrolling column when it has more content than fits.</summary>
    static ScrollableControl? Scroller(Control view) =>
        new[] { view }.Concat(All(view)).OfType<ScrollableControl>().FirstOrDefault(s => s.AutoScroll && s.Visible && s.VerticalScroll.Visible);

    /// <summary>The first PrintWindow after a page or dialog change can come back half painted off-screen, so print twice and keep the second.</summary>
    static void Capture(Form f, string path)
    {
        CaptureOnce(f, Path.Combine(Path.GetTempPath(), "ufc3-shot-warmup.png"));
        System.Threading.Thread.Sleep(400); Application.DoEvents();
        CaptureOnce(f, path);
    }

    static void CaptureOnce(Form f, string path)
    {
        for (int i = 0; i < 3; i++) { f.Invalidate(true); f.Update(); f.Refresh(); RedrawWindow(f.Handle, IntPtr.Zero, IntPtr.Zero, 0x0001 | 0x0004 | 0x0080 | 0x0100 | 0x0400); Application.DoEvents(); System.Threading.Thread.Sleep(150); Application.DoEvents(); }
        GetWindowRect(f.Handle, out var w);
        using var full = new Bitmap(w.R - w.L, w.B - w.T, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(full))
        {
            var hdc = g.GetHdc();
            try { PrintWindow(f.Handle, hdc, 2); }   // PW_RENDERFULLCONTENT: works for off-screen windows
            finally { g.ReleaseHdc(hdc); }
        }
        // crop the invisible resize borders to the visible frame (DWMWA_EXTENDED_FRAME_BOUNDS)
        var crop = new Rectangle(0, 0, full.Width, full.Height);
        if (DwmGetWindowAttribute(f.Handle, 9, out var rc, Marshal.SizeOf<RECT>()) == 0)
        {
            var vis = Rectangle.Intersect(crop, new Rectangle(rc.L - w.L, rc.T - w.T, rc.R - rc.L, rc.B - rc.T));
            if (vis.Width > 0 && vis.Height > 0) crop = vis;
        }
        using var bmp = full.Clone(crop, PixelFormat.Format32bppArgb);
        bmp.Save(path, ImageFormat.Png);
    }
}
