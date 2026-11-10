using System.Globalization;
using System.Text;
using NAudio.MediaFoundation;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace UFC3Launcher;

/// <summary>Decoded, trimmed and loudness-matched stereo audio (44.1/48 kHz, float). Preview plays exactly this and
/// Save encodes exactly this, so what you hear is what the game gets.</summary>
public sealed class MusicClip
{
    public float[] Samples = Array.Empty<float>();
    public int Rate;
    public double Seconds => Rate == 0 ? 0 : Samples.Length / 2.0 / Rate;
}

/// <summary>Walkout songs: audio export plus the round-trip-safe editing of fighter_music.toml. Launcher only; the game
/// reads the toml (src/fighter_music.h) and plays the files through Media Foundation.</summary>
public static class Music
{
    public const string Marker = "# music-tab:";
    /// <summary>The game's toml has one global volume (no per-file volume), so every song is levelled here instead:
    /// RMS about -18 dBFS (peak-limited), times the Loudness choice.</summary>
    const float TargetRms = 0.125f;

    public static string Dir()
    {
        var d = new Toml().Get("ufc3_fighter_music_dir");
        return string.IsNullOrWhiteSpace(d) ? Path.Combine(Paths.Mods, "music") : Path.GetFullPath(d, Paths.Root);
    }

    public static string TomlFile(string? dir = null) => Path.Combine(dir ?? Dir(), "fighter_music.toml");

    public static string SafeName(string name)
    {
        var sb = new StringBuilder();
        foreach (var c in name) sb.Append(char.IsAsciiLetterOrDigit(c) || c is '-' or '.' ? c : '_');
        var s = sb.ToString().Trim('_', '.');
        return (s.Length == 0 ? "fighter" : s) + ".m4a";
    }

    // ------------------------------------------------------------------ time

    /// <summary>"1:23", "0:01:23.5", "83" or "83.5" (seconds) -> seconds; null when it isn't a time.</summary>
    public static double? ParseTime(string s)
    {
        s = s.Trim();
        if (s.Length == 0) return 0;
        double total = 0;
        foreach (var p in s.Split(':'))
        {
            if (!double.TryParse(p.Trim().Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) || v < 0) return null;
            total = total * 60 + v;
        }
        return total;
    }

    public static string FormatTime(double sec)
    {
        int t = (int)Math.Round(sec);
        return t >= 3600 ? $"{t / 3600}:{t / 60 % 60:00}:{t % 60:00}" : $"{t / 60}:{t % 60:00}";
    }

    // ------------------------------------------------------------------ audio

    public const double FadeOut = 1.5;
    /// <summary>How long a walkout lasts in game (measured from play logs: 36.6 s and 37.9 s). The game stops the song then.</summary>
    public const double WalkoutSeconds = 38;

    /// <summary>Whole-song overview: loudest negative/positive sample per 50 ms, mono mix. Computed once per chosen song.</summary>
    public sealed class Peaks
    {
        public const double Step = 0.05;
        public float[] Min = Array.Empty<float>(), Max = Array.Empty<float>();
        public double Seconds;
    }

    public static Peaks ComputePeaks(string path, CancellationToken ct = default)
    {
        Mf();
        using var reader = new MediaFoundationReader(path);
        ISampleProvider sp = reader.ToSampleProvider();
        int ch = sp.WaveFormat.Channels, rate = sp.WaveFormat.SampleRate;
        int per = Math.Max(1, (int)(rate * Peaks.Step));
        var mins = new List<float>(); var maxs = new List<float>();
        var buf = new float[per * ch * 20];
        float lo = 0, hi = 0; int cnt = 0; long frames = 0; int n;
        while ((n = sp.Read(buf, 0, buf.Length)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            for (int i = 0; i + ch <= n; i += ch)
            {
                float v = 0; for (int c = 0; c < ch; c++) v += buf[i + c];
                v /= ch;
                if (v < lo) lo = v; if (v > hi) hi = v;
                frames++;
                if (++cnt == per) { mins.Add(lo); maxs.Add(hi); lo = hi = 0; cnt = 0; }
            }
        }
        if (cnt > 0) { mins.Add(lo); maxs.Add(hi); }
        return new Peaks { Min = mins.ToArray(), Max = maxs.ToArray(), Seconds = (double)frames / rate };
    }

    /// <summary>The song as a 16-bit stereo-or-mono stream from <paramref name="startSec"/>, for listening (not levelled).</summary>
    public static IWaveProvider OpenStream(string path, double startSec, out IDisposable owner)
    {
        Mf();
        var reader = new MediaFoundationReader(path);
        owner = reader;
        if (startSec > 0) reader.CurrentTime = TimeSpan.FromSeconds(startSec);
        ISampleProvider sp = reader.ToSampleProvider();
        if (sp.WaveFormat.Channels > 2) sp = new MultiplexingSampleProvider(new[] { sp }, 2);
        return new SampleToWaveProvider16(sp);
    }

    static bool mfStarted;
    static void Mf() { if (!mfStarted) { MediaFoundationApi.Startup(); mfStarted = true; } }

    /// <summary>Decode from <paramref name="startSec"/> to the end, to stereo 44.1/48 kHz, and level it.</summary>
    public static MusicClip Decode(string path, double startSec, int loudnessPct, double endSec = 0, double fadeSec = FadeOut)
    {
        Mf();
        using var reader = new MediaFoundationReader(path);
        ISampleProvider sp = reader.ToSampleProvider();
        if (sp.WaveFormat.Channels == 1) sp = new MonoToStereoSampleProvider(sp);
        else if (sp.WaveFormat.Channels > 2) sp = new MultiplexingSampleProvider(new[] { sp }, 2);
        int rate = sp.WaveFormat.SampleRate;
        if (rate != 44100 && rate != 48000) { sp = new WdlResamplingSampleProvider(sp, 44100); rate = 44100; }
        long skip = (long)(startSec * rate) * 2;
        long limit = endSec > startSec ? (long)((endSec - startSec) * rate) * 2 : long.MaxValue;   // samples kept after the start
        var all = new List<float>(rate * 2 * 240);
        var buf = new float[rate * 2];
        int n;
        while ((n = sp.Read(buf, 0, buf.Length)) > 0)
        {
            int from = 0;
            if (skip > 0) { int s = (int)Math.Min(skip, n); skip -= s; from = s; }
            for (int i = from; i < n && all.Count < limit; i++) all.Add(buf[i]);
            if (all.Count >= limit) break;
        }
        if (all.Count < rate) throw new InvalidDataException("Nothing left to play after the start point.");
        var data = all.ToArray();
        double sum = 0; float peak = 1e-6f;
        foreach (var v in data) { sum += (double)v * v; peak = Math.Max(peak, Math.Abs(v)); }
        float rms = (float)Math.Sqrt(sum / data.Length);
        float gain = Math.Min(TargetRms / Math.Max(rms, 1e-6f), 0.95f / peak) * (loudnessPct / 100f);
        for (int i = 0; i < data.Length; i++) data[i] = Math.Clamp(data[i] * gain, -0.98f, 0.98f);
        if (endSec > startSec && fadeSec > 0)
        {
            // short fade-out at the cut so the loop back to the start isn't a hard cut
            int fade = (int)Math.Min(fadeSec * rate, data.Length / 2 / 2) ;
            for (int k = 0; k < fade; k++)
            {
                float g = 1f - (float)(k + 1) / fade;
                int i = data.Length - 2 * (fade - k);
                data[i] *= g; data[i + 1] *= g;
            }
        }
        return new MusicClip { Samples = data, Rate = rate };
    }

    public static void EncodeM4a(MusicClip clip, string outPath, int bitRate = 256000)
    {
        Mf();
        Directory.CreateDirectory(Path.GetDirectoryName(outPath)!);
        var tmp = outPath + ".tmp.m4a";
        try
        {
            File.Delete(tmp);
            MediaFoundationEncoder.EncodeToAac(new ClipProvider(clip).ToWaveProvider16(), tmp, bitRate);
            File.Move(tmp, outPath, true);
        }
        finally { try { File.Delete(tmp); } catch { } }
    }

    public static double DurationOf(string path)
    {
        Mf();
        using var r = new MediaFoundationReader(path);
        return r.TotalTime.TotalSeconds;
    }

    public sealed class ClipProvider : ISampleProvider
    {
        readonly MusicClip clip; int pos; readonly int limit;
        /// <param name="maxSeconds">Stop after this many seconds (0 = play it all).</param>
        public ClipProvider(MusicClip c, double maxSeconds = 0) { clip = c; limit = maxSeconds > 0 ? (int)Math.Min(c.Samples.Length, maxSeconds * c.Rate * 2) & ~1 : c.Samples.Length; WaveFormat = WaveFormat.CreateIeeeFloatWaveFormat(c.Rate, 2); }
        public WaveFormat WaveFormat { get; }
        public int Read(float[] buffer, int offset, int count)
        {
            int n = Math.Max(0, Math.Min(count, limit - pos));
            Array.Copy(clip.Samples, pos, buffer, offset, n);
            pos += n;
            return n;
        }
    }

    // ------------------------------------------------------------------ fighter_music.toml

    public sealed class Mapping
    {
        public Dictionary<int, string> Tracks = new();
        public string? Any;
    }

    /// <summary>What the game will play, read the way fighter_music.h reads it: [fighters] "id" = "file", "*" = "file",
    /// then every [[track]] (file + fighters list) on top.</summary>
    public static Mapping Parse(IEnumerable<string> lines)
    {
        var m = new Mapping();
        string sect = "";
        string? file = null; var ids = new List<int>();
        bool inArr = false; var arr = new StringBuilder();
        void Flush() { if (sect == "track" && file != null) foreach (var i in ids) m.Tracks[i] = file; file = null; ids = new(); }
        foreach (var raw in lines)
        {
            var l = StripComment(raw).Trim();
            if (inArr) { arr.Append(' ').Append(l); if (l.Contains(']')) { inArr = false; ids = Ints(arr.ToString()); } continue; }
            if (l.Length == 0) continue;
            if (l.StartsWith('[')) { Flush(); sect = l.Trim('[', ']').Trim(); continue; }
            int eq = l.IndexOf('=');
            if (eq < 0) continue;
            var key = l[..eq].Trim().Trim('"'); var val = l[(eq + 1)..].Trim();
            if (sect == "fighters")
            {
                if (!val.StartsWith('"')) continue;
                var f = val.Trim('"');
                if (key == "*") m.Any = f; else if (int.TryParse(key, out var id)) m.Tracks[id] = f;
            }
            else if (sect == "track")
            {
                if (key == "file") file = val.Trim('"').Replace("\\\\", "\\");
                else if (key == "fighters")
                {
                    if (val.Contains(']')) ids = Ints(val); else { inArr = true; arr.Clear().Append(val); }
                }
            }
        }
        Flush();
        return m;
    }

    static List<int> Ints(string s)
    {
        var r = new List<int>();
        foreach (var p in s.Trim().Trim('[', ']').Split(',', StringSplitOptions.RemoveEmptyEntries))
            if (int.TryParse(p.Trim(), out var v) && v >= 0) r.Add(v);
        return r;
    }

    static string StripComment(string l)
    {
        bool q = false;
        for (int i = 0; i < l.Length; i++) { if (l[i] == '"') q = !q; else if (l[i] == '#' && !q) return l[..i]; }
        return l;
    }

    /// <summary>The launcher's own song entry for a fighter: marker comment + [[track]] block.</summary>
    public sealed record Entry(string Name, string File, List<int> Ids, double Start, int Loudness, string Source, double End = 0);

    public static List<Entry> OwnEntries(IList<string> lines)
    {
        var res = new List<Entry>();
        for (int i = 0; i < lines.Count; i++)
        {
            if (!lines[i].StartsWith(Marker)) continue;
            var parts = lines[i][Marker.Length..].Split(" | ");
            double start = 0, end = 0; int vol = 100; string src = "";
            foreach (var p in parts.Skip(1))
            {
                if (p.StartsWith("start=")) double.TryParse(p[6..], NumberStyles.Float, CultureInfo.InvariantCulture, out start);
                else if (p.StartsWith("end=")) double.TryParse(p[4..], NumberStyles.Float, CultureInfo.InvariantCulture, out end);
                else if (p.StartsWith("loudness=")) int.TryParse(p[9..], out vol);
                else if (p.StartsWith("source=")) src = p[7..];
            }
            var block = new List<string>();
            for (int j = i + 1; j < lines.Count && lines[j].Trim().Length > 0; j++) block.Add(lines[j]);
            var mp = Parse(block);
            if (mp.Tracks.Count == 0) continue;
            res.Add(new Entry(parts[0].Trim(), mp.Tracks.Values.First(), mp.Tracks.Keys.OrderBy(x => x).ToList(), start, vol, src, end));
        }
        return res;
    }

    /// <summary>Removes the launcher's block for this fighter (marker line through the blank line after it). Returns the file name it mapped.</summary>
    public static string? RemoveOwn(List<string> lines, string name)
    {
        for (int i = 0; i < lines.Count; i++)
        {
            if (!lines[i].StartsWith(Marker)) continue;
            if (!lines[i][Marker.Length..].Split(" | ")[0].Trim().Equals(name, StringComparison.OrdinalIgnoreCase)) continue;
            int j = i + 1;
            while (j < lines.Count && lines[j].Trim().Length > 0) j++;
            var file = Parse(lines.Skip(i + 1).Take(j - i - 1)).Tracks.Values.FirstOrDefault();
            if (j < lines.Count) j++;   // the blank separator
            lines.RemoveRange(i, j - i);
            return file ?? "";
        }
        return null;
    }

    public static List<string> ReadLines(string toml) => File.Exists(toml) ? File.ReadAllLines(toml).ToList() : new();

    static void WriteLines(string toml, List<string> lines)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(toml)!);
        while (lines.Count > 0 && lines[^1].Trim().Length == 0) lines.RemoveAt(lines.Count - 1);
        File.WriteAllText(toml, string.Join(Environment.NewLine, lines) + Environment.NewLine, new UTF8Encoding(false));
    }

    public static void SetEntry(string toml, Entry e)
    {
        var lines = ReadLines(toml);
        RemoveOwn(lines, e.Name);
        while (lines.Count > 0 && lines[^1].Trim().Length == 0) lines.RemoveAt(lines.Count - 1);
        if (lines.Count > 0) lines.Add("");
        lines.Add($"{Marker} {e.Name} | start={e.Start.ToString("0.###", CultureInfo.InvariantCulture)}{(e.End > e.Start ? " | end=" + e.End.ToString("0.###", CultureInfo.InvariantCulture) : "")} | loudness={e.Loudness} | source={e.Source}");
        lines.Add("[[track]]");
        lines.Add("file = \"" + e.File.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"");
        lines.Add("fighters = [" + string.Join(", ", e.Ids) + "]");
        WriteLines(toml, lines);
    }

    /// <summary>Drops the launcher's entry; returns the m4a it pointed to (null when there was none). The caller deletes that file.</summary>
    public static string? RemoveEntry(string toml, string name)
    {
        var lines = ReadLines(toml);
        var f = RemoveOwn(lines, name);
        if (f != null) WriteLines(toml, lines);
        return f;
    }

    /// <summary>Turns ufc3_fighter_music on in ufc3.toml. True when it was off and has just been switched on.</summary>
    public static bool EnableInGame()
    {
        var t = new Toml();
        if (t.GetBool("ufc3_fighter_music", false)) return false;
        t.Set("ufc3_fighter_music", true); t.Save();
        return true;
    }

    // ------------------------------------------------------------------ headless self-test

    /// <summary>--music-selftest: tone file -> trim from 5 s -> m4a -> toml round trip. Writes music-selftest-log.txt next to the exe.</summary>
    public static int SelfTest()
    {
        var log = new List<string>(); int fails = 0;
        void Check(string what, bool ok, string extra = "") { log.Add((ok ? "PASS " : "FAIL ") + what + (extra.Length > 0 ? "  " + extra : "")); if (!ok) fails++; }
        try
        {
            var dir = Path.Combine(Paths.Root, "music-selftest"); Directory.CreateDirectory(dir);
            var src = Path.Combine(dir, "tone.wav");
            const double len = 20;
            using (var w = new WaveFileWriter(src, new WaveFormat(44100, 16, 2)))
                for (int i = 0; i < 44100 * len; i++)
                {
                    short v = (short)(9000 * Math.Sin(2 * Math.PI * (i / 44100.0 < 5 ? 220 : 440) * i / 44100.0));
                    w.WriteByte((byte)(v & 255)); w.WriteByte((byte)(v >> 8)); w.WriteByte((byte)(v & 255)); w.WriteByte((byte)(v >> 8));
                }
            var before = new FileInfo(src).Length;
            var mdir = Path.Combine(Paths.Mods, "music");
            Directory.CreateDirectory(mdir);
            var toml = TomlFile(mdir);
            File.WriteAllLines(toml, new[] { "# my own notes", "volume = 0.8", "loop = true", "", "[fighters]", "5 = \"hand.mp3\"  # keep me", "\"*\" = \"any.mp3\"", "", "[[track]]", "file = \"spider.mp3\"", "fighters = [22, 309,", "  1005]" });
            var clip = Decode(src, 5, 100);
            Check("decoded from 5 s", Math.Abs(clip.Seconds - (len - 5)) < 0.1, $"{clip.Seconds:0.00} s");
            var cut = Decode(src, 5, 100, 12);
            float tail = 0; for (int i = cut.Samples.Length - 200; i < cut.Samples.Length; i++) tail = Math.Max(tail, Math.Abs(cut.Samples[i]));
            Check("cut 5 s to 12 s = 7 s, fades out at the end", Math.Abs(cut.Seconds - 7) < 0.1 && tail < 0.01, $"{cut.Seconds:0.00} s, tail {tail:0.0000}");
            var pk = ComputePeaks(src);
            Check("peaks cover the whole song", Math.Abs(pk.Seconds - len) < 0.1 && pk.Max.Length >= len / Peaks.Step - 2 && pk.Max.Max() > 0.1f, $"{pk.Seconds:0.00} s, {pk.Max.Length} columns");
            var f = SafeName("Test Fighter");
            var outPath = Path.Combine(mdir, f);
            EncodeM4a(clip, outPath);
            var dur = DurationOf(outPath);
            Check("m4a written, duration = source - 5 s", File.Exists(outPath) && Math.Abs(dur - (len - 5)) < 0.3, $"{dur:0.00} s, {new FileInfo(outPath).Length / 1024} KB");
            Check("original file untouched", new FileInfo(src).Length == before);
            SetEntry(toml, new Entry("Test Fighter", f, new() { 7, 1007 }, 5, 100, src));
            var after = ReadLines(toml);
            var m = Parse(after);
            Check("toml parses as the game does", m.Tracks.GetValueOrDefault(7) == f && m.Tracks.GetValueOrDefault(1007) == f && m.Tracks.GetValueOrDefault(5) == "hand.mp3"
                && m.Tracks.GetValueOrDefault(22) == "spider.mp3" && m.Tracks.GetValueOrDefault(1005) == "spider.mp3" && m.Any == "any.mp3");
            Check("comments and other entries kept", after.Contains("# my own notes") && after.Any(l => l.Contains("# keep me")) && after.Contains("volume = 0.8"));
            SetEntry(toml, new Entry("Test Fighter", f, new() { 7, 1007 }, 6, 90, src, 14.5));
            Check("saving again replaces, no duplicate", OwnEntries(ReadLines(toml)).Count == 1 && ReadLines(toml).Count(l => l.StartsWith(Marker)) == 1);
            var own = OwnEntries(ReadLines(toml)).Single();
            Check("start and loudness remembered", own.Start == 6 && own.Loudness == 90 && own.Source == src && own.End == 14.5);
            var gone = RemoveEntry(toml, "Test Fighter");
            if (gone != null) File.Delete(Path.Combine(mdir, gone));
            m = Parse(ReadLines(toml));
            Check("remove: entry and m4a gone, rest kept", !m.Tracks.ContainsKey(7) && !File.Exists(outPath) && m.Tracks.GetValueOrDefault(5) == "hand.mp3" && m.Tracks[22] == "spider.mp3");
            Check("original still there after remove", File.Exists(src));
            Check("ufc3_fighter_music switched on", EnableInGame() && new Toml().GetBool("ufc3_fighter_music", false));
            if (Paths.AssetsReady)
            {
                var r = Roster.Load(Paths.Assets, Paths.Mods);
                Check("roster read from game files", r.Count > 100, $"{r.Count} fighters, e.g. " + string.Join(", ", r.Take(3).Select(x => $"{x.Name} [{string.Join(',', x.Ids)}]")));
                var gsp = r.FirstOrDefault(x => x.Name.Contains("St-Pierre", StringComparison.OrdinalIgnoreCase));
                if (gsp != null) log.Add($"INFO {gsp.Name}: ids {string.Join(',', gsp.Ids)}");
            }
            else log.Add("INFO no game files here, roster not tested");
        }
        catch (Exception ex) { log.Add("FAIL exception: " + ex); fails++; }
        log.Add(fails == 0 ? "ALL PASS" : $"{fails} FAILED");
        File.WriteAllLines(Path.Combine(Paths.Root, "music-selftest-log.txt"), log);
        return fails == 0 ? 0 : 1;
    }
}
