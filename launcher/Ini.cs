using System.Text;

namespace UFC3Launcher;

/// <summary>Edits single keys of an ini file in place: comments, order, other keys and line endings stay as they are
/// (OptiScaler.ini is a 60 KB commented file with mixed LF/CRLF; ReShade.ini is also written by ReShade itself).
/// Lines are split on LF only and each keeps its own CR.</summary>
public class Ini
{
    readonly string path;
    readonly List<string> lines;
    /// <summary>"\r" when the file uses CRLF anywhere: added to lines this class inserts.</summary>
    readonly string cr;

    Ini(string path, string text)
    {
        this.path = path;
        cr = text.Contains("\r\n") ? "\r" : "";
        lines = text.Split('\n').ToList();
        if (lines.Count > 0 && lines[^1] == "") lines.RemoveAt(lines.Count - 1);
    }

    public static Ini Load(string path) => new(path, File.Exists(path) ? File.ReadAllText(path) : "");

    static bool IsSection(string l, out string name)
    {
        var t = l.Trim();
        name = t.Length > 2 && t[0] == '[' && t[^1] == ']' ? t[1..^1] : "";
        return name.Length > 0;
    }

    /// <summary>Index of the section header, or -1.</summary>
    int Section(string section)
    {
        for (int i = 0; i < lines.Count; i++)
            if (IsSection(lines[i], out var n) && n.Equals(section, StringComparison.OrdinalIgnoreCase)) return i;
        return -1;
    }

    /// <summary>Section "" = keys before the first section (dlss5-feed.cfg has no sections).</summary>
    int Key(string section, string key)
    {
        int s = section == "" ? -1 : Section(section);
        if (s < 0 && section != "") return -1;
        for (int i = s + 1; i < lines.Count && !IsSection(lines[i], out _); i++)
        {
            var t = lines[i].TrimStart();
            int eq = t.IndexOf('=');
            if (eq > 0 && !t.StartsWith(';') && !t.StartsWith('#') && t[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) return i;
        }
        return -1;
    }

    public string? Get(string section, string key)
    {
        int i = Key(section, key);
        return i < 0 ? null : lines[i][(lines[i].IndexOf('=') + 1)..].Trim();
    }

    public void Set(string section, string key, string value)
    {
        int i = Key(section, key);
        if (i >= 0)
        {
            var l = lines[i];
            lines[i] = l[..(l.IndexOf('=') + 1)] + value + (l.EndsWith('\r') ? "\r" : "");
            return;
        }
        if (section == "") { lines.Insert(0, $"{key}={value}{cr}"); return; }
        int s = Section(section);
        if (s < 0) { if (lines.Count > 0 && lines[^1].Trim() != "") lines.Add(cr); lines.Add($"[{section}]{cr}"); s = lines.Count - 1; }
        lines.Insert(s + 1, $"{key}={value}{cr}");
    }

    public void Save() => File.WriteAllText(path, string.Join("\n", lines) + "\n", new UTF8Encoding(false));
}
