using System.Text;

namespace UFC3Launcher;

/// <summary>Xbox 360 XGD/XDVDFS reader. Extracts exactly what tools/xiso_extract.py extracts.</summary>
public class Xgd : IDisposable
{
    const int Sector = 2048;
    static readonly byte[] Magic = "MICROSOFT*XBOX*MEDIA"u8.ToArray();
    // rebuilt XISO, XGD2, XGD1, XGD3 (same list as xiso_extract.py) plus two older layouts
    static readonly long[] Offsets = { 0x0, 0xFD90000, 0x18300000, 0x2080000, 0xFB20, 0x20600 };

    public record Entry(string Path, bool IsDir, long Sector, long Size);
    public record XexInfo(uint TitleId, uint MediaId);

    readonly FileStream fs;
    readonly long gameOffset;
    public List<Entry> Entries { get; } = new();

    public Xgd(string iso)
    {
        fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        try
        {
            gameOffset = FindGameOffset();
            var vd = ReadAt(gameOffset + 32 * Sector, 28);
            long rootSector = BitConverter.ToUInt32(vd, 20), rootSize = BitConverter.ToUInt32(vd, 24);
            if (rootSize != 0) ReadTree(gameOffset + rootSector * Sector, rootSize, "");
        }
        catch { fs.Dispose(); throw; }
    }

    byte[] ReadAt(long off, int len)
    {
        var b = new byte[len];
        if (off < 0 || off + len > fs.Length) throw new InvalidDataException("Read outside the image");
        fs.Seek(off, SeekOrigin.Begin);
        fs.ReadExactly(b);
        return b;
    }

    long FindGameOffset()
    {
        foreach (var o in Offsets)
        {
            long m = o + 32 * Sector;
            if (m + Sector > fs.Length) continue;
            var hdr = ReadAt(m, Sector);
            if (hdr.AsSpan(0, 20).SequenceEqual(Magic) && hdr.AsSpan(0x7EC, 20).SequenceEqual(Magic)) return o;
        }
        throw new InvalidDataException("This is not an Xbox 360 game disc image.");
    }

    void ReadTree(long dirOff, long dirSize, string parent)
    {
        if (dirSize > 64 << 20) throw new InvalidDataException("Suspicious directory size");
        var dir = ReadAt(dirOff, (int)dirSize);
        Walk(dir, 0, parent, new HashSet<int>());
    }

    void Walk(byte[] dir, int ordinal, string parent, HashSet<int> seen)
    {
        int o = ordinal * 4;
        if (o + 14 > dir.Length) throw new InvalidDataException("Directory entry outside table");
        if (!seen.Add(ordinal)) throw new InvalidDataException("Directory tree cycle");
        int l = BitConverter.ToUInt16(dir, o), r = BitConverter.ToUInt16(dir, o + 2);
        if (l == 0xFFFF && r == 0xFFFF) return; // padding
        long sector = BitConverter.ToUInt32(dir, o + 4), size = BitConverter.ToUInt32(dir, o + 8);
        bool isDir = (dir[o + 12] & 0x10) != 0;
        int nl = dir[o + 13];
        if (o + 14 + nl > dir.Length) throw new InvalidDataException("Directory name outside table");

        if (l != 0) Walk(dir, l, parent, seen);
        var name = Encoding.ASCII.GetString(dir, o + 14, nl);
        if (name.Contains("..") || name.Contains('/') || name.Contains('\\') || name.Contains(':'))
            throw new InvalidDataException("Unsafe name in image: " + name);
        var path = parent.Length == 0 ? name : parent + "/" + name;
        Entries.Add(new Entry(path, isDir, sector, size));
        if (isDir && size != 0) ReadTree(gameOffset + sector * Sector, size, path);
        if (r != 0) Walk(dir, r, parent, seen);
    }

    public Entry? Find(string path) =>
        Entries.FirstOrDefault(e => !e.IsDir && e.Path.Equals(path, StringComparison.OrdinalIgnoreCase));

    /// <summary>Title ID and media ID from the execution-info block of an XEX2 header.</summary>
    public XexInfo ReadXexInfo(Entry xex)
    {
        int len = (int)Math.Min(xex.Size, 0x10000);
        var h = ReadAt(gameOffset + xex.Sector * Sector, len);
        if (len < 0x18 || h[0] != 'X' || h[1] != 'E' || h[2] != 'X') throw new InvalidDataException("default.xex is not an Xbox 360 executable.");
        uint BE(int o) => (uint)(h[o] << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3]);
        uint count = BE(0x14);
        for (uint i = 0; i < count && 0x18 + i * 8 + 8 <= len; i++)
        {
            if (BE((int)(0x18 + i * 8)) != 0x00040006) continue;
            uint v = BE((int)(0x1C + i * 8));
            if (v + 16 > len) break;
            return new XexInfo(BE((int)v + 12), BE((int)v));
        }
        throw new InvalidDataException("default.xex has no title information.");
    }

    public long TotalBytes => Entries.Where(e => !e.IsDir).Sum(e => e.Size);

    /// <summary>Extracts every file under outRoot (empty folders too). Finished files of the right size are
    /// kept, so a cancelled run resumes; default.xex is written last, so a half copy never looks complete.</summary>
    public void ExtractAll(string outRoot, IProgress<(string, double)> progress, CancellationToken ct)
    {
        var files = Entries.Where(e => !e.IsDir)
            .OrderBy(e => e.Path.Equals("default.xex", StringComparison.OrdinalIgnoreCase) ? 1 : 0).ToList();
        long total = files.Sum(f => f.Size), done = 0;
        var root = Path.GetFullPath(outRoot);
        Directory.CreateDirectory(root);
        foreach (var d in Entries.Where(e => e.IsDir))
            Directory.CreateDirectory(SafePath(root, d.Path));
        var buf = new byte[8 << 20];
        foreach (var e in files)
        {
            ct.ThrowIfCancellationRequested();
            var dest = SafePath(root, e.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            if (File.Exists(dest) && new FileInfo(dest).Length == e.Size) { done += e.Size; continue; }
            var tmp = dest + ".part";
            using (var o = new FileStream(tmp, FileMode.Create, FileAccess.Write))
            {
                fs.Seek(gameOffset + e.Sector * Sector, SeekOrigin.Begin);
                long left = e.Size;
                while (left > 0)
                {
                    ct.ThrowIfCancellationRequested();
                    int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
                    if (n <= 0) throw new EndOfStreamException("The image is truncated (incomplete download?).");
                    o.Write(buf, 0, n);
                    left -= n; done += n;
                    progress.Report((e.Path, total == 0 ? 1 : (double)done / total));
                }
            }
            File.Move(tmp, dest, true);
        }
        progress.Report(("", 1));
    }

    static string SafePath(string root, string rel)
    {
        var p = Path.GetFullPath(Path.Combine(root, rel.Replace('/', '\\')));
        if (!p.StartsWith(root + Path.DirectorySeparatorChar) && p != root) throw new InvalidDataException("Path escapes output: " + rel);
        return p;
    }

    public void Dispose() => fs.Dispose();
}
