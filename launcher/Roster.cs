using System.Buffers.Binary;
using System.Text;

namespace UFC3Launcher;

/// <summary>One fighter as the player knows them: a name and every id the game uses for that fighter
/// (disc, PRIDE version, alternate-mode versions, mod-pack ports).</summary>
public sealed record RosterFighter(string Name, List<int> Ids)
{
    /// <summary>Ids that come from an installed DLC package (PlayerParamDLC&lt;id&gt;.bin, ids 1300+).</summary>
    public HashSet<int> DlcIds { get; init; } = new();
}

/// <summary>
/// Reads the fighter list from the installed game files, the same way mods_src/fighter_music/list_fighters.py does:
/// DAT/static_x.mpc leaf /root/sys/plpm (276-byte records: u16 id at +0, u16 flags at +2, name label hashes at
/// +8/+12/+16) and the names from DAT/ytx/EN/generic.ytx + dlc.ytx (group 0x050C5D71). A file in the mods folder's
/// DAT\ wins over the one in the game files, like in game, so mod-pack fighters appear without any extra list.
/// DLC fighters: each installed package (userdata\&lt;profile&gt;\5451087D\00000002\&lt;pkg&gt;\) carries one
/// PlayerParamDLC&lt;id&gt;.bin per fighter (a 276-byte plpm record, ids 1300-1314; the disc plpm has unnamed
/// placeholders for them) and its names in dlc_&lt;lang&gt;.ytx (group 0x1F953AD4, not the disc's name group).
/// Only fighters whose package is installed are listed.
/// </summary>
public static class Roster
{
    const uint NameGroup = 0x050C5D71;

    const uint TitleId = 0x5451087D;

    public static List<RosterFighter> Load(string assets, string mods, string lang = "EN", string? userData = null)
    {
        string Pick(params string[] parts)
        {
            var m = Path.Combine(new[] { mods, "DAT" }.Concat(parts).ToArray());
            return File.Exists(m) ? m : Path.Combine(new[] { assets, "DAT" }.Concat(parts).ToArray());
        }
        var mpc = Pick("static_x.mpc");
        if (!File.Exists(mpc)) throw new FileNotFoundException("The game files have no DAT\\static_x.mpc yet (Setup tab).");
        var plpm = MpcLeaf(File.ReadAllBytes(mpc), "/root/sys/plpm");
        var names = YtxNames(Pick("ytx", lang, "generic.ytx"));
        foreach (var kv in YtxNames(Pick("ytx", lang, "dlc.ytx"))) names[kv.Key] = kv.Value;
        int count = (int)BinaryPrimitives.ReadUInt32BigEndian(plpm);
        var byName = new Dictionary<string, RosterFighter>(StringComparer.OrdinalIgnoreCase);
        var order = new List<RosterFighter>();
        var dlc = userData != null ? DlcRecords(userData, lang) : new();
        void Add(byte[] d, int o, Dictionary<uint, string> nm, bool fromDlc)
        {
            int id = BinaryPrimitives.ReadUInt16BigEndian(d.AsSpan(o));
            int flags = BinaryPrimitives.ReadUInt16BigEndian(d.AsSpan(o + 2));
            bool staff = (flags & 0x8000) == 0;
            var parts = new List<string>();
            for (int k = 0; k < 3; k++)
            {
                uint h = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(o + 8 + 4 * k));
                if (h != 0 && nm.TryGetValue(h, out var s) && s.Length > 0) parts.Add(s);
            }
            var name = string.Join(' ', parts);
            if (id == 0 || staff || name.Length == 0) return;
            if (!byName.TryGetValue(name, out var f)) { f = new RosterFighter(name, new()); byName[name] = f; order.Add(f); }
            if (!f.Ids.Contains(id)) f.Ids.Add(id);
            if (fromDlc) f.DlcIds.Add(id);
        }
        for (int i = 0; i < count; i++)
        {
            int o = 16 + 276 * i;
            if (o + 276 > plpm.Length) break;
            int id = BinaryPrimitives.ReadUInt16BigEndian(plpm.AsSpan(o));
            if (dlc.ContainsKey(id)) continue;   // the package's own record (and names) win
            Add(plpm, o, names, false);
        }
        var merged = new Dictionary<Dictionary<uint, string>, Dictionary<uint, string>>(ReferenceEqualityComparer.Instance);
        foreach (var (rec, nm) in dlc.Values)
        {
            // The package's dlc_<lang>.ytx first, then the disc's names.
            if (!merged.TryGetValue(nm, out var all)) { all = new(names); foreach (var kv in nm) all[kv.Key] = kv.Value; merged[nm] = all; }
            Add(rec, 0, all, true);
        }
        foreach (var f in order) f.Ids.Sort();
        return order.OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }

    /// <summary>Every installed DLC package's fighter records: id -&gt; (276-byte record, names for it). Profile
    /// folders other than 0000000000000000 are read too.</summary>
    static Dictionary<int, (byte[] Rec, Dictionary<uint, string> Names)> DlcRecords(string userData, string lang)
    {
        var res = new Dictionary<int, (byte[], Dictionary<uint, string>)>();
        try
        {
            if (!Directory.Exists(userData)) return res;
            foreach (var profile in Directory.EnumerateDirectories(userData))
            {
                var content = Path.Combine(profile, TitleId.ToString("X8"), "00000002");
                if (!Directory.Exists(content)) continue;
                foreach (var pkg in Directory.EnumerateDirectories(content).OrderBy(p => p, StringComparer.OrdinalIgnoreCase))
                {
                    var recs = Directory.GetFiles(pkg, "PlayerParamDLC*.bin");
                    if (recs.Length == 0) continue;   // Ultimate Fights video packs, title updates
                    var ytx = Path.Combine(pkg, $"dlc_{lang.ToLowerInvariant()}.ytx");
                    if (!File.Exists(ytx)) ytx = Path.Combine(pkg, "dlc_en.ytx");
                    var nm = YtxNames(ytx, null);
                    foreach (var r in recs)
                    {
                        try
                        {
                            var d = File.ReadAllBytes(r);
                            if (d.Length < 276) continue;
                            res[BinaryPrimitives.ReadUInt16BigEndian(d)] = (d, nm);
                        }
                        catch (IOException) { }
                    }
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return res;
    }

    static byte[] MpcLeaf(byte[] d, string want)
    {
        uint table = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(8)), dataStart = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(12));
        (uint tag, string name, uint value) Entry(int i)
        {
            int p = (int)table + 40 * i;
            var nm = d.AsSpan(p + 4, 32); int z = nm.IndexOf((byte)0); if (z >= 0) nm = nm[..z];
            return (BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(p)), Encoding.Latin1.GetString(nm), BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(p + 36)));
        }
        int cur = 0;
        foreach (var part in want.Trim('/').Split('/').Skip(1))
        {
            var (tag, _, first) = Entry(cur);
            int found = -1;
            for (long c = first; c < first + tag; c++) if (Entry((int)c).name == part) { found = (int)c; break; }
            if (found < 0) throw new InvalidDataException($"static_x.mpc: no {part}");
            cur = found;
        }
        var (t, _, off) = Entry(cur);
        int size = (int)(t & 0xFFFFFF) * 32;
        return d.AsSpan((int)(dataStart + off), size).ToArray();
    }

    /// <summary>Labels -&gt; strings of one ytx group (null: every group).</summary>
    static Dictionary<uint, string> YtxNames(string path, uint? group = NameGroup)
    {
        var res = new Dictionary<uint, string>();
        if (!File.Exists(path)) return res;
        var d = File.ReadAllBytes(path);
        uint groups = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(0x20));
        for (int g = 0; g < groups; g++)
        {
            int gp = 0x28 + 12 * g;
            uint gh = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(gp)), cnt = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(gp + 4)), off = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(gp + 8));
            if (group != null && gh != group) continue;
            for (int e = 0; e < cnt; e++)
            {
                int ep = 0x20 + (int)off + 8 * e;
                uint label = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(ep)), so = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(ep + 4));
                int p = 0x20 + (int)so, end = p;
                while (end + 1 < d.Length && !(d[end] == 0 && d[end + 1] == 0)) end += 2;
                res[label] = Encoding.BigEndianUnicode.GetString(d, p, end - p);
            }
        }
        return res;
    }
}
