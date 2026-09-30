// Echo Arcade Reels setup: installs the Echo Arcade tablet tab with only the REELS tile (Instagram Reels),
// using the bundled Python and the project's own tools/install.py, and makes sure the plugin gets loaded.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace ArcadeReelsSetup
{
	static class Program
	{
		[STAThread]
		static void Main(string[] args)
		{
			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			Application.Run(new MainForm(args.Length > 0 ? args[0] : null));
		}
	}

	class MainForm : Form
	{
		const string Payload = "payload.zip";                   // embedded: tools, plugin, host, loader, Python
		const uint ExeTimestamp = 1683152886;                  // the supported echovr.exe build
		const string LoaderMarker = "Echo Arcade plugin loader";
		const string DiscGlowMarker = "DiscGlow active:";      // DiscGlow also loads echoloader.json plugins
		const string PackagePrefix = "48037dc70b0ecab2_";

		static readonly string AppDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "EchoArcadeReels");
		static string StatePath { get { return Path.Combine(AppDir, "app", "install_state.json"); } }
		static string SettingsPath { get { return Path.Combine(AppDir, "folder.txt"); } }

		string _game;   // ready-at-dawn-echo-arena folder
		bool _busy;
		readonly Label lblFolder = new Label();
		readonly StatusLine status = new StatusLine();
		readonly FlatButton btnInstall = new FlatButton(), btnUninstall = new FlatButton(), btnLaunch = new FlatButton();
		readonly TextBox log = new TextBox();

		string Bin { get { return Path.Combine(_game, "bin", "win10"); } }
		string Packages { get { return Path.Combine(_game, "_data", "5932408047", "rad15", "win10", "packages"); } }

		[DllImport("dwmapi.dll")]
		static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
		protected override void OnHandleCreated(EventArgs e)
		{
			base.OnHandleCreated(e);
			int dark = 1;
			DwmSetWindowAttribute(Handle, 20, ref dark, 4);
			int caption = Theme.Back.R | (Theme.Back.G << 8) | (Theme.Back.B << 16);
			DwmSetWindowAttribute(Handle, 35, ref caption, 4);
		}

		public MainForm(string folderArgument)
		{
			Text = "Echo Arcade Reels";
			try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
			FormBorderStyle = FormBorderStyle.FixedSingle;
			MaximizeBox = false;
			StartPosition = FormStartPosition.CenterScreen;
			ClientSize = new Size(720, 560);
			BackColor = Theme.Back;
			ForeColor = Theme.Text;
			Font = Theme.Body;

			var title = new Label { Text = "Echo Arcade Reels", Font = Theme.Title, ForeColor = Theme.Text, AutoSize = true, Left = 22, Top = 16 };
			var sub = new Label { Text = "Instagram Reels on the Echo VR hand tablet (PC)", ForeColor = Theme.Muted, AutoSize = true, Left = 25, Top = 52 };

			var card = new Card { Title = "Install", Left = 20, Top = 84, Width = 680, Height = 186 };
			lblFolder.SetBounds(18, 50, 540, 20);
			lblFolder.ForeColor = Theme.Muted; lblFolder.BackColor = Theme.Card; lblFolder.AutoEllipsis = true;
			var browse = new FlatButton { Text = "Browse" };
			browse.SetBounds(572, 44, 90, 32);
			browse.Click += delegate { Browse(); };
			status.SetBounds(18, 84, 644, 44);
			btnInstall.Primary = true; btnInstall.Text = "Install / Repair"; btnInstall.SetBounds(18, 136, 150, 34); btnInstall.Click += delegate { Run(Install); };
			btnUninstall.Text = "Uninstall"; btnUninstall.SetBounds(178, 136, 110, 34); btnUninstall.Click += delegate { Run(Uninstall); };
			btnLaunch.Text = "Launch Echo"; btnLaunch.SetBounds(298, 136, 130, 34); btnLaunch.Click += delegate { Launch(); };
			card.Controls.AddRange(new Control[] { lblFolder, browse, status, btnInstall, btnUninstall, btnLaunch });

			var logCard = new Card { Title = "Details", Left = 20, Top = 286, Width = 680, Height = 254 };
			log.Multiline = true; log.ReadOnly = true; log.ScrollBars = ScrollBars.Vertical; log.BorderStyle = BorderStyle.None;
			log.BackColor = Theme.Card; log.ForeColor = Theme.Muted; log.Font = new Font("Consolas", 8.5f);
			log.SetBounds(18, 44, 644, 194);
			logCard.Controls.Add(log);
			Controls.AddRange(new Control[] { title, sub, card, logCard });

			Activated += delegate { if (!_busy) RefreshStatus(); };
			SetGame(folderArgument ?? LoadSetting() ?? DetectGame());
			Write("After installing: start Echo VR, open the hand tablet and press the gamepad tab (far left), then REELS.");
			Write("The first time, log into Instagram in the Chrome window that opens on your desktop.");
		}

		// ---- Finding Echo ----

		static string Normalize(string path)
		{
			if (string.IsNullOrEmpty(path)) return null;
			foreach (string c in new[] { path, Path.Combine(path, "Software", "ready-at-dawn-echo-arena") })
				if (File.Exists(Path.Combine(c, "bin", "win10", "echovr.exe"))) return c;
			string up = Path.GetDirectoryName(Path.GetDirectoryName(path.TrimEnd('\\')) ?? "");
			return up != null && File.Exists(Path.Combine(up, "bin", "win10", "echovr.exe")) ? up : null;  // picked bin\win10
		}

		static string DetectGame()
		{
			var libraries = new List<string>();
			try
			{
				using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Oculus VR, LLC\Oculus\Libraries"))
					if (key != null)
						foreach (string name in key.GetSubKeyNames())
							using (var lib = key.OpenSubKey(name))
							{
								object p = lib != null ? lib.GetValue("OriginalPath") : null;
								if (p != null) libraries.Add(p.ToString());
							}
			}
			catch { }
			libraries.Add(@"C:\Program Files\Oculus\Software");
			foreach (string lib in libraries)
			{
				string found = Normalize(Path.Combine(lib, "Software", "ready-at-dawn-echo-arena"));
				if (found != null) return found;
			}
			return null;
		}

		static string LoadSetting()
		{
			try { return File.Exists(SettingsPath) ? Normalize(File.ReadAllText(SettingsPath).Trim()) : null; } catch { return null; }
		}

		void Browse()
		{
			using (var dlg = new FolderBrowserDialog { Description = "Select the Echo VR folder (ready-at-dawn-echo-arena)" })
			{
				if (dlg.ShowDialog(this) != DialogResult.OK) return;
				if (Normalize(dlg.SelectedPath) == null) { Msg("echovr.exe was not found in that folder."); return; }
				SetGame(dlg.SelectedPath);
			}
		}

		void SetGame(string path)
		{
			_game = Normalize(path);
			lblFolder.Text = _game ?? "Echo VR was not found. Click Browse to pick its folder.";
			if (_game != null)
				try { Directory.CreateDirectory(AppDir); File.WriteAllText(SettingsPath, _game); } catch { }
			RefreshStatus();
		}

		// ---- Checks ----

		bool GameRunning()
		{
			return Process.GetProcessesByName("echovr").Length > 0;
		}

		static bool Contains(string path, string marker)
		{
			try
			{
				byte[] data = File.ReadAllBytes(path), m = Encoding.ASCII.GetBytes(marker);
				for (int i = 0; i + m.Length <= data.Length; i++)
				{
					int k = 0;
					while (k < m.Length && data[i + k] == m[k]) k++;
					if (k == m.Length) return true;
				}
			}
			catch { }
			return false;
		}

		static uint PeTimestamp(string exe)
		{
			try
			{
				byte[] head = new byte[4096];
				using (var f = File.OpenRead(exe)) f.Read(head, 0, head.Length);
				return BitConverter.ToUInt32(head, BitConverter.ToInt32(head, 0x3c) + 8);
			}
			catch { return 0; }
		}

		// EchoLoader reads echoloader.json itself; anything else in dbgcore.dll (e.g. EchoRelay's patch) does not.
		bool EchoLoaderPresent() { return Contains(Path.Combine(Bin, "dbgcore.dll"), "echoloader.json"); }

		bool Installed()
		{
			if (!File.Exists(StatePath)) return false;
			try
			{
				string pkg = System.Text.RegularExpressions.Regex.Match(File.ReadAllText(StatePath), "\"package\"\\s*:\\s*\"([^\"]+)\"").Groups[1].Value;
				return File.Exists(Path.Combine(Bin, "plugins", "EchoArcade.dll")) && pkg.Length > 0 && File.Exists(Path.Combine(Packages, pkg));
			}
			catch { return false; }
		}

		void RefreshStatus()
		{
			bool valid = _game != null;
			btnInstall.Enabled = btnLaunch.Enabled = valid && !_busy;
			btnUninstall.Enabled = valid && !_busy && File.Exists(StatePath);
			if (!valid) { status.Set("Pick your Echo VR folder to continue.", Theme.Muted); return; }
			var parts = new List<string>();
			Color dot = Theme.Muted;
			if (PeTimestamp(Path.Combine(Bin, "echovr.exe")) != ExeTimestamp) { parts.Add("This echovr.exe is not the build Echo Arcade supports; it will stay inactive."); dot = Theme.Warn; }
			if (Installed()) { parts.Add("Installed."); if (dot != Theme.Warn) dot = Theme.Good; }
			else if (File.Exists(StatePath)) { parts.Add("Installed, but its tablet data is missing (another mod tool rewrote the game data). Click Install / Repair."); dot = Theme.Warn; }
			else parts.Add("Not installed.");
			if (GameRunning()) parts.Add("Echo is running.");
			status.Set(string.Join(" ", parts), dot);
		}

		// ---- Actions ----

		void Run(Action action)
		{
			if (GameRunning()) { Msg("Close Echo VR first."); return; }
			_busy = true;
			RefreshStatus();
			Task.Run(() =>
			{
				try { action(); }
				catch (Exception ex) { Write("ERROR: " + ex.Message); }
				finally { BeginInvoke((Action)delegate { _busy = false; RefreshStatus(); }); }
			});
		}

		void Write(string line)
		{
			if (InvokeRequired) { BeginInvoke((Action<string>)Write, line); return; }
			log.AppendText(line + Environment.NewLine);
		}

		// Unpacks the embedded payload into %LOCALAPPDATA%\EchoArcadeReels\app (kept: it holds the install state and backups)
		void Unpack()
		{
			string app = Path.Combine(AppDir, "app");
			using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream(Payload))
			using (var zip = new ZipArchive(s))
				foreach (var entry in zip.Entries)
				{
					if (entry.FullName.EndsWith("/")) continue;
					string target = Path.GetFullPath(Path.Combine(app, entry.FullName));
					if (!target.StartsWith(Path.GetFullPath(app), StringComparison.OrdinalIgnoreCase)) continue;
					Directory.CreateDirectory(Path.GetDirectoryName(target));
					entry.ExtractToFile(target, true);
				}
			Write("Unpacked the installer files to " + app);
		}

		int Python(string args)
		{
			string app = Path.Combine(AppDir, "app");
			var psi = new ProcessStartInfo(Path.Combine(app, "python", "python.exe"), "-u tools\\install.py " + args + " --game \"" + _game + "\"")
			{
				WorkingDirectory = app, UseShellExecute = false, CreateNoWindow = true,
				RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
			};
			psi.EnvironmentVariables["ECHOVR_GAME"] = _game;
			psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
			using (var p = Process.Start(psi))
			{
				p.OutputDataReceived += (o, e) => { if (e.Data != null) Write(e.Data); };
				p.ErrorDataReceived += (o, e) => { if (e.Data != null) Write(e.Data); };
				p.BeginOutputReadLine();
				p.BeginErrorReadLine();
				p.WaitForExit();
				return p.ExitCode;
			}
		}

		void Install()
		{
			Unpack();
			// A stale install (its package was deleted by another mod tool): start over on the current game data
			if (File.Exists(StatePath) && !Installed())
			{
				File.Move(StatePath, StatePath + ".stale-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
				Write("Previous install's tablet data is gone; installing fresh on the current game data.");
			}
			string loaderJson = Path.Combine(Bin, "echoloader.json");
			if (!File.Exists(loaderJson)) File.WriteAllText(loaderJson, "{\n    \"plugins\": []\n}\n");

			if (!File.Exists(StatePath))
			{
				Write("Building the tablet tab from your game data...");
				if (Python("install") != 0) { Write("Install failed (see above)."); return; }
			}
			else
			{
				Write("Updating the plugin and host...");
				if (Python("update") != 0) { Write("Update failed (see above)."); return; }
			}
			ConfigureReelsOnly();
			InstallLoader();
			Write("Done. Start Echo VR, open the hand tablet and press the gamepad tab, then REELS.");
		}

		// Only the REELS tile, sound on the Windows default output
		void ConfigureReelsOnly()
		{
			string ini = Path.Combine(Bin, "plugins", "EchoArcade", "arcade.ini");
			if (!File.Exists(ini)) return;
			var lines = File.ReadAllLines(ini).ToList();
			Action<string, string> set = (key, value) =>
			{
				int host = lines.FindIndex(l => l.Trim().Equals("[host]", StringComparison.OrdinalIgnoreCase));
				if (host < 0) { lines.Add("[host]"); host = lines.Count - 1; }
				int at = lines.FindIndex(host + 1, l => l.TrimStart().StartsWith(key + "=", StringComparison.OrdinalIgnoreCase));
				if (at >= 0) lines[at] = key + "=" + value; else lines.Insert(host + 1, key + "=" + value);
			};
			set("tiles", "reels");
			set("audio_device", "default");
			File.WriteAllLines(ini, lines);
			Write("Configured: only the REELS tile, sound on the Windows default output.");
		}

		// Makes sure something loads plugins\EchoArcade.dll: EchoLoader, DiscGlow, or our small loader as dinput8.dll
		void InstallLoader()
		{
			string dinput = Path.Combine(Bin, "dinput8.dll"), chain = Path.Combine(Bin, "dinput8.chain.dll");
			if (EchoLoaderPresent()) { Write("EchoLoader is installed; it loads the plugin."); return; }
			if (File.Exists(dinput) && Contains(dinput, DiscGlowMarker)) { Write("DiscGlow is installed; it loads the plugin."); return; }
			string ours = Path.Combine(AppDir, "app", "loader", "dinput8.dll");
			if (File.Exists(dinput) && !Contains(dinput, LoaderMarker))
			{
				if (File.Exists(chain)) { Write("WARNING: dinput8.dll and dinput8.chain.dll both belong to other mods; the plugin will not load."); return; }
				File.Move(dinput, chain);
				Write("Kept your other dinput8 mod as dinput8.chain.dll; the loader loads it too.");
			}
			File.Copy(ours, dinput, true);
			Write("Installed the plugin loader (dinput8.dll).");
		}

		void Uninstall()
		{
			Unpack();
			int code = Python("restore");
			if (code != 0 && (DialogResult)Invoke((Func<DialogResult>)(() => MessageBox.Show(this,
				"The game data changed after Echo Arcade was installed (another mod or a game update). Restore the backup anyway? That also removes changes made since.",
				"Echo Arcade Reels", MessageBoxButtons.YesNo, MessageBoxIcon.Warning))) == DialogResult.Yes)
				code = Python("restore --force");
			string dinput = Path.Combine(Bin, "dinput8.dll"), chain = Path.Combine(Bin, "dinput8.chain.dll");
			if (File.Exists(dinput) && Contains(dinput, LoaderMarker))
			{
				File.Delete(dinput);
				if (File.Exists(chain)) File.Move(chain, dinput);
				Write("Removed the plugin loader.");
			}
			Write(code == 0 ? "Uninstalled." : "Uninstall incomplete (see above).");
		}

		void Launch()
		{
			if (GameRunning()) { Msg("Echo VR is already running."); return; }
			try { Process.Start(new ProcessStartInfo(Path.Combine(Bin, "echovr.exe")) { WorkingDirectory = Bin, UseShellExecute = true }); }
			catch (Exception ex) { Msg("Could not start Echo VR: " + ex.Message); }
		}

		void Msg(string text) { MessageBox.Show(this, text, "Echo Arcade Reels"); }
	}
}
