using System.Globalization;
using System.Text.RegularExpressions;

namespace UFC3Launcher;

/// <summary>
/// The DLSS 5 look = OptiScaler-NR's [DlssNr] keys in dlss5\host64\OptiScaler.ini. That file is the one place it is
/// saved: the launcher's DLSS 5 tab edits it directly, OptiScaler's own "Save INI" (F10 panel) writes it too, and
/// style/preset/strength changes made in the F10 panel without "Save INI" are copied into it when the game exits
/// (<see cref="KeepInGameChanges"/>), from the "NR diagnostic creation" lines OptiScaler logs at every model rebuild.
/// </summary>
public static class Dlss5Look
{
    public static string IniPath => Path.Combine(Dlss5.Dir, @"host64\OptiScaler.ini");
    /// <summary>OptiScaler.ini as setup left it; "Reset" copies it back.</summary>
    public static string DefaultIniPath => IniPath + ".default";
    static string LogPath => Path.Combine(Dlss5.Dir, @"host64\OptiScaler.log");
    const string Sec = "DlssNr";

    public record Setting(string Key, string Label, decimal Min, decimal Max, decimal Step, decimal Default, string Help);

    public static readonly string[] Styles = { "Standard", "Natural", "Cinematic" };
    public static readonly string[] Presets = { "Default", "Preset 1", "Preset 2", "Preset 3" };

    public static readonly Setting[] Sliders =
    {
        new("Intensity", "Intensity", 0, 2, 0.05m, 1, "Overall strength of the relighting (1.00-1.05 is the usual pick)."),
        new("TransferStrength", "Detail strength", 0, 3, 0.05m, 1, "How far the picture moves toward the model's (0 = off, 1 = the model's picture)."),
        new("LocalStructure", "Local structure", 0, 2, 0.05m, 1, "Fine shape/contrast the model adds."),
        new("LocalTone", "Local tone", 0, 2, 0.05m, 1, "Local light and shadow the model adds."),
        new("Passes", "Passes", 1, 3, 1, 1, "Model layers per frame: 2 and 3 are stronger and cost 2x/3x the model time. Passes 2 hung the GPU once on the RTX 40 runtime; 1 is the safe pick."),
        new("WorkingScale", "Working scale", 0.25m, 1, 0.05m, 1, "Fraction of the frame the model works at: lower is faster, 0.5 holds 60 fps at 4K."),
    };

    static Ini Load() => Ini.Load(IniPath);
    public static bool Present => File.Exists(IniPath);

    static int Int(string? v, int def) => int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i) ? i : def;
    static decimal Dec(string? v, decimal def) => decimal.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : def;
    static string Str(decimal d) => d.ToString("0.0##", CultureInfo.InvariantCulture);

    public static int Style => Math.Clamp(Int(Load().Get(Sec, "Style"), 0), 0, Styles.Length - 1);
    public static int Preset => Math.Clamp(Int(Load().Get(Sec, "Preset"), 0), 0, Presets.Length - 1);
    public static decimal Get(Setting s) => Math.Clamp(Dec(Load().Get(Sec, s.Key), s.Default), s.Min, s.Max);

    static void Write(string key, string value)
    {
        var ini = Load();
        ini.Set(Sec, key, value);
        ini.Save();
    }

    public static void SetStyle(int v) => Write("Style", v.ToString(CultureInfo.InvariantCulture));
    public static void SetPreset(int v) => Write("Preset", v.ToString(CultureInfo.InvariantCulture));
    public static void Set(Setting s, decimal v) => Write(s.Key, s.Step >= 1 ? ((int)v).ToString(CultureInfo.InvariantCulture) : Str(v));

    /// <summary>Keeps a copy of OptiScaler.ini as setup left it, once.</summary>
    public static void BackupDefault()
    {
        if (Present && !File.Exists(DefaultIniPath)) File.Copy(IniPath, DefaultIniPath);
    }

    public static void ResetToDefault()
    {
        if (File.Exists(DefaultIniPath)) File.Copy(DefaultIniPath, IniPath, true);
    }

    static readonly Regex Creation = new(
        @"NR diagnostic creation: \d+x\d+, preset=(?<Preset>-?\d+), style=(?<Style>-?\d+), intensity=(?<Intensity>[-\d.]+), " +
        @"structure=(?<LocalStructure>[-\d.]+), tone=(?<LocalTone>[-\d.]+), skin=(?<SkinStructure>[-\d.]+), autoMask=(?<AutoMask>true|false)",
        RegexOptions.Compiled);

    /// <summary>
    /// After a DLSS 5 run: model settings changed in the F10 panel are written to OptiScaler.ini, so the next run
    /// starts with them even though "Save INI" was not pressed. Only values that changed during the run are written
    /// (first vs last model build in OptiScaler.log). Skipped when OptiScaler.ini itself was saved during the run.
    /// Returns what was kept, for the log.
    /// </summary>
    public static string KeepInGameChanges(DateTime runStart)
    {
        if (!Present || !File.Exists(LogPath)) return "no OptiScaler.ini/log";
        if (File.GetLastWriteTime(IniPath) > runStart) return "OptiScaler.ini was saved in game; left as saved";
        if (File.GetLastWriteTime(LogPath) < runStart) return "OptiScaler.log is from an older run";
        string text;
        using (var fs = new FileStream(LogPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (var sr = new StreamReader(fs)) text = sr.ReadToEnd();
        var ms = Creation.Matches(text);
        if (ms.Count < 2) return "no model change in game";
        Match first = ms[0], last = ms[^1];
        var ini = Load();
        var kept = new List<string>();
        foreach (var key in new[] { "Preset", "Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask" })
        {
            string a = first.Groups[key].Value, b = last.Groups[key].Value;
            if (a == b) continue;
            ini.Set(Sec, key, b);
            kept.Add($"{key}={b}");
        }
        if (kept.Count == 0) return "no model change in game";
        ini.Save();
        return "kept from the F10 panel: " + string.Join(", ", kept);
    }
}
