using System.Text;

namespace UFC3Launcher;

/// <summary>Minimal key = value editor for ufc3.toml: only the keys it is asked to set change, every other line
/// (comments, paths, the F4 menu's settings) stays as it is.</summary>
public class Toml
{
    readonly List<string> lines;
    public Toml() => lines = File.Exists(Paths.Toml) ? File.ReadAllLines(Paths.Toml).ToList() : new();

    int Find(string key) =>
        lines.FindIndex(l =>
        {
            var t = l.TrimStart();
            return !t.StartsWith('#') && t.StartsWith(key) && t[key.Length..].TrimStart().StartsWith('=');
        });

    public string? Get(string key)
    {
        int i = Find(key);
        if (i < 0) return null;
        var v = lines[i][(lines[i].IndexOf('=') + 1)..].Trim();
        int hash = v.IndexOf(" #", StringComparison.Ordinal);
        if (hash >= 0 && !v.StartsWith('"')) v = v[..hash].Trim();
        return v.Trim('"');
    }

    public int GetInt(string key, int def) => int.TryParse(Get(key), out var v) ? v : def;
    public bool GetBool(string key, bool def) => Get(key) is { } s ? s == "true" : def;
    public string GetStr(string key, string def) => Get(key) ?? def;

    public void Set(string key, string rawValue)
    {
        int i = Find(key);
        var line = $"{key} = {rawValue}";
        if (i >= 0) lines[i] = line; else lines.Add(line);
    }
    public void Set(string key, int v) => Set(key, v.ToString());
    public void Set(string key, bool v) => Set(key, v ? "true" : "false");
    public void SetStr(string key, string v) => Set(key, "\"" + v + "\"");

    public void Save() => File.WriteAllText(Paths.Toml, string.Join(Environment.NewLine, lines) + Environment.NewLine, new UTF8Encoding(false));
}
