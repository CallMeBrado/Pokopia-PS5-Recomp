// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace PokopiaPS5Builder.Standalone
{
    internal static class Launcher
    {
        const string Gui = "PokopiaPS5Builder.exe";
        const string Prefix = "PokopiaPS5Builder/";
        static readonly StringComparer Paths = StringComparer.OrdinalIgnoreCase;
        static readonly Assembly Bundle = typeof(Launcher).Assembly;

        [STAThread]
        static void Main()
        {
            try
            {
                string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(local)) throw new IOException("Your local application data folder is unavailable.");
                string installed = EnsurePackage(Path.Combine(local, "PokopiaPS5Builder", "packages"));
                Process.Start(new ProcessStartInfo(Path.Combine(installed, Gui)) { UseShellExecute = false, WorkingDirectory = installed });
            }
            catch (Exception e)
            {
                MessageBox.Show("The builder could not start.\n\n" + e.Message, "Pokopia PS5 Builder", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        sealed class FileRecord
        {
            internal string Hash;
            internal long Length;
        }

        static string Hex(byte[] bytes) => BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
        static string Hash(Stream stream)
        {
            using (var sha = SHA256.Create()) return Hex(sha.ComputeHash(stream));
        }
        static Stream Resource(string name) => Bundle.GetManifestResourceStream("PokopiaPS5Builder." + name)
            ?? throw new InvalidDataException("Missing embedded builder component: " + name);
        static bool IsHash(string value) => value != null && value.Length == 64
            && Array.TrueForAll(value.ToCharArray(), c => c >= '0' && c <= '9' || c >= 'a' && c <= 'f');

        static void CheckRelativePath(string name)
        {
            if (string.IsNullOrEmpty(name) || name.Contains("\\")) throw new InvalidDataException("Invalid package path: " + name);
            foreach (string part in name.Split('/'))
            {
                if (part.Length == 0 || part.EndsWith(".", StringComparison.Ordinal) || part.EndsWith(" ", StringComparison.Ordinal)
                    || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                    throw new InvalidDataException("Invalid package path: " + name);
                string stem = part.Split('.')[0].TrimEnd().ToUpperInvariant();
                if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$"
                    || (stem.Length == 4 && (stem.StartsWith("COM", StringComparison.Ordinal) || stem.StartsWith("LPT", StringComparison.Ordinal))
                        && "0123456789¹²³".IndexOf(stem[3]) >= 0))
                    throw new InvalidDataException("Reserved package path: " + name);
            }
        }

        static void Plain(string path)
        {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("A package path is a reparse point: " + path);
        }

        static void EnsureDirectory(string root, string path, HashSet<string> checkedDirs, bool create)
        {
            if (checkedDirs.Contains(path)) return;
            if (!Paths.Equals(path, root)) EnsureDirectory(root, Path.GetDirectoryName(path), checkedDirs, create);
            if (create) Directory.CreateDirectory(path);
            Plain(path);
            checkedDirs.Add(path);
        }

        static Dictionary<string, FileRecord> ReadManifest(out string payloadHash, out string toolchainHash)
        {
            var files = new Dictionary<string, FileRecord>(Paths);
            using (var input = new StreamReader(Resource("Manifest"), Encoding.UTF8))
            {
                payloadHash = input.ReadLine();
                toolchainHash = input.ReadLine();
                if (!IsHash(payloadHash) || !IsHash(toolchainHash)) throw new InvalidDataException("Invalid embedded package identities.");
                string line;
                while ((line = input.ReadLine()) != null)
                {
                    string[] fields = line.Split('\t');
                    if (fields.Length != 3 || !IsHash(fields[0]) || !long.TryParse(fields[1], NumberStyles.None,
                        CultureInfo.InvariantCulture, out long length)) throw new InvalidDataException("Invalid embedded file manifest.");
                    CheckRelativePath(fields[2]);
                    files.Add(fields[2], new FileRecord { Hash = fields[0], Length = length });
                }
            }
            if (!files.ContainsKey(Gui) || !files.ContainsKey("toolchain/VERSION")) throw new InvalidDataException("Incomplete builder manifest.");
            return files;
        }

        static bool Matches(string root, Dictionary<string, FileRecord> files)
        {
            if (!Directory.Exists(root)) return false;
            var dirs = new HashSet<string>(Paths);
            foreach (var pair in files)
            {
                string path = Path.Combine(root, pair.Key.Replace('/', Path.DirectorySeparatorChar));
                if (!File.Exists(path)) return false;
                EnsureDirectory(root, Path.GetDirectoryName(path), dirs, false);
                Plain(path);
                using (var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete))
                    if (stream.Length != pair.Value.Length || Hash(stream) != pair.Value.Hash) return false;
            }
            return true;
        }

        static void Extract(string resource, string digest, bool toolchain, string root,
            Dictionary<string, FileRecord> files, HashSet<string> extracted)
        {
            var dirs = new HashSet<string>(Paths);
            using (Stream input = Resource(resource))
            {
                if (Hash(input) != digest) throw new InvalidDataException("Damaged embedded package: " + resource);
                input.Position = 0;
                using (var archive = new ZipArchive(input, ZipArchiveMode.Read))
                foreach (var entry in archive.Entries)
                {
                    string prefix = toolchain ? "toolchain/" : Prefix;
                    if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal)) throw new InvalidDataException("Unexpected archive root.");
                    string name = toolchain ? entry.FullName : entry.FullName.Substring(prefix.Length);
                    bool directory = entry.FullName.EndsWith("/", StringComparison.Ordinal);
                    if (directory && name.Length != 0) name = name.Substring(0, name.Length - 1);
                    if (directory && name.Length == 0) continue;
                    CheckRelativePath(name);
                    int kind = (entry.ExternalAttributes >> 16) & 0xf000;
                    if ((entry.ExternalAttributes & (int)FileAttributes.ReparsePoint) != 0 || (kind != 0 && kind != (directory ? 0x4000 : 0x8000)))
                        throw new InvalidDataException("Unsupported archive entry: " + name);
                    string path = Path.Combine(root, name.Replace('/', Path.DirectorySeparatorChar));
                    if (directory) { EnsureDirectory(root, path, dirs, true); continue; }
                    if (!files.TryGetValue(name, out FileRecord record) || entry.Length != record.Length || !extracted.Add(name))
                        throw new InvalidDataException("Unexpected entry in package: " + name);
                    EnsureDirectory(root, Path.GetDirectoryName(path), dirs, true);
                    using (var data = entry.Open())
                    using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    using (var sha = SHA256.Create())
                    using (var hashed = new CryptoStream(output, sha, CryptoStreamMode.Write))
                    {
                        data.CopyTo(hashed);
                        hashed.FlushFinalBlock();
                        if (output.Length != record.Length || Hex(sha.Hash) != record.Hash) throw new InvalidDataException("Damaged file: " + name);
                    }
                }
            }
        }

        // Does not start a process or display UI. Smoke tests can call it with a temporary cache root.
        internal static string EnsurePackage(string packages)
        {
            var files = ReadManifest(out string payload, out string toolchain);
            string key;
            using (var sha = SHA256.Create()) key = Hex(sha.ComputeHash(Encoding.ASCII.GetBytes(payload + "\n" + toolchain)));
            packages = Path.GetFullPath(packages);
            Directory.CreateDirectory(packages);
            Plain(Path.GetDirectoryName(packages));
            Plain(packages);
            string target = Path.Combine(packages, key);
            using (var guard = new Mutex(false, @"Local\PokopiaPS5Builder-" + key))
            {
                try { guard.WaitOne(); } catch (AbandonedMutexException) { /* The new owner can retry from scratch. */ }
                try
                {
                    if (Matches(target, files)) return target;
                    string stage = Path.Combine(packages, ".install-" + Guid.NewGuid().ToString("N"));
                    Directory.CreateDirectory(stage);
                    try
                    {
                        var extracted = new HashSet<string>(Paths);
                        Extract("PayloadZip", payload, false, stage, files, extracted);
                        Extract("ToolchainZip", toolchain, true, stage, files, extracted);
                        if (extracted.Count != files.Count) throw new InvalidDataException("The embedded builder is incomplete.");
                        if (Directory.Exists(target))
                        {
                            Plain(target);
                            // Preserve the user's download cache and any other files in the damaged copy.
                            Directory.Move(target, target + ".previous-" + Guid.NewGuid().ToString("N"));
                        }
                        Directory.Move(stage, target);
                        return target;
                    }
                    finally { if (Directory.Exists(stage)) Directory.Delete(stage, true); }
                }
                finally { guard.ReleaseMutex(); }
            }
        }
    }
}
