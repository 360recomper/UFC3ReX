using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace UFC3Launcher;

/// <summary>
/// Mod pack = a zip with modpack.json at the root and mods\ mirroring the game's mods folder. Install extracts
/// mods\* into the game's mods folder and writes mods\modpack-installed.json (the exact file list), so Uninstall
/// removes exactly those files. A file the pack overwrites is moved to mods\.modpack-backup and put back on uninstall.
/// </summary>
public static class ModPack
{
    public class Manifest
    {
        public string Name { get; set; } = "";
        public string Version { get; set; } = "";
        public string Game { get; set; } = "";
        [JsonPropertyName("requires_launcher")] public string RequiresLauncher { get; set; } = "";
        public string Description { get; set; } = "";
        public int Files { get; set; }
    }

    public class Record
    {
        public string Name { get; set; } = "";
        public string Version { get; set; } = "";
        public string Installed { get; set; } = "";
        public List<string> Files { get; set; } = new();
        /// <summary>Files that existed before the install and were moved to the backup folder.</summary>
        public List<string> Backed { get; set; } = new();
    }

    /// <summary>A pack that passed the checks: what it is and which zip entries to extract.</summary>
    public class Checked
    {
        public Manifest Info = new();
        public List<(ZipArchiveEntry Entry, string Rel)> Entries = new();
        public List<string> Warnings = new();
    }

    static readonly JsonSerializerOptions Json = new()
    {
        PropertyNameCaseInsensitive = true, WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    };
    const string RecordName = "modpack-installed.json";
    const string BackupName = ".modpack-backup";
    public static string RecordPath => Path.Combine(Paths.Mods, RecordName);
    static string BackupDir => Path.Combine(Paths.Mods, BackupName);

    public static Record? Installed()
    {
        try { return JsonSerializer.Deserialize<Record>(File.ReadAllText(RecordPath), Json); }
        catch { return null; }
    }

    /// <summary>Path of a "mods/..." zip entry relative to the mods folder, "" for a folder entry, or null when the
    /// entry is not under mods/. Throws on anything that could leave the mods folder (zip-slip).</summary>
    public static string? SafeRel(string entryName)
    {
        var n = entryName.Replace('\\', '/');
        if (n.Split('/').Contains("..") || n.Contains(':') || n.StartsWith('/'))
            throw new InvalidDataException("Unsafe path in the pack: " + entryName);
        if (!n.StartsWith("mods/", StringComparison.OrdinalIgnoreCase)) return null;
        var rel = n[5..];
        if (rel.Length == 0 || rel.EndsWith('/')) return "";
        var parts = rel.Split('/');
        if (parts.Any(p => p == "." || p == ""))
            throw new InvalidDataException("Unsafe path in the pack: " + entryName);
        var full = Path.GetFullPath(Path.Combine(Paths.Mods, rel));
        var root = Path.GetFullPath(Paths.Mods) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Unsafe path in the pack: " + entryName);
        // the launcher's own bookkeeping is not a pack's to overwrite
        if (parts[0].Equals(BackupName, StringComparison.OrdinalIgnoreCase) || rel.Equals(RecordName, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Unsafe path in the pack: " + entryName);
        return string.Join('/', parts);
    }

    /// <summary>Reads and checks the zip; throws InvalidDataException with a plain message when it can't be installed.</summary>
    public static Checked Check(ZipArchive zip)
    {
        var mf = zip.GetEntry("modpack.json") ?? throw new InvalidDataException("This is not a UFC 3 mod pack (modpack.json is missing).");
        var res = new Checked();
        try
        {
            using var s = mf.Open();
            res.Info = JsonSerializer.Deserialize<Manifest>(s, Json) ?? throw new InvalidDataException("empty");
        }
        catch (Exception ex) when (ex is JsonException or InvalidDataException)
        { throw new InvalidDataException("modpack.json is damaged or not valid."); }
        var m = res.Info;
        if (!m.Game.Equals("UFC3", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"This mod pack is for \"{(m.Game == "" ? "an unknown game" : m.Game)}\", not UFC 3.");
        if (string.IsNullOrWhiteSpace(m.Name)) throw new InvalidDataException("modpack.json has no name.");
        foreach (var e in zip.Entries)
        {
            var rel = SafeRel(e.FullName);
            if (!string.IsNullOrEmpty(rel)) res.Entries.Add((e, rel));
        }
        if (res.Entries.Count == 0) throw new InvalidDataException("The pack has no files in mods\\.");
        if (m.Files > 0 && m.Files != res.Entries.Count)
            throw new InvalidDataException($"The pack should hold {m.Files} files but has {res.Entries.Count}: the download is incomplete or damaged.");
        if (m.RequiresLauncher != "" && AppVersion.Newer(m.RequiresLauncher, AppVersion.Launcher))
            res.Warnings.Add($"This pack needs launcher {m.RequiresLauncher} or newer (this is {AppVersion.Launcher}). Some of it may not work.");
        return res;
    }

    /// <summary>Checks a zip file and returns what it holds (for showing before install).</summary>
    public static (Manifest Info, int Files, List<string> Warnings) Inspect(string zipPath)
    {
        using var z = ZipFile.OpenRead(zipPath);
        var c = Check(z);
        return (c.Info, c.Entries.Count, c.Warnings);
    }

    /// <summary>Installs a pack (replacing an installed one first). Returns a short summary.</summary>
    public static string Install(string zipPath, Action<string, double> progress, CancellationToken ct)
    {
        using var zip = ZipFile.OpenRead(zipPath);
        var chk = Check(zip);
        if (Installed() != null) Uninstall(progress);
        Directory.CreateDirectory(Paths.Mods);
        var rec = new Record { Name = chk.Info.Name, Version = chk.Info.Version, Installed = DateTime.Now.ToString("yyyy-MM-dd HH:mm") };
        File.WriteAllText(RecordPath, JsonSerializer.Serialize(rec, Json));    // before the first file, so a crash can be undone
        int i = 0;
        try
        {
            foreach (var (e, rel) in chk.Entries)
            {
                ct.ThrowIfCancellationRequested();
                progress("Installing " + rel, (double)i++ / chk.Entries.Count);
                var dst = Path.Combine(Paths.Mods, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                if (File.Exists(dst))
                {
                    var bk = Path.Combine(BackupDir, rel);
                    Directory.CreateDirectory(Path.GetDirectoryName(bk)!);
                    File.Move(dst, bk, true);
                    rec.Backed.Add(rel);
                }
                var tmp = dst + ".packtmp";
                e.ExtractToFile(tmp, true);
                File.Move(tmp, dst, true);
                rec.Files.Add(rel);
            }
        }
        finally { File.WriteAllText(RecordPath, JsonSerializer.Serialize(rec, Json)); }
        progress("Done", 1);
        return $"Installed \"{rec.Name}\" {rec.Version}: {rec.Files.Count} files.";
    }

    /// <summary>Removes exactly the files the install recorded (and puts back what it replaced).</summary>
    public static string Uninstall(Action<string, double> progress)
    {
        var rec = Installed() ?? throw new InvalidOperationException("No mod pack is installed.");
        int i = 0;
        foreach (var rel in rec.Files)
        {
            progress("Removing " + rel, (double)i++ / Math.Max(1, rec.Files.Count));
            var dst = Path.Combine(Paths.Mods, SafeRel("mods/" + rel) ?? throw new InvalidDataException("bad record"));
            if (File.Exists(dst)) File.Delete(dst);
            var bk = Path.Combine(BackupDir, rel);
            if (rec.Backed.Contains(rel) && File.Exists(bk)) File.Move(bk, dst);
            else PruneEmptyDirs(Path.GetDirectoryName(dst)!);
        }
        if (Directory.Exists(BackupDir)) try { Directory.Delete(BackupDir, true); } catch { }
        File.Delete(RecordPath);
        return $"Uninstalled \"{rec.Name}\": {rec.Files.Count} files removed.";
    }

    static void PruneEmptyDirs(string dir)
    {
        var root = Path.GetFullPath(Paths.Mods).TrimEnd('\\');
        while (Path.GetFullPath(dir).TrimEnd('\\').Length > root.Length && Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any())
        {
            Directory.Delete(dir);
            dir = Path.GetDirectoryName(dir)!;
        }
    }
}
