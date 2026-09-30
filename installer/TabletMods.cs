// Other tablet mods that change the same tablet data as Echo Arcade, found from their install records,
// and their removal. Echo Arcade needs the stock tablet layout to add its tab.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Web.Script.Serialization;

namespace ArcadeReelsSetup
{
	static class TabletMods
	{
		// ---- EchoTabletTrainer ----
		// Its bin\win10\EchoTabletTrainer.install.json lists every file it wrote: whether the file existed before,
		// SHA-256 before and after, and a backup of the original (bin\win10\EchoTabletTrainer-backup-<installed>\<n>.before).

		public static string TrainerRecord(string game)
		{
			return Path.Combine(game, "bin", "win10", "EchoTabletTrainer.install.json");
		}

		public static bool TrainerInstalled(string game)
		{
			return File.Exists(TrainerRecord(game));
		}

		class RecordedFile
		{
			public string Relative, Path, Backup, Before, After;
			public bool Existed;
		}

		static string Sha(string path)
		{
			using (var sha = SHA256.Create())
			using (var stream = File.OpenRead(path))
				return string.Concat(sha.ComputeHash(stream).Select(b => b.ToString("x2")));
		}

		// Puts back every file the trainer changed and removes the ones it added. Everything removed or replaced is
		// first copied to <game>\EchoTabletTrainer-removed-<time>. 'confirm' is asked before undoing later changes.
		public static bool RemoveTrainer(string game, Action<string> log, Func<string, bool> confirm)
		{
			string record = TrainerRecord(game);
			var root = (Dictionary<string, object>)new JavaScriptSerializer().DeserializeObject(File.ReadAllText(record));
			string installed = root.ContainsKey("installed") ? root["installed"] as string : null;
			var files = new List<RecordedFile>();
			foreach (Dictionary<string, object> entry in (object[])root["files"])
			{
				var file = new RecordedFile
				{
					Relative = (string)entry["relative"],
					Existed = entry["existed"] is bool && (bool)entry["existed"],
					Before = entry["before"] as string,
					After = entry["after"] as string,
					Backup = entry["backup"] as string,
				};
				if (file.Relative.Contains("..") || System.IO.Path.IsPathRooted(file.Relative))
				{
					log("EchoTabletTrainer: unexpected path in its install record (" + file.Relative + "); not touching anything.");
					return false;
				}
				file.Path = System.IO.Path.Combine(game, file.Relative);
				// The record keeps absolute backup paths; the game folder may have moved since
				if (file.Backup == null || !File.Exists(file.Backup))
					file.Backup = installed == null || file.Backup == null ? null : System.IO.Path.Combine(game, "bin", "win10",
						"EchoTabletTrainer-backup-" + installed, System.IO.Path.GetFileName(file.Backup));
				files.Add(file);
			}

			foreach (RecordedFile file in files.Where(f => f.Existed))
				if (file.Backup == null || !File.Exists(file.Backup) || (file.Before != null && Sha(file.Backup) != file.Before))
				{
					log("EchoTabletTrainer: the backup of " + file.Relative + " is missing or damaged, so it cannot be uninstalled automatically.");
					log("         Use the trainer's own uninstaller (or restore the game files), then click Install / Repair again.");
					return false;
				}

			var changed = files.Where(f => (File.Exists(f.Path) ? Sha(f.Path) : null) != f.After).Select(f => f.Relative).ToList();
			if (changed.Count > 0 && !confirm("These files changed after EchoTabletTrainer was installed (another mod or tool):\n\n  " +
				string.Join("\n  ", changed) + "\n\nUninstalling puts back the versions from before the trainer, which also undoes those later changes " +
				"(a copy of everything is kept). Continue?"))
			{
				log("EchoTabletTrainer: uninstall cancelled.");
				return false;
			}

			string safe = System.IO.Path.Combine(game, "EchoTabletTrainer-removed-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
			Directory.CreateDirectory(safe);
			foreach (RecordedFile file in files)
				if (File.Exists(file.Path))
					File.Copy(file.Path, System.IO.Path.Combine(safe, file.Relative.Replace("\\", "__").Replace("/", "__")), true);
			File.Copy(record, System.IO.Path.Combine(safe, "EchoTabletTrainer.install.json"), true);

			foreach (RecordedFile file in files)
			{
				if (file.Existed)
					File.Copy(file.Backup, file.Path, true);
				else if (File.Exists(file.Path))
				{
					File.Delete(file.Path);
					string dir = System.IO.Path.GetDirectoryName(file.Path);
					if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any())
						Directory.Delete(dir); // e.g. its music folder
				}
			}
			File.Delete(record);

			bool verified = files.Where(f => f.Existed && f.Before != null).All(f => Sha(f.Path) == f.Before);
			log("Uninstalled EchoTabletTrainer: " + files.Count(f => f.Existed) + " file(s) restored, " + files.Count(f => !f.Existed) + " removed" +
				(verified ? " (checked against its record)." : " (WARNING: some restored files do not match its record)."));
			log("A copy of everything removed or replaced is in " + safe);
			return verified;
		}
	}
}
