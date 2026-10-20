using System.Text.Json;

namespace UFC3Launcher;

/// <summary>All paths are relative to the folder holding the launcher exe (the play folder with ufc3.exe).</summary>
public static class Paths
{
    public static readonly string Root =
        Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory;
    public static string GameExe => Path.Combine(Root, "ufc3.exe");
    public static string Config => Path.Combine(Root, "launcher.json");

    public static string Toml => Path.Combine(Root, "ufc3.toml");
    public static bool IsInstalled => File.Exists(GameExe);

    static string TomlPath(string key, string fallback)
    {
        try { var m = new UFC3Launcher.Toml().Get(key); if (!string.IsNullOrWhiteSpace(m)) return Path.GetFullPath(m, Root); }
        catch { }
        return fallback;
    }

    /// <summary>The game files (the extracted disc): game_data_root from ufc3.toml, else assets\ next to the exe.</summary>
    public static string Assets => TomlPath("game_data_root", Path.Combine(Root, "assets"));
    /// <summary>Saves and DLC: user_data_root from ufc3.toml, else userdata\ next to the exe.</summary>
    public static string UserData => TomlPath("user_data_root", Path.Combine(Root, "userdata"));
    /// <summary>Setup is done once the disc's default.xex is in the game files.</summary>
    public static bool AssetsReady => File.Exists(Path.Combine(Assets, "default.xex"));

    /// <summary>The game's mods folder: mods_root from ufc3.toml when set, else mods\ next to the exe.</summary>
    public static string Mods
    {
        get
        {
            try { var m = new UFC3Launcher.Toml().Get("mods_root"); if (!string.IsNullOrWhiteSpace(m)) return Path.GetFullPath(m, Root); }
            catch { }
            return Path.Combine(Root, "mods");
        }
    }
}

/// <summary>launcher.json next to the exe: launcher-only state (the game's own settings live in ufc3.toml).</summary>
public class Config
{
    /// <summary>Play starts the game with DLSS 5 while DLSS 5 is set up.</summary>
    public bool PlayWithDlss5 { get; set; }
    /// <summary>resolution_scale for DLSS 5 runs (2 = 2560x1440 in, 4K out: what holds 60 fps).</summary>
    public int Dlss5Scale { get; set; } = 2;
    /// <summary>ReShade's fps counter in DLSS 5 runs.</summary>
    public bool Dlss5ShowFps { get; set; }
    /// <summary>F10 live panel in DLSS 5 runs (OptiScaler's window put over the game while it is open). Off by
    /// default: the look is set in the launcher's DLSS 5 tab, and the game stays the only window.</summary>
    public bool Dlss5LivePanel { get; set; }

    /// <summary>Ask GitHub for a newer release when the launcher starts.</summary>
    public bool CheckForUpdates { get; set; } = true;

    static readonly JsonSerializerOptions Opts = new() { WriteIndented = true };

    public static Config Load()
    {
        try { return JsonSerializer.Deserialize<Config>(File.ReadAllText(Paths.Config)) ?? new(); }
        catch { return new(); }
    }

    public void Save() => File.WriteAllText(Paths.Config, JsonSerializer.Serialize(this, Opts));
}
