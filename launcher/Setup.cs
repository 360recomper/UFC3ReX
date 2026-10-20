using System.IO.Compression;
using System.Text;

namespace UFC3Launcher;

/// <summary>First-run work: disc image -> assets\, DLC packages -> userdata\...\00000002 + headers.</summary>
public static class Setup
{
    public const uint TitleId = 0x5451087D, MediaId = 0x3E0E52AB;

    /// <summary>Checks the image is UFC Undisputed 3 (USA), then extracts it into dest (resumable).</summary>
    public static void ImportIso(string iso, string dest, IProgress<(string, double)> progress, CancellationToken ct)
    {
        progress.Report(("Reading the disc image...", 0));
        using var xgd = new Xgd(iso);
        var xex = xgd.Find("default.xex") ?? throw new InvalidDataException("default.xex not found: this is not a game disc.");
        var info = xgd.ReadXexInfo(xex);
        if (info.TitleId != TitleId)
            throw new InvalidDataException($"This is not UFC Undisputed 3 (title {info.TitleId:X8}, expected {TitleId:X8}).");
        if (info.MediaId != MediaId)
            throw new InvalidDataException($"This is a different release of UFC Undisputed 3 (media {info.MediaId:X8}). The recomp supports the USA disc (media {MediaId:X8}).");
        CheckSpace(dest, xgd);
        xgd.ExtractAll(dest, progress, ct);
        if (!File.Exists(Path.Combine(dest, "default.xex"))) throw new InvalidDataException("default.xex missing after copy.");
    }

    static void CheckSpace(string dest, Xgd xgd)
    {
        try
        {
            long have = 0;
            if (Directory.Exists(dest))
                foreach (var f in new DirectoryInfo(dest).EnumerateFiles("*", SearchOption.AllDirectories)) have += f.Length;
            long need = xgd.TotalBytes - have;
            var root = Path.GetPathRoot(Path.GetFullPath(dest));
            if (root != null && need > 0 && new DriveInfo(root).AvailableFreeSpace < need + (256L << 20))
                throw new IOException($"Not enough free space on {root}: need about {need / 1073741824.0:0.0} GB.");
        }
        catch (IOException) { throw; }
        catch { /* space check is best effort */ }
    }

    /// <summary>Installs Xbox 360 DLC packages from a package, a zip or a folder (recursively) into userdata.</summary>
    public static List<string> ImportDlc(string source, string userdata, IProgress<(string, double)> progress, CancellationToken ct)
    {
        var log = new List<string>();
        int ok = 0;
        void One(string file, string name)
        {
            progress.Report((name, 0));
            try
            {
                using var pkg = new Stfs(file);
                if (pkg.ContentType != 2) { log.Add($"skipped (not DLC): {name}"); return; }
                if (pkg.TitleId != TitleId) { log.Add($"skipped (not UFC 3): {name}"); return; }
                var titleDir = Path.Combine(userdata, "0000000000000000", TitleId.ToString("X8"));
                var dest = Path.Combine(titleDir, "00000002", name);
                var tmp = dest + ".partial";
                if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
                var (n, bytes) = pkg.Extract(tmp, ct);
                if (Directory.Exists(dest)) Directory.Delete(dest, true);
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                Directory.Move(tmp, dest);
                var hdr = Path.Combine(titleDir, "Headers", "00000002");
                Directory.CreateDirectory(hdr);
                File.WriteAllBytes(Path.Combine(hdr, name + ".header"), HeaderBytes(pkg.Title, name));
                log.Add($"ok   {pkg.Title}  [{name}]  {n} files, {bytes / 1048576.0:0} MB");
                ok++;
            }
            catch (InvalidDataException ex) when (ex.Message == "not an STFS package") { /* unrelated file */ }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex) { log.Add($"failed: {name} ({ex.Message})"); }
        }

        void Scan(string path)
        {
            ct.ThrowIfCancellationRequested();
            if (Directory.Exists(path))
            {
                foreach (var f in Directory.GetFiles(path, "*", SearchOption.AllDirectories).Order()) Scan(f);
                return;
            }
            bool zip;
            using (var s = File.OpenRead(path)) { var m = new byte[4]; zip = s.Read(m) == 4 && m[0] == 'P' && m[1] == 'K'; }
            if (!zip) { One(path, Path.GetFileName(path)); return; }
            using var z = ZipFile.OpenRead(path);
            foreach (var entry in z.Entries.Where(e => e.Length > 0x1000))
            {
                ct.ThrowIfCancellationRequested();
                var tmp = Path.Combine(Path.GetTempPath(), "ufc3-dlc-" + Guid.NewGuid().ToString("N"));
                try
                {
                    entry.ExtractToFile(tmp);
                    One(tmp, Path.GetFileName(entry.FullName));
                }
                finally { try { File.Delete(tmp); } catch { } }
            }
        }

        Scan(source);
        log.Insert(0, ok == 0 ? "No UFC 3 DLC packages found there." : $"{ok} DLC package(s) installed.");
        return log;
    }

    /// <summary>XCONTENT_AGGREGATE_DATA (0x148 bytes) plus an all-ones license mask: every pack counts as purchased.</summary>
    static byte[] HeaderBytes(string title, string fileName)
    {
        var b = new byte[0x148 + 4];
        void BE32(int o, uint v) { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }
        BE32(0, 1); BE32(4, 2);
        var nm = Encoding.BigEndianUnicode.GetBytes(title);
        Array.Copy(nm, 0, b, 8, Math.Min(nm.Length, 254));
        var fn = Encoding.ASCII.GetBytes(fileName);
        Array.Copy(fn, 0, b, 0x108, Math.Min(fn.Length, 42));
        BE32(0x140, TitleId);
        b[0x148] = b[0x149] = b[0x14A] = b[0x14B] = 0xFF;
        return b;
    }
}
