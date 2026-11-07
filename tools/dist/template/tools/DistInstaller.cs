// Macross 30 QA distribution: data installer core.
// Compiled at run time by tools/install.ps1 (Windows PowerShell 5.1 Add-Type,
// so this file must stay C# 5: no string interpolation, no ?. operators).
//
// Reads the user's own disc image (ISO9660, same walk as tools/inspect_iso.py),
// writes the original disc files, rebuilds the Korean files from MKDELTA1
// deltas against the original disc files, builds the HDD install tree, and
// checks every written byte against the SHA-256 in korean/install-plan.tsv.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace MacrossDist
{
    public sealed class IsoFile
    {
        public string Path;
        public long Size;
        public readonly List<long> Offsets = new List<long>();
        public readonly List<long> Sizes = new List<long>();
    }

    public sealed class Iso : IDisposable
    {
        readonly FileStream stream;
        readonly Dictionary<string, IsoFile> files = new Dictionary<string, IsoFile>(StringComparer.Ordinal);
        readonly Dictionary<string, IsoFile> folded = new Dictionary<string, IsoFile>(StringComparer.OrdinalIgnoreCase);

        public Iso(string path)
        {
            stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
            byte[] pvd = ReadRaw(16L * 2048, 2048);
            if (pvd[0] != 1 || Encoding.ASCII.GetString(pvd, 1, 5) != "CD001" || pvd[6] != 1)
                throw new InvalidDataException("Not an ISO9660 disc image (no primary volume descriptor): " + path);
            byte[] root = new byte[34];
            Buffer.BlockCopy(pvd, 156, root, 0, 34);
            Walk(root, "", new HashSet<long>());
            foreach (IsoFile f in files.Values)
                if (!folded.ContainsKey(f.Path)) folded[f.Path] = f;
        }

        public long Length { get { return stream.Length; } }

        public IsoFile Find(string path)
        {
            IsoFile f;
            if (files.TryGetValue(path, out f)) return f;
            return folded.TryGetValue(path, out f) ? f : null;
        }

        byte[] ReadRaw(long offset, int count)
        {
            byte[] data = new byte[count];
            stream.Position = offset;
            int done = 0;
            while (done < count)
            {
                int n = stream.Read(data, done, count - done);
                if (n <= 0) throw new EndOfStreamException("Disc image is truncated");
                done += n;
            }
            return data;
        }

        void Walk(byte[] record, string parent, HashSet<long> visited)
        {
            long extent = BitConverter.ToUInt32(record, 2);
            int size = (int)BitConverter.ToUInt32(record, 10);
            if (!visited.Add(extent)) return;
            byte[] data = ReadRaw(extent * 2048, size);
            int pos = 0;
            while (pos < data.Length)
            {
                int length = data[pos];
                if (length == 0) { pos = (pos / 2048 + 1) * 2048; continue; }
                if (pos + length > data.Length) break;
                byte[] rec = new byte[length];
                Buffer.BlockCopy(data, pos, rec, 0, length);
                pos += length;
                int nameLength = rec[32];
                if (nameLength == 1 && (rec[33] == 0 || rec[33] == 1)) continue;
                string name = Encoding.ASCII.GetString(rec, 33, nameLength);
                int semicolon = name.IndexOf(';');
                if (semicolon >= 0) name = name.Substring(0, semicolon);
                string path = parent.Length == 0 ? name : parent + "/" + name;
                if ((rec[25] & 2) != 0) { Walk(rec, path, visited); continue; }
                IsoFile file;
                if (!files.TryGetValue(path, out file))
                {
                    file = new IsoFile();
                    file.Path = path;
                    files[path] = file;
                }
                long extentSize = BitConverter.ToUInt32(rec, 10);
                file.Offsets.Add((long)BitConverter.ToUInt32(rec, 2) * 2048);
                file.Sizes.Add(extentSize);
                file.Size += extentSize;
            }
        }

        // Reads count bytes of a file starting at offset (across multi-extent files).
        public void Read(IsoFile file, long offset, byte[] buffer, int count)
        {
            if (offset < 0 || count < 0 || offset + count > file.Size)
                throw new ArgumentOutOfRangeException("offset", "Read outside disc file " + file.Path);
            int done = 0;
            for (int i = 0; i < file.Offsets.Count && done < count; i++)
            {
                long extentSize = file.Sizes[i];
                if (offset >= extentSize) { offset -= extentSize; continue; }
                int amount = (int)Math.Min(count - done, extentSize - offset);
                stream.Position = file.Offsets[i] + offset;
                int got = 0;
                while (got < amount)
                {
                    int n = stream.Read(buffer, done + got, amount - got);
                    if (n <= 0) throw new EndOfStreamException("Disc image is truncated in " + file.Path);
                    got += n;
                }
                done += amount;
                offset = 0;
            }
            if (done != count) throw new EndOfStreamException("Short read in " + file.Path);
        }

        public void Dispose() { stream.Dispose(); }
    }

    public sealed class PlanRow
    {
        public string Kind;    // dir, iso, delta, copy
        public string Target;  // relative to the install root, '/' separated
        public long Size;
        public string Sha256;
        public string Source;  // iso: disc path; delta: disc path of the original; copy: plan target
        public string Delta;   // delta: relative to the distribution root
    }

    sealed class HashSink : IDisposable
    {
        readonly SHA256 sha = SHA256.Create();
        readonly Stream output;
        public long Length;
        public HashSink(Stream output) { this.output = output; }
        public void Write(byte[] buffer, int count)
        {
            sha.TransformBlock(buffer, 0, count, null, 0);
            if (output != null) output.Write(buffer, 0, count);
            Length += count;
        }
        public string Finish()
        {
            sha.TransformFinalBlock(new byte[0], 0, 0);
            return Installer.Hex(sha.Hash);
        }
        public void Dispose() { sha.Dispose(); }
    }

    public static class Installer
    {
        const int Chunk = 8 << 20;
        static readonly byte[] Magic = Encoding.ASCII.GetBytes("MKDELTA1");

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateHardLink(string newFile, string existingFile, IntPtr securityAttributes);

        public static string Hex(byte[] data)
        {
            StringBuilder sb = new StringBuilder(data.Length * 2);
            foreach (byte b in data) sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
            return sb.ToString();
        }

        static void Log(string text) { Console.WriteLine(text); }

        public static List<PlanRow> LoadPlan(string path)
        {
            List<PlanRow> rows = new List<PlanRow>();
            foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0 || line[0] == '#') continue;
                string[] f = line.Split('\t');
                if (f.Length != 6) throw new InvalidDataException("Bad plan line: " + line);
                PlanRow row = new PlanRow();
                row.Kind = f[0]; row.Target = f[1];
                row.Size = f[2].Length == 0 ? 0 : long.Parse(f[2], CultureInfo.InvariantCulture);
                row.Sha256 = f[3]; row.Source = f[4]; row.Delta = f[5];
                if (row.Target.Contains("..") || Path.IsPathRooted(row.Target))
                    throw new InvalidDataException("Unsafe plan target: " + row.Target);
                rows.Add(row);
            }
            return rows;
        }

        static string Full(string root, string relative)
        {
            return Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
        }

        static bool Matches(string pattern, string target)
        {
            if (string.IsNullOrEmpty(pattern)) return true;
            foreach (string p in pattern.Split(','))
            {
                string q = p.Trim();
                if (q.Length == 0) continue;
                string rx = "^" + System.Text.RegularExpressions.Regex.Escape(q).Replace("\\*", ".*").Replace("\\?", ".") + "$";
                if (System.Text.RegularExpressions.Regex.IsMatch(target, rx, System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                    return true;
            }
            return false;
        }

        // Install state cache: target -> size, last write ticks, verified sha256.
        static Dictionary<string, string[]> LoadState(string path)
        {
            Dictionary<string, string[]> state = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
            if (!File.Exists(path)) return state;
            foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
            {
                string[] f = line.Split('\t');
                if (f.Length == 4) state[f[0]] = f;
            }
            return state;
        }

        static void SaveState(string path, Dictionary<string, string[]> state)
        {
            List<string> lines = new List<string>();
            foreach (string[] f in state.Values) lines.Add(string.Join("\t", f));
            lines.Sort(StringComparer.Ordinal);
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllLines(path, lines.ToArray(), new UTF8Encoding(false));
        }

        public static string HashFile(string path)
        {
            using (FileStream s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            using (HashSink sink = new HashSink(null))
            {
                byte[] buffer = new byte[Chunk];
                int n;
                while ((n = s.Read(buffer, 0, buffer.Length)) > 0) sink.Write(buffer, n);
                return sink.Finish();
            }
        }

        static void ExtractIso(Iso iso, IsoFile file, HashSink sink)
        {
            byte[] buffer = new byte[Chunk];
            for (long offset = 0; offset < file.Size; offset += Chunk)
            {
                int count = (int)Math.Min(Chunk, file.Size - offset);
                iso.Read(file, offset, buffer, count);
                sink.Write(buffer, count);
            }
        }

        sealed class DeltaHeader
        {
            public int Block;
            public long SourceSize, TargetSize, LiteralSize, LiteralStart;
            public string SourceSha, TargetSha;
            public byte[] Kinds;
            public long[] Offsets, Lengths;
        }

        static DeltaHeader ReadDelta(FileStream fs)
        {
            BinaryReader br = new BinaryReader(fs);
            byte[] magic = br.ReadBytes(8);
            for (int i = 0; i < 8; i++)
                if (magic.Length != 8 || magic[i] != Magic[i]) throw new InvalidDataException("Not an MKDELTA1 file: " + fs.Name);
            if (br.ReadUInt32() != 1) throw new InvalidDataException("Unsupported delta version: " + fs.Name);
            DeltaHeader h = new DeltaHeader();
            h.Block = (int)br.ReadUInt32();
            h.SourceSize = br.ReadInt64(); h.SourceSha = Hex(br.ReadBytes(32));
            h.TargetSize = br.ReadInt64(); h.TargetSha = Hex(br.ReadBytes(32));
            uint count = br.ReadUInt32();
            h.Kinds = new byte[count]; h.Offsets = new long[count]; h.Lengths = new long[count];
            for (uint i = 0; i < count; i++)
            {
                h.Kinds[i] = br.ReadByte(); h.Offsets[i] = br.ReadInt64(); h.Lengths[i] = br.ReadInt64();
            }
            h.LiteralSize = br.ReadInt64();
            h.LiteralStart = 8 + 4 + 4 + 8 + 32 + 8 + 32 + 4 + 17L * count + 8;
            return h;
        }

        static void ApplyDelta(Iso iso, IsoFile source, string deltaPath, HashSink sink)
        {
            using (FileStream fs = new FileStream(deltaPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            {
                DeltaHeader h = ReadDelta(fs);
                if (h.SourceSize != source.Size)
                    throw new InvalidDataException("Delta source size differs from the disc file " + source.Path);
                fs.Position = h.LiteralStart;
                byte[] buffer = new byte[Chunk];
                using (DeflateStream literal = new DeflateStream(fs, CompressionMode.Decompress, true))
                {
                    for (int i = 0; i < h.Kinds.Length; i++)
                    {
                        long done = 0;
                        while (done < h.Lengths[i])
                        {
                            int count = (int)Math.Min(Chunk, h.Lengths[i] - done);
                            if (h.Kinds[i] == 0)
                            {
                                iso.Read(source, h.Offsets[i] + done, buffer, count);
                            }
                            else if (h.Kinds[i] == 1)
                            {
                                int got = 0;
                                while (got < count)
                                {
                                    int n = literal.Read(buffer, got, count - got);
                                    if (n <= 0) throw new EndOfStreamException("Delta literal stream ended early: " + deltaPath);
                                    got += n;
                                }
                            }
                            else throw new InvalidDataException("Bad delta op in " + deltaPath);
                            sink.Write(buffer, count);
                            done += count;
                        }
                    }
                }
                if (sink.Length != h.TargetSize) throw new InvalidDataException("Delta produced a wrong size: " + deltaPath);
            }
        }

        static void CopyFile(string source, HashSink sink)
        {
            using (FileStream s = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            {
                byte[] buffer = new byte[Chunk];
                int n;
                while ((n = s.Read(buffer, 0, buffer.Length)) > 0) sink.Write(buffer, n);
            }
        }

        // mode: install | dryrun | verify. Returns the number of problems (0 = success).
        public static int Run(string distRoot, string targetRoot, string planPath, string isoPath,
                              string mode, bool linkHdd, string only, string dataVersion)
        {
            List<PlanRow> plan = LoadPlan(planPath);
            Dictionary<string, PlanRow> byTarget = new Dictionary<string, PlanRow>(StringComparer.OrdinalIgnoreCase);
            foreach (PlanRow row in plan) byTarget[row.Target] = row;
            bool verify = mode == "verify", dry = mode == "dryrun";
            if (!verify && !dry && mode != "install") throw new ArgumentException("mode");
            string statePath = Full(targetRoot, "data/.install-state.tsv");
            Dictionary<string, string[]> state = LoadState(statePath);
            Iso iso = null;
            int problems = 0;
            long bytesDone = 0;
            DateTime started = DateTime.UtcNow;
            try
            {
                if (!verify)
                {
                    iso = new Iso(isoPath);
                    // Pre-flight: every disc file the plan needs exists with the expected size.
                    long needed = 0;
                    foreach (PlanRow row in plan)
                    {
                        if (row.Kind == "dir" || !Matches(only, row.Target)) continue;
                        string existing = Full(targetRoot, row.Target);
                        bool present = File.Exists(existing) && new FileInfo(existing).Length == row.Size;
                        if (!present && !(row.Kind == "copy" && linkHdd)) needed += row.Size;
                        if (row.Kind == "copy") continue;
                        IsoFile f = iso.Find(row.Source);
                        if (f == null)
                            throw new InvalidDataException("This disc image lacks " + row.Source + " (not Macross 30 BLJS10184?)");
                        if (row.Kind == "iso" && f.Size != row.Size)
                            throw new InvalidDataException("Disc file size differs: " + row.Source + " (different disc revision?)");
                        if (row.Kind == "delta")
                        {
                            using (FileStream fs = File.OpenRead(Full(distRoot, row.Delta)))
                            {
                                DeltaHeader h = ReadDelta(fs);
                                if (h.SourceSize != f.Size || h.TargetSize != row.Size || h.TargetSha != row.Sha256)
                                    throw new InvalidDataException("Korean pack does not match this plan or disc: " + row.Delta);
                            }
                        }
                    }
                    long free = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(targetRoot))).AvailableFreeSpace;
                    Log(string.Format(CultureInfo.InvariantCulture, "Disc image OK ({0:N0} bytes). Data still to write: {1:N1} GB, free space: {2:N1} GB",
                        iso.Length, needed / 1e9, free / 1e9));
                    if (!dry && free < needed + (1L << 30))
                        throw new IOException(string.Format(CultureInfo.InvariantCulture,
                            "Not enough free disk space: need {0:N1} GB (+1 GB margin), have {1:N1} GB", needed / 1e9, free / 1e9));
                }
                int index = 0;
                foreach (PlanRow row in plan)
                {
                    index++;
                    if (!Matches(only, row.Target)) continue;
                    string full = Full(targetRoot, row.Target);
                    string tag = string.Format(CultureInfo.InvariantCulture, "[{0}/{1}] {2}", index, plan.Count, row.Target);
                    if (row.Kind == "dir")
                    {
                        if (!dry && !verify) Directory.CreateDirectory(full);
                        continue;
                    }
                    if (verify)
                    {
                        if (!File.Exists(full)) { Log(tag + ": MISSING"); problems++; continue; }
                        string actual = HashFile(full);
                        bool ok = actual == row.Sha256 && new FileInfo(full).Length == row.Size;
                        Log(tag + (ok ? ": ok" : ": MISMATCH " + actual));
                        if (!ok) problems++;
                        continue;
                    }
                    if (!dry && File.Exists(full))
                    {
                        FileInfo info = new FileInfo(full);
                        string[] cached;
                        if (info.Length == row.Size && state.TryGetValue(row.Target, out cached) &&
                            cached[1] == info.Length.ToString(CultureInfo.InvariantCulture) &&
                            cached[2] == info.LastWriteTimeUtc.Ticks.ToString(CultureInfo.InvariantCulture) &&
                            cached[3] == row.Sha256)
                        {
                            Log(tag + ": already installed");
                            continue;
                        }
                        if (info.Length == row.Size && HashFile(full) == row.Sha256)
                        {
                            state[row.Target] = new string[] { row.Target, info.Length.ToString(CultureInfo.InvariantCulture),
                                info.LastWriteTimeUtc.Ticks.ToString(CultureInfo.InvariantCulture), row.Sha256 };
                            Log(tag + ": already installed (verified)");
                            continue;
                        }
                    }
                    if (dry && row.Kind == "copy")
                    {
                        PlanRow src;
                        bool ok = byTarget.TryGetValue(row.Source, out src) && src.Sha256 == row.Sha256 && src.Size == row.Size;
                        Log(tag + (ok ? ": copy of " + row.Source + " (hash identical in plan)" : ": BAD COPY SOURCE"));
                        if (!ok) problems++;
                        continue;
                    }
                    if (!dry) Directory.CreateDirectory(Path.GetDirectoryName(full));
                    string partial = full + ".partial";
                    string actualSha;
                    if (!dry && row.Kind == "copy" && linkHdd)
                    {
                        string existing = Full(targetRoot, row.Source);
                        if (File.Exists(full)) File.Delete(full);
                        if (!CreateHardLink(full, existing, IntPtr.Zero))
                            throw new IOException("CreateHardLink failed (" + Marshal.GetLastWin32Error() + "): " + full);
                        actualSha = HashFile(full);
                    }
                    else
                    {
                        using (FileStream output = dry ? null : new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                        using (HashSink sink = new HashSink(output))
                        {
                            if (row.Kind == "iso") ExtractIso(iso, iso.Find(row.Source), sink);
                            else if (row.Kind == "delta") ApplyDelta(iso, iso.Find(row.Source), Full(distRoot, row.Delta), sink);
                            else if (row.Kind == "copy") CopyFile(Full(targetRoot, row.Source), sink);
                            else throw new InvalidDataException("Unknown plan kind: " + row.Kind);
                            actualSha = sink.Finish();
                        }
                    }
                    bytesDone += row.Size;
                    if (actualSha != row.Sha256)
                    {
                        if (!dry && File.Exists(partial)) File.Delete(partial);
                        Log(tag + ": HASH MISMATCH (expected " + row.Sha256 + ", got " + actualSha + ")");
                        problems++;
                        if (!dry)
                            throw new InvalidDataException("Hash mismatch for " + row.Target +
                                ". The disc image is not the expected decrypted BLJS10184 dump (encrypted Redump images must be decrypted first).");
                        continue;
                    }
                    if (!dry && !(row.Kind == "copy" && linkHdd))
                    {
                        if (File.Exists(full)) File.Delete(full);
                        File.Move(partial, full);
                    }
                    if (!dry)
                    {
                        FileInfo done = new FileInfo(full);
                        state[row.Target] = new string[] { row.Target, done.Length.ToString(CultureInfo.InvariantCulture),
                            done.LastWriteTimeUtc.Ticks.ToString(CultureInfo.InvariantCulture), row.Sha256 };
                        SaveState(statePath, state);
                    }
                    double seconds = Math.Max(0.001, (DateTime.UtcNow - started).TotalSeconds);
                    Log(string.Format(CultureInfo.InvariantCulture, "{0}: ok ({1}, {2:N0} bytes) total {3:N1} GB, {4:N0} MB/s",
                        tag, row.Kind, row.Size, bytesDone / 1e9, bytesDone / 1e6 / seconds));
                }
                if (!dry && !verify && problems == 0 && string.IsNullOrEmpty(only))
                {
                    SaveState(statePath, state);
                    File.WriteAllText(Full(targetRoot, "data/installed-version.txt"), dataVersion + "\r\n", new UTF8Encoding(false));
                }
            }
            finally
            {
                if (iso != null) iso.Dispose();
            }
            return problems;
        }
    }
}
