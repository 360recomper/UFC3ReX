using System.Text;

namespace UFC3Launcher;

/// <summary>Minimal STFS (LIVE/PIRS/CON) reader for DLC packages (port of tools/install_dlc.py).</summary>
public class Stfs : IDisposable
{
    static readonly int[] BlocksPerLevel = { 170, 170 * 170, 170 * 170 * 170 };
    readonly FileStream f;
    readonly int shift, ftCount, ftBlock;
    readonly long baseOff;
    public uint ContentType { get; }
    public uint TitleId { get; }
    public string Title { get; }

    static int I24(byte[] b, int o) => b[o] | (b[o + 1] << 8) | (b[o + 2] << 16);
    static int I24BE(byte[] b, int o) => (b[o] << 16) | (b[o + 1] << 8) | b[o + 2];
    static uint BE32(byte[] b, int o) => (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);

    public Stfs(string path)
    {
        f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            var h = new byte[0x1000];
            if (f.Read(h, 0, h.Length) < h.Length) throw new InvalidDataException("not an STFS package");
            var magic = Encoding.ASCII.GetString(h, 0, 4);
            if (magic is not ("LIVE" or "PIRS" or "CON ")) throw new InvalidDataException("not an STFS package");
            uint headerSize = BE32(h, 0x340);
            ContentType = BE32(h, 0x344);
            TitleId = BE32(h, 0x360);
            shift = (h[0x37B] & 1) != 0 ? 0 : 1;
            ftCount = BitConverter.ToUInt16(h, 0x37C);
            ftBlock = I24(h, 0x37E);
            Title = Encoding.BigEndianUnicode.GetString(h, 0x411, 0x100).Split('\0')[0].Trim();
            baseOff = (headerSize + 0xFFF) & ~0xFFFL;
        }
        catch { f.Dispose(); throw; }
    }

    long BlockOffset(int idx)
    {
        long block = idx;
        foreach (var lvl in BlocksPerLevel)
        {
            block += (long)((idx + lvl) / lvl) << shift;
            if (idx < lvl) break;
        }
        return baseOff + (block << 12);
    }

    long HashTableOffset(int idx)
    {
        long block = (long)(idx / 170) * (170 + (1 << shift));
        if (idx >= 170) block += (long)((idx / (170 * 170)) + 1) << shift;
        if (idx >= 170 * 170) block += 1 << shift;
        return baseOff + (block << 12);
    }

    int NextBlock(int idx)
    {
        var b = new byte[0x18];
        f.Seek(HashTableOffset(idx) + (idx % 170) * 0x18, SeekOrigin.Begin);
        f.ReadExactly(b);
        return I24BE(b, 0x15);
    }

    byte[] ReadBlock(int idx)
    {
        var b = new byte[0x1000];
        f.Seek(BlockOffset(idx), SeekOrigin.Begin);
        f.ReadExactly(b);
        return b;
    }

    record Ent(string Name, bool Contig, bool IsDir, int Blocks, int Start, int Parent, long Size);

    List<Ent> Entries()
    {
        var res = new List<Ent>();
        int blkIdx = ftBlock;
        for (int i = 0; i < ftCount; i++)
        {
            var blk = ReadBlock(blkIdx);
            for (int e = 0; e < 0x1000; e += 0x40)
            {
                int flags = blk[e + 0x28], n = flags & 0x3F;
                if (n == 0) continue;
                res.Add(new Ent(Encoding.ASCII.GetString(blk, e, n), (flags & 0x40) != 0, (flags & 0x80) != 0,
                    I24(blk, e + 0x29), I24(blk, e + 0x2F), (short)(blk[e + 0x32] << 8 | blk[e + 0x33]),
                    BE32(blk, e + 0x34)));
            }
            blkIdx = ftCount == 1 ? blkIdx + 1 : NextBlock(blkIdx);
        }
        return res;
    }

    /// <summary>Extract all files into dest; returns (files, bytes).</summary>
    public (int, long) Extract(string dest, CancellationToken ct = default)
    {
        var ents = Entries();
        var root = Path.GetFullPath(dest);
        string Rel(int i)
        {
            var parts = new List<string>();
            for (int guard = 0; i >= 0; i = ents[i].Parent)
            {
                if (guard++ > 1000 || i >= ents.Count) throw new InvalidDataException("bad STFS directory");
                var nm = ents[i].Name;
                if (nm is "." or ".." || nm.Contains('/') || nm.Contains('\\') || nm.Contains(':'))
                    throw new InvalidDataException("unsafe path in package");
                parts.Add(nm);
            }
            parts.Reverse();
            return Path.Combine(parts.ToArray());
        }
        int count = 0; long bytes = 0;
        for (int i = 0; i < ents.Count; i++)
        {
            ct.ThrowIfCancellationRequested();
            var e = ents[i];
            var p = Path.GetFullPath(Path.Combine(root, Rel(i)));
            if (!p.StartsWith(root + Path.DirectorySeparatorChar)) throw new InvalidDataException("unsafe path in package");
            if (e.IsDir) { Directory.CreateDirectory(p); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(p)!);
            long remaining = e.Size;
            int blk = e.Start;
            using var o = new FileStream(p, FileMode.Create, FileAccess.Write);
            for (int b = 0; b < e.Blocks && remaining > 0; b++)
            {
                var chunk = ReadBlock(blk);
                int n = (int)Math.Min(0x1000, remaining);
                o.Write(chunk, 0, n);
                remaining -= n;
                blk = e.Contig ? blk + 1 : NextBlock(blk);
            }
            if (remaining > 0) throw new InvalidDataException($"{e.Name}: short read");
            count++; bytes += e.Size;
        }
        return (count, bytes);
    }

    public void Dispose() => f.Dispose();
}
