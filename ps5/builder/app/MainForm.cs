// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia PS5 Builder: the window. Picks the dump, the output folder and the formats, runs
// builder\pipeline.py with the bundled Python and shows its "@progress" lines.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace PokopiaPS5Builder
{
    internal sealed class MainForm : Form
    {
        // pipeline.py's steps, in order, with the names shown here
        static readonly (string Key, string Text)[] Steps =
        {
            ("verify", "Check the game files"),
            ("tools", "Get the build tools"),
            ("source", "Prepare the port's sources"),
            ("lift", "Translate the game code"),
            ("generate", "Prepare the modules"),
            ("shaders", "Pre-compile every shader"),
            ("build", "Build eboot.bin"),
            ("title", "Assemble the title folder"),
            ("exfat", "Write the exFAT image"),
            ("ffpkg", "Write the UFS2 image (.ffpkg)"),
            ("ftp", "Upload the title to PS5"),
        };

        readonly string appDir = AppDomain.CurrentDomain.BaseDirectory;
        readonly string settingsPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "PokopiaPS5Builder", "settings.txt");

        TextBox dumpBox, outBox, workBox, logBox, ps5AddressBox;
        Label dumpState, workState, stepLabel, timeLabel, iconLabel;
        CheckBox folderCheck, exfatCheck, ffpkgCheck, ftpCheck, keepCheck;
        NumericUpDown jobsBox;
        PictureBox iconPreview;
        ProgressBar bar;
        Button buildButton, stopButton, openButton, pickIconButton, automaticIconButton;
        ListView stepList;
        Process proc;
        string iconPath;
        string automaticIconPath;
        string iconDump;
        DateTime started;
        readonly Timer clock = new Timer { Interval = 1000 };
        readonly List<string> outputs = new List<string>();
        string lastError;

        public MainForm()
        {
            Text = "Pokopia PS5 Builder";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(900, 720);
            MinimumSize = new Size(760, 600);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            BuildLayout();
            LoadSettings();
            clock.Tick += (s, e) => timeLabel.Text = Elapsed();
            FormClosing += OnClosing;
            UpdateDumpState();
            UpdateWorkState();
        }

        // --- layout -------------------------------------------------------------------------------
        void BuildLayout()
        {
            var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 1, RowCount = 5 };
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(root);

            var intro = new Label
            {
                AutoSize = true, MaximumSize = new Size(860, 0), Margin = new Padding(0, 0, 0, 8),
                Text = "Builds the PS5 version of Pokopia from your own dump of Pokopia 2.0.1 (update 655360): base\\ and "
                     + "updates\\655360\\ with decrypted exefs and romfs. Nothing of the game comes with this program. "
                     + "The first build takes one to three hours and needs about 45 GB free; the finished title is about 15 GB.",
            };
            root.Controls.Add(intro);

            // inputs
            var inputs = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3 };
            inputs.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            inputs.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            inputs.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            dumpBox = new TextBox { Dock = DockStyle.Fill };
            dumpBox.TextChanged += (s, e) => UpdateDumpState();
            outBox = new TextBox { Dock = DockStyle.Fill };
            inputs.Controls.Add(new Label { Text = "Pokopia dump:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0);
            inputs.Controls.Add(dumpBox, 1, 0);
            inputs.Controls.Add(Browse(dumpBox, "Pick the dump folder (the one that holds base and updates)"), 2, 0);
            dumpState = new Label { AutoSize = true, ForeColor = Color.DimGray, Margin = new Padding(3, 0, 3, 6) };
            inputs.Controls.Add(dumpState, 1, 1);
            inputs.Controls.Add(new Label { Text = "Local output folder:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2);
            inputs.Controls.Add(outBox, 1, 2);
            inputs.Controls.Add(Browse(outBox, "Pick where the PS5 title goes"), 2, 2);
            workBox = new TextBox { Dock = DockStyle.Fill };
            outBox.TextChanged += (s, e) => UpdateWorkState();
            workBox.TextChanged += (s, e) => UpdateWorkState();
            inputs.Controls.Add(new Label { Text = "Temporary files:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 3);
            inputs.Controls.Add(workBox, 1, 3);
            inputs.Controls.Add(Browse(workBox, "Pick a folder for the temporary files (an NTFS drive with about 45 GB free)"), 2, 3);
            workState = new Label { AutoSize = true, ForeColor = Color.DimGray, Margin = new Padding(3, 0, 3, 6) };
            inputs.Controls.Add(workState, 1, 4);
            root.Controls.Add(inputs);

            // options: formats, icon, jobs
            var options = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3, Margin = new Padding(0, 8, 0, 8) };
            options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));

            var formats = new GroupBox { Text = "Export as", Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(8) };
            var fflow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
            folderCheck = new CheckBox { Text = "Folder (PPSA94010) to copy to the PS5", AutoSize = true, Checked = true };
            exfatCheck = new CheckBox { Text = "exFAT image (.exfat) for ShadowMountPlus", AutoSize = true };
            ffpkgCheck = new CheckBox { Text = "UFS2 image (.ffpkg) for ShadowMountPlus", AutoSize = true };
            ftpCheck = new CheckBox { Text = "Upload directly to PS5 over FTP", AutoSize = true };
            var address = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(20, 0, 0, 0) };
            address.Controls.Add(new Label { Text = "PS5 IP address", AutoSize = true, Anchor = AnchorStyles.Left });
            ps5AddressBox = new TextBox { Width = 165, Enabled = false, AccessibleName = "PS5 IP address or host and port" };
            address.Controls.Add(ps5AddressBox);
            var ftpHint = new Label { Text = "Start the PS5 FTP server first. Default port: 2121.", AutoSize = true,
                                      ForeColor = Color.DimGray, Margin = new Padding(20, 0, 0, 0) };
            ftpCheck.CheckedChanged += (s, e) =>
            {
                ps5AddressBox.Enabled = ftpCheck.Checked && proc == null;
                ftpHint.Enabled = ftpCheck.Checked;
            };
            ftpHint.Enabled = false;
            keepCheck = new CheckBox { Text = "Keep the intermediate files (faster rebuilds, +30 GB)", AutoSize = true, Margin = new Padding(3, 10, 3, 3) };
            fflow.Controls.AddRange(new Control[] { folderCheck, exfatCheck, ffpkgCheck, ftpCheck, address, ftpHint, keepCheck });
            formats.Controls.Add(fflow);
            options.Controls.Add(formats, 0, 0);

            var iconGroup = new GroupBox { Text = "Home screen icon", Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(8) };
            var iflow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
            iconPreview = new PictureBox { Size = new Size(96, 96), SizeMode = PictureBoxSizeMode.Zoom, BorderStyle = BorderStyle.FixedSingle };
            iconLabel = new Label { AutoSize = true, MaximumSize = new Size(160, 0), ForeColor = Color.DimGray };
            var iconButtons = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
            pickIconButton = new Button { Text = "Choose...", AutoSize = true };
            pickIconButton.Click += (s, e) => PickIcon();
            automaticIconButton = new Button { Text = "Automatic", AutoSize = true };
            automaticIconButton.Click += (s, e) => SetIcon(null);
            iconButtons.Controls.AddRange(new Control[] { pickIconButton, automaticIconButton });
            iflow.Controls.AddRange(new Control[] { iconPreview, iconLabel, iconButtons });
            iconGroup.Controls.Add(iflow);
            options.Controls.Add(iconGroup, 1, 0);

            var jobsGroup = new GroupBox { Text = "CPU threads", Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(8) };
            jobsBox = new NumericUpDown { Minimum = 1, Maximum = 256, Value = Math.Max(1, Environment.ProcessorCount - 1), Width = 70 };
            var jflow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, AutoSize = true };
            jflow.Controls.Add(jobsBox);
            jflow.Controls.Add(new Label { AutoSize = true, MaximumSize = new Size(180, 0), ForeColor = Color.DimGray,
                                           Text = "Fewer threads keep the PC responsive while it builds." });
            jobsGroup.Controls.Add(jflow);
            options.Controls.Add(jobsGroup, 2, 0);
            root.Controls.Add(options);

            // progress: steps on the left, log on the right
            var split = new SplitContainer { Dock = DockStyle.Fill, SplitterDistance = 300, FixedPanel = FixedPanel.Panel1 };
            stepList = new ListView { Dock = DockStyle.Fill, View = View.Details, HeaderStyle = ColumnHeaderStyle.None,
                                      FullRowSelect = true, MultiSelect = false };
            stepList.Columns.Add("", 26);
            stepList.Columns.Add("", 250);
            foreach (var step in Steps)
                stepList.Items.Add(new ListViewItem(new[] { "", step.Text }) { Tag = step.Key });
            split.Panel1.Controls.Add(stepList);
            logBox = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both,
                                   WordWrap = false, Font = new Font("Consolas", 8.5f), BackColor = SystemColors.Window };
            split.Panel2.Controls.Add(logBox);
            root.Controls.Add(split);

            // bottom: current step, bar, buttons
            var bottom = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 5, Margin = new Padding(0, 8, 0, 0) };
            bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            for (int i = 0; i < 4; i++)
                bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            stepLabel = new Label { AutoSize = true, Text = "Ready.", Margin = new Padding(3, 3, 3, 6) };
            bottom.Controls.Add(stepLabel, 0, 0);
            bottom.SetColumnSpan(stepLabel, 4);
            timeLabel = new Label { AutoSize = true, Anchor = AnchorStyles.Right, ForeColor = Color.DimGray };
            bottom.Controls.Add(timeLabel, 4, 0);
            bar = new ProgressBar { Dock = DockStyle.Fill, Height = 22, Maximum = 1000 };
            bottom.Controls.Add(bar, 0, 1);
            openButton = new Button { Text = "Open output", AutoSize = true, Enabled = false };
            openButton.Click += (s, e) => OpenOutput();
            stopButton = new Button { Text = "Stop", AutoSize = true, Enabled = false };
            stopButton.Click += (s, e) => Stop();
            buildButton = new Button { Text = "Build", AutoSize = true, Font = new Font(Font, FontStyle.Bold) };
            buildButton.Click += (s, e) => Start();
            bottom.Controls.Add(openButton, 1, 1);
            bottom.Controls.Add(stopButton, 2, 1);
            bottom.Controls.Add(buildButton, 3, 1);
            root.Controls.Add(bottom);
            AcceptButton = buildButton;
            SetIcon(null);
        }

        Button Browse(TextBox target, string title)
        {
            var b = new Button { Text = "Browse...", AutoSize = true };
            b.Click += (s, e) =>
            {
                using (var dlg = new FolderBrowserDialog { Description = title, ShowNewFolderButton = true })
                {
                    if (Directory.Exists(target.Text))
                        dlg.SelectedPath = target.Text;
                    if (dlg.ShowDialog(this) == DialogResult.OK)
                        target.Text = dlg.SelectedPath;
                }
            };
            return b;
        }

        // --- dump check -----------------------------------------------------------------------------
        string ResolveDump(string path)
        {
            // accept the dump folder, its updates\655360 folder, or the exefs folder itself
            if (string.IsNullOrWhiteSpace(path))
                return null;
            string dir;
            try
            {
                dir = Path.GetFullPath(path.Trim());
                if (dir.Length > Path.GetPathRoot(dir).Length)
                    dir = dir.TrimEnd('\\', '/');
            }
            catch (Exception e) when (e is ArgumentException || e is NotSupportedException || e is PathTooLongException)
            {
                return null;
            }
            for (int i = 0; i < 4 && !string.IsNullOrEmpty(dir); i++)
            {
                if (File.Exists(Path.Combine(dir, "updates", "655360", "exefs", "main")))
                    return dir;
                dir = Path.GetDirectoryName(dir);
            }
            return null;
        }

        void UpdateDumpState()
        {
            var dump = ResolveDump(dumpBox.Text);
            if (!string.Equals(dump, iconDump, StringComparison.OrdinalIgnoreCase))
            {
                iconDump = dump;
                if (iconPath == null)
                    RefreshAutomaticIcon();
            }
            if (string.IsNullOrWhiteSpace(dumpBox.Text))
            {
                dumpState.Text = "The folder that holds base\\ and updates\\655360\\.";
                dumpState.ForeColor = Color.DimGray;
            }
            else if (dump == null)
            {
                dumpState.Text = "No updates\\655360\\exefs\\main here. Pick the folder that holds base\\ and updates\\.";
                dumpState.ForeColor = Color.Firebrick;
            }
            else if (!File.Exists(Path.Combine(dump, "updates", "655360", "romfs", "LI_C0_R.IDX")))
            {
                dumpState.Text = "exefs found, but updates\\655360\\romfs is missing or not extracted.";
                dumpState.ForeColor = Color.Firebrick;
            }
            else
            {
                dumpState.Text = "Dump found: " + dump + " (checked in full when the build starts).";
                dumpState.ForeColor = Color.SeaGreen;
            }
        }

        string WorkDir()
        {
            var work = workBox.Text.Trim();
            return work.Length > 0 ? work : Path.Combine(outBox.Text.Trim(), "work");
        }

        // The shader steps write hundreds of thousands of small files: fine on NTFS, very slow on exFAT/FAT32.
        void UpdateWorkState()
        {
            string format = null;
            long free = -1;
            try
            {
                var root = Path.GetPathRoot(Path.GetFullPath(WorkDir()));
                var drive = new DriveInfo(root);
                if (drive.IsReady) { format = drive.DriveFormat; free = drive.AvailableFreeSpace; }
            }
            catch { }
            var where = workBox.Text.Trim().Length > 0 ? "" : "Empty: a work folder inside the output folder. ";
            if (format == null)
            {
                workState.Text = where + "About 45 GB while it builds; an NTFS drive is much faster.";
                workState.ForeColor = Color.DimGray;
            }
            else if (format != "NTFS")
            {
                workState.Text = where + "That drive is " + format + ": the shader steps will be many times slower. "
                               + "Pick a folder on an NTFS drive here.";
                workState.ForeColor = Color.DarkOrange;
            }
            else
            {
                workState.Text = where + "NTFS, " + (free >> 30) + " GB free (needs about 45 GB).";
                workState.ForeColor = free >= (45L << 30) ? Color.SeaGreen : Color.DarkOrange;
            }
        }

        // --- icon ---------------------------------------------------------------------------------
        void PickIcon()
        {
            using (var dlg = new OpenFileDialog { Filter = "Images|*.png;*.jpg;*.jpeg;*.bmp;*.gif|All files|*.*", Title = "Choose the home screen icon" })
            {
                if (dlg.ShowDialog(this) == DialogResult.OK)
                    SetIcon(dlg.FileName);
            }
        }

        void SetIcon(string path)
        {
            if (path == null)
            {
                iconPath = null;
                RefreshAutomaticIcon();
            }
            else if (TryPreviewIcon(path, "Custom icon"))
                iconPath = path;
            else
                MessageBox.Show(this, "That image could not be read.", Text);
        }

        void RefreshAutomaticIcon()
        {
            automaticIconPath = FindDumpIcon(iconDump, path => TryPreviewIcon(path, "Icon from game dump"));
            if (automaticIconPath == null &&
                !TryPreviewIcon(Path.Combine(appDir, "builder", "default-icon0.png"), "Built-in icon"))
                SetIconPreview(null, "Icon unavailable");
        }

        static string FindDumpIcon(string dump, Func<string, bool> use)
        {
            if (dump == null)
                return null;
            foreach (var root in new[] { Path.Combine(dump, "updates", "655360"), Path.Combine(dump, "base"), dump })
            {
                var control = Path.Combine(root, "control");
                foreach (var directory in new[] { Path.Combine(control, "romfs"), control, root })
                {
                    var preferred = new[] { "icon_AmericanEnglish.dat", "icon_BritishEnglish.dat", "icon0.png", "icon.png" };
                    foreach (var name in preferred)
                    {
                        var path = Path.Combine(directory, name);
                        if (File.Exists(path) && use(path))
                            return path;
                    }
                    try
                    {
                        if (directory == root || !Directory.Exists(directory))
                            continue;
                        foreach (var path in Directory.GetFiles(directory, "icon_*.dat").OrderBy(p => p, StringComparer.OrdinalIgnoreCase))
                            if (use(path))
                                return path;
                    }
                    catch (IOException) { }
                    catch (UnauthorizedAccessException) { }
                }
            }
            return null;
        }

        bool TryPreviewIcon(string path, string caption)
        {
            try
            {
                using (var stream = File.OpenRead(path))
                using (var image = Image.FromStream(stream, false, true))
                    SetIconPreview(new Bitmap(image), caption);
                return true;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException ||
                                      e is ArgumentException || e is OutOfMemoryException)
            {
                return false;
            }
        }

        void SetIconPreview(Image image, string caption)
        {
            var previous = iconPreview.Image;
            iconPreview.Image = image;
            previous?.Dispose();
            iconLabel.Text = caption;
        }

        // The PS5 wants a 512x512 PNG: scale the chosen picture to cover the square, centred.
        string WriteIcon(string workDir)
        {
            var input = iconPath ?? automaticIconPath;
            if (input == null)
                return null;
            Directory.CreateDirectory(workDir);
            var dest = Path.Combine(workDir, "selected-icon0.png");
            using (var src = Image.FromFile(input))
            using (var bmp = new Bitmap(512, 512, PixelFormat.Format32bppArgb))
            using (var g = Graphics.FromImage(bmp))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.Clear(Color.Black);
                var scale = Math.Max(512.0 / src.Width, 512.0 / src.Height);
                int w = (int)Math.Round(src.Width * scale), h = (int)Math.Round(src.Height * scale);
                g.DrawImage(src, (512 - w) / 2, (512 - h) / 2, w, h);
                bmp.Save(dest, ImageFormat.Png);
            }
            return dest;
        }

        // --- running ------------------------------------------------------------------------------
        string Python()
        {
            var bundled = Path.Combine(appDir, "python", "python.exe");
            return File.Exists(bundled) ? bundled : "python";
        }

        void Start()
        {
            var dump = ResolveDump(dumpBox.Text);
            if (dump == null)
            {
                MessageBox.Show(this, "Pick your Pokopia dump folder first (it holds base\\ and updates\\655360\\).", Text);
                return;
            }
            var output = outBox.Text.Trim();
            if (output.Length == 0)
            {
                MessageBox.Show(this, "Pick an output folder first.", Text);
                return;
            }
            var formats = new List<string>();
            if (folderCheck.Checked) formats.Add("folder");
            if (exfatCheck.Checked) formats.Add("exfat");
            if (ffpkgCheck.Checked) formats.Add("ffpkg");
            if (ftpCheck.Checked) formats.Add("ftp");
            var ps5Address = ps5AddressBox.Text.Trim();
            if (ftpCheck.Checked && ps5Address.Length == 0)
            {
                MessageBox.Show(this, "Enter the PS5 IP address for FTP upload.", Text);
                ps5AddressBox.Focus();
                return;
            }
            if (formats.Count == 0)
            {
                MessageBox.Show(this, "Choose at least one export format.", Text);
                return;
            }
            try
            {
                Directory.CreateDirectory(output);
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The output folder cannot be created: " + e.Message, Text);
                return;
            }
            SaveSettings();
            string icon;
            try
            {
                if (iconPath == null)
                    RefreshAutomaticIcon();
                icon = WriteIcon(WorkDir());
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The icon could not be converted: " + e.Message, Text);
                return;
            }

            var args = new StringBuilder();
            args.Append("-I ").Append(Quote(Path.Combine(appDir, "builder", "pipeline.py")));
            args.Append(" --dump ").Append(Quote(dump)).Append(" --out ").Append(Quote(output));
            args.Append(" --formats ").Append(string.Join(",", formats));
            if (workBox.Text.Trim().Length > 0) args.Append(" --work ").Append(Quote(workBox.Text.Trim()));
            args.Append(" --jobs ").Append((int)jobsBox.Value);
            if (icon != null) args.Append(" --icon ").Append(Quote(icon));
            if (keepCheck.Checked) args.Append(" --keep");
            if (ftpCheck.Checked) args.Append(" --ps5-ip ").Append(Quote(ps5Address));

            foreach (ListViewItem item in stepList.Items)
            {
                var key = (string)item.Tag;
                bool used = (key != "exfat" && key != "ffpkg" && key != "ftp") || formats.Contains(key);
                item.Text = "";
                item.ForeColor = used ? SystemColors.WindowText : SystemColors.GrayText;
            }
            outputs.Clear();
            lastError = null;
            logBox.Clear();
            Log("Starting: " + Python() + " " + args);
            var info = new ProcessStartInfo(Python(), args.ToString())
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8, WorkingDirectory = appDir,
            };
            info.EnvironmentVariables["PYTHONUTF8"] = "1";
            info.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            try
            {
                proc = new Process { StartInfo = info };
                proc.OutputDataReceived += (s, e) => { if (e.Data != null) BeginInvoke(new Action(() => OnLine(e.Data))); };
                proc.ErrorDataReceived += (s, e) => { if (e.Data != null) BeginInvoke(new Action(() => Log(e.Data))); };
                proc.Exited += (s, e) =>
                {
                    var completed = (Process)s;
                    completed.WaitForExit();
                    BeginInvoke(new Action(OnExit));
                };
                proc.Start();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
                proc.EnableRaisingEvents = true;
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The build could not start: " + e.Message, Text);
                proc = null;
                return;
            }
            started = DateTime.Now;
            clock.Start();
            SetRunning(true);
            stepLabel.Text = "Starting...";
        }

        static string Quote(string s) => "\"" + s.Replace("\"", "\\\"") + "\"";

        void OnLine(string line)
        {
            if (!line.StartsWith("@"))
            {
                Log(line);
                return;
            }
            int space = line.IndexOf(' ');
            var kind = space < 0 ? line.Substring(1) : line.Substring(1, space - 1);
            var rest = space < 0 ? "" : line.Substring(space + 1).Replace("\\n", Environment.NewLine);
            switch (kind)
            {
                case "progress":
                    var parts = rest.Split(new[] { ' ' }, 4);
                    if (parts.Length == 4 && long.TryParse(parts[1], out var done) && long.TryParse(parts[2], out var total))
                        Progress(parts[0], done, total, parts[3]);
                    break;
                case "done":
                    outputs.Add(rest);
                    Log("Made: " + rest);
                    break;
                case "error":
                    lastError = rest;
                    Log("ERROR: " + rest);
                    break;
                case "cancelled":
                    Log(rest);
                    break;
                default:
                    Log(rest);
                    break;
            }
        }

        string currentStep;

        void Progress(string step, long done, long total, string text)
        {
            if (step != currentStep)
            {
                bool before = true;
                foreach (ListViewItem item in stepList.Items)
                {
                    var key = (string)item.Tag;
                    if (key == step) { before = false; item.Text = "▶"; item.EnsureVisible(); continue; }
                    if (before && item.ForeColor != SystemColors.GrayText) item.Text = "✓";
                }
                currentStep = step;
                Log(text);
            }
            stepLabel.Text = text;
            bar.Value = total > 0 ? (int)Math.Min(1000, done * 1000 / total) : 0;
        }

        void OnExit()
        {
            clock.Stop();
            int code = proc?.ExitCode ?? -1;
            proc = null;
            currentStep = null;
            SetRunning(false);
            timeLabel.Text = Elapsed();
            if (code == 0)
            {
                foreach (ListViewItem item in stepList.Items)
                    if (item.ForeColor != SystemColors.GrayText) item.Text = "✓";
                bar.Value = 1000;
                var uploaded = outputs.Any(IsFtpOutput);
                stepLabel.Text = uploaded ? "Done. Title uploaded to the PS5." : "Done. Local exports are ready.";
                var message = "The PS5 title is ready:\n\n" + string.Join("\n", outputs);
                if (uploaded)
                    message += "\n\nOn the PS5, open your homebrew launcher to start the uploaded title.";
                if (outputs.Any(path => File.Exists(path) && !IsFtpOutput(path)))
                    message += "\n\nUse ShadowMountPlus to open the exported image.";
                if (!uploaded && outputs.Any(Directory.Exists))
                    message += "\n\nCopy PPSA94010 to your PS5 for use with your homebrew launcher.";
                MessageBox.Show(this, message, Text);
            }
            else if (code == 2)
                stepLabel.Text = "Stopped. Press Build to continue where it stopped.";
            else
            {
                stepLabel.Text = "The build failed. See the log; press Build to try again (it continues where it stopped).";
                MessageBox.Show(this, lastError ?? "The build stopped with exit code " + code + ". See the log.", Text,
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        void Stop()
        {
            if (proc == null)
                return;
            try
            {
                var work = WorkDir();
                Directory.CreateDirectory(work);
                File.WriteAllText(Path.Combine(work, "cancel"), "stop\n");
                stepLabel.Text = "Stopping...";
                stopButton.Enabled = false;
            }
            catch (Exception e)
            {
                Log("Could not ask the build to stop: " + e.Message);
            }
        }

        void SetRunning(bool running)
        {
            buildButton.Enabled = !running;
            stopButton.Enabled = running;
            openButton.Enabled = !running && outputs.Any(path => !IsFtpOutput(path));
            foreach (var c in new Control[] { dumpBox, outBox, workBox, folderCheck, exfatCheck, ffpkgCheck,
                                               ftpCheck, keepCheck, jobsBox, pickIconButton, automaticIconButton })
                c.Enabled = !running;
            ps5AddressBox.Enabled = !running && ftpCheck.Checked;
        }

        static bool IsFtpOutput(string path) =>
            Uri.TryCreate(path, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeFtp;

        void OpenOutput()
        {
            var target = outputs.FirstOrDefault(path => !IsFtpOutput(path));
            if (target == null)
                return;
            try
            {
                if (File.Exists(target))
                    Process.Start("explorer.exe", "/select,\"" + target + "\"");
                else
                    Process.Start("explorer.exe", "\"" + target + "\"");
            }
            catch { }
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (proc == null)
                return;
            if (MessageBox.Show(this, "A build is running. Stop it and close? Starting again later continues where it stopped.",
                                Text, MessageBoxButtons.YesNo) != DialogResult.Yes)
            {
                e.Cancel = true;
                return;
            }
            Stop();
            try { proc?.WaitForExit(15000); } catch { }
        }

        void Log(string line)
        {
            if (logBox.TextLength > 2_000_000)
                logBox.Text = logBox.Text.Substring(logBox.TextLength - 1_000_000);
            logBox.AppendText(line + Environment.NewLine);
        }

        string Elapsed()
        {
            var t = DateTime.Now - started;
            return $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}";
        }

        // --- settings -----------------------------------------------------------------------------
        void LoadSettings()
        {
            try
            {
                if (!File.Exists(settingsPath))
                    return;
                foreach (var line in File.ReadAllLines(settingsPath))
                {
                    var eq = line.IndexOf('=');
                    if (eq < 0) continue;
                    var key = line.Substring(0, eq);
                    var value = line.Substring(eq + 1);
                    switch (key)
                    {
                        case "dump": dumpBox.Text = value; break;
                        case "out": outBox.Text = value; break;
                        case "work": workBox.Text = value; break;
                        case "folder": folderCheck.Checked = value == "1"; break;
                        case "exfat": exfatCheck.Checked = value == "1"; break;
                        case "ffpkg": ffpkgCheck.Checked = value == "1"; break;
                        case "ps5Address": ps5AddressBox.Text = value; break;
                        case "keep": keepCheck.Checked = value == "1"; break;
                        case "jobs": if (int.TryParse(value, out var j)) jobsBox.Value = Math.Max(1, Math.Min(256, j)); break;
                        case "icon": if (File.Exists(value)) SetIcon(value); break;
                    }
                }
            }
            catch { }
        }

        void SaveSettings()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(settingsPath));
                File.WriteAllLines(settingsPath, new[]
                {
                    "dump=" + dumpBox.Text.Trim(), "out=" + outBox.Text.Trim(), "work=" + workBox.Text.Trim(),
                    "folder=" + (folderCheck.Checked ? 1 : 0), "exfat=" + (exfatCheck.Checked ? 1 : 0),
                    "ffpkg=" + (ffpkgCheck.Checked ? 1 : 0), "keep=" + (keepCheck.Checked ? 1 : 0),
                    "jobs=" + (int)jobsBox.Value, "icon=" + (iconPath ?? ""),
                    "ps5Address=" + ps5AddressBox.Text.Trim(),
                });
            }
            catch { }
        }
    }

    internal static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
