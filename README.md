# PlugY4Kolbot
theBGuy's fork of "PlugY, The Survival Kit" by Yohann Nicolas, the plug-in for Diablo II Lord of Destruction, hardened for running many kolbot/D2Bot games at once. This fork is developed and tested on Diablo II 1.14d; upstream PlugY also supports 1.09b - 1.13d, which this fork doesn't test.

> The upstream repository is [ChaosMarc/PlugY](https://github.com/ChaosMarc/PlugY). Ready-to-use builds of this fork, with checksums and build attestations, are on the [Releases page](https://github.com/theBGuy/PlugY4Kolbot/releases). File names are unchanged (`PlugY.dll`, `PlugY.exe`, `PlugY.ini` and the `PlugY\` folder), so it is a drop-in upgrade for an existing PlugY install.

Extends Diablo II - Lord of Destruction by a number of useful features

## FEATURES
Each features can be turned on/off via PlugY.ini (see "COMMENTS ON THE CONFIGURATION FILE")
* Disable access to Battle.net.
* Infinite storage space in the stash.
* Shared storage space in the stash.
* Enable the ladder only runewords for singleplayer and TCP/IP game.
* World Event and Uber Quest for singleplayer and TCP/IP game.
* Can open Cow Level Portal even when player have kill the Cow King in that difficulty.
* Prevent Nihlathak's portal from closing.
* Move Deckard Cain near the waypoint in Harrogath.
* Unassign assigned skills and stats points.
* Auto backup save files before saving.
* Crash-safe saves (1.14d, single player, on by default): a save that doesn't change the shared stash lands whole, so after a crash or a kill the character and its stash load from the same complete save. `ActiveCrashSafeSaves=0` brings back the old save writer; either way, PlugY refuses to load a character another game is running.
* Change the selected language.
* Always regenerate maps in SinglePlayer like in MultiPlayer.
* Automatically execute /players X when you launch a new game.
* Added some pages for display more characters stats.
* Display item level in its popup.
* Launch any number of Diablo II games on the same computer.
* Increase the stash to 10x10 squares; on 1.14d, with the multi-page stash, you can turn it off again without losing items.
* Stash requests for bots and scripts (1.14d): show any page, see what every page holds, and move items between pages without leaving the page shown (see [docs/stash-protocol.md](docs/stash-protocol.md)).
* Pinned items (1.14d), the Horadric Cube by default, stay on their page when you swap stash pages.
* Stat bonus (1.14d, a cheat, off by default): extra magic find and gold find for every character in the games you run.
* Quest drops on every boss kill (1.14d, off by default): Andariel, Duriel, Mephisto, Diablo and Baal always drop their quest drop, even after you have finished their quest.
* Change the save path directory.
* Always display Mana and Life values above the globes.
* D2 can load all files, even those opened with Microsoft Excel (disabled by default).
* Display the current stats value (without magical bonus).
* Can launch game in windowed mode.
* Add following commands (see "COMMENTS ON THE CONFIGURATION FILE") :
	* /lockmouse : Lock mouse cursor in the window.
	* /lock : Same as /lockmouse.
	* /unlockmouse : Unlock mouse cursor in the window.
	* /unlock : Same as /unlockmouse.
	* /renamepage name : Rename current page stash (use # for page number).
	* /rp name : See /renamepage.
	* /setindex : Set current stash page as index.
	* /setmainindex : Set current stash page as main index.
	* /resetindex : Remove index flag on the current stash page.
	* /insertpage : Insert a new page after the current page.
	* /ip : See /insertpage.
	* /deletepage : Delete current page if empty.
	* /dp : See /deletepage.
	* /swappage page : Swap the content of current stash page with the content of another page.
	* /sp page : See /swappage.
	* /togglepage page : Swap the content of current stash page with the content of another page in opposing stash shared/personal.
	* /tp page : See /togglepage.
	* /dlm : Toggle always display mana and life mode.
	* /dml : See /dlm.
	* /dl : See /dlm.
	* /dm : See /dlm.
	* /page 1 : Show normal stats page (stats page must be opened).
	* /page 2 : Show extra stats page (stats page must be opened).
	* /page 3 : Show resistance stats page (stats page must be opened).
	* /page 4 : Show player breakpoints (stats page must be opened).
	* /page 5 : Show mercenaries breakpoints (stats page must be opened).
	* /page 6 : (beta) Show available runewords (stats page must be opened).
	* /save : (beta) Save game without exit.
	* /listcube : (beta) Create a "cube.txt" file in save directory containing all cube's receipts.
	* /renamechar newname : (beta) Rename your character and save it, in single player only. The new name is 2 to 15 letters or underscores.
	
## Installing
* **Installer:** choose the Diablo II folder (the one that holds `Game.exe`); any copy of Diablo II works. For D2Bot / kolbot, tick **Patch Game.exe (D2Bot / kolbot)**: `Game.exe` then starts PlugY by itself, with PlugY's files next to it. Each install gets its own entry in Apps & features. Uninstalling the install that patched `Game.exe` restores it, and leaves your other PlugY4Kolbot installs alone. The desktop and Start-menu shortcuts start the install that created them last. Installing over an existing install keeps your `PlugY.ini` settings and adds each option a new install's `PlugY.ini` has that yours lacks, such as a new feature's section, set to the value PlugY was already using (from `PlugY\PlugYDefault.ini`), so you can find and change it there; the `[LAUNCHING]` section, which only `PlugY.exe` reads, stays as it is. A pre-release's installer also turns `ActiveLogFile=0` into `1` and sets `UpdateChannel=prerelease` (see [Logs](#logs) and [Privacy](#privacy)).
* **Silent install:** `PlugY4Kolbot_v<version>_Installer.exe /S /D2="C:\Games\Diablo II" /PATCH` installs without showing its pages, for the Diablo II folder that `/D2=` names (a full path, in quotes). It exits with 2, installing nothing, when `/D2=` is not a full path or the folder holds no `Game.exe`. `/PATCH` ticks **Patch Game.exe** for D2Bot / kolbot, so everything goes beside `Game.exe`; if `Game.exe` can't be patched (for example while it runs), PlugY4Kolbot is still installed and the installer exits with 3. Without `/PATCH`, a new install puts `PlugY.exe` and `PlugY.ini` in `Mod PlugY` for Windows' registered copy of Diablo II, and beside `Game.exe` for any other copy. To update an existing install silently, also give its PlugY folder as `/D=<folder>`, last and without quotes, as the updater does: it is updated where it is, its patch included.
* **Zip:** extract `PlugY4Kolbot_v<version>.zip` into the Diablo II folder. For D2Bot / kolbot, also run `PatchD2File.exe` there (from `PlugY4Kolbot_v<version>_D2FilePatcher.zip`); it asks for administrator rights when the folder needs them, such as under Program Files. Its script options and exit codes are in `PlugY_The_Survival_Kit_-_Readme.txt`, in the main zip.

## Updating
* **In game:** when a newer release is out, the main menu shows **Update available** and the new version above the PlugY version (with `ActivePrintPlugYVersion=1`, the default). The answer from the last check shows at once, even while a fresh check runs. On Diablo II 1.14d, the first game after a newer release is found also prints one chat line, once per launch, telling you to run the updater, so you see it even when a bot leaves the main menu at once.
* **One click:** double-click `PlugY4KolbotUpdater.exe` in your PlugY folder (the one that holds `PlugY.exe`), or use the Start-menu shortcut "Check for PlugY4Kolbot updates". It updates that install to the newest release, verifies the download against `SHA256SUMS.txt` and GitHub's own SHA-256 digest first, and keeps your `PlugY.ini` settings, adding the options it lacks (see [Installing](#installing)); a pre-release's installer turns `ActiveLogFile=0` into `1` and sets `UpdateChannel=prerelease`, see [Logs](#logs) and [Privacy](#privacy).
  * An install the installer made, wherever it is: the updater runs the new installer for that install alone, without its pages. The install keeps its `Game.exe` patch and its shortcuts, and Apps & features shows its new version. The installer asks for administrator rights.
  * A zip-style install: the updater replaces the files one by one and backs up each to `PlugY4Kolbot-backup\<old version>-<timestamp>\`, and `PlugY.ini` too before it looks for options to add. It neither backs up nor changes a `PlugY.ini` that is read-only, is a link, has other hard links or is over 1 MB, and says why. It asks for administrator rights only when the folder needs them, such as under Program Files.
* **D2Bot / kolbot:** close every Diablo II game and `PlugY.exe` that use the folder first. While any of them still uses the files, the updater changes nothing and asks you to close them, then run it again.
* **Two installs in one Diablo II folder** (such as a `Mod PlugY` install and a patched one beside `Game.exe`) share its `PlugY.dll`, so give them the same `UpdateChannel` (a pre-release's installer changes only the `PlugY.ini` of the install it installs or updates). The updater never puts an older `PlugY.dll` back: while the other install's channel is ahead, it changes nothing and exits with 1, `--check` included.
* **Scripted use:** `PlugY4KolbotUpdater.exe --check` only reports, and `--yes` installs without asking. `--prerelease` also offers alpha, beta and rc builds. Exit codes: 0 updated or up to date, 1 error, 2 update available (`--check`), 3 files in use, 4 cancelled.

## Privacy
The in-game update check asks api.github.com for the newest release at most once a day per Windows user and update channel (hourly after a failed check), and every running game shares one cached answer per update channel in the `%LOCALAPPDATA%\PlugY4Kolbot` folder. Without a usable cache folder it doesn't check at all. It sends only a `PlugY4Kolbot/<version>` User-Agent, though GitHub still sees your IP address. The game never waits for the check. Two options in the `[GENERAL]` section of `PlugY.ini` control it:
* `CheckForUpdates=1` (default). Set it to 0 and the game never contacts GitHub; the updater still checks when you run it.
* `UpdateChannel=stable` (default). `prerelease` also offers alpha, beta and rc builds. A pre-release's installer sets `prerelease` at every install or update, also when the updater updates an install the installer made, so testers hear about the next pre-release; a later stable install keeps it, so set it back to `stable` to hear only about releases.

## Logs
Each launch of the game writes its own log to the `PlugY\logs` folder, named after its start time and process id; when the game folder is read-only, the logs go to `%LOCALAPPDATA%\PlugY4Kolbot\logs` instead. Logs older than 7 days are removed, and at most the newest 500 are kept. `ActiveLogFile=1` (the default for new installs) logs the whole session and `0` only PlugY's startup; an existing `PlugY.ini` keeps its value, except that a pre-release's installer turns `0` into `1`, so testers' logs cover the whole session.

## Contribute
Bug reports and pull requests are welcome in [this repository](https://github.com/theBGuy/PlugY4Kolbot).
* **Bugs and ideas:** open an issue with your game version, your `PlugY.ini` settings, the session log from `PlugY\logs` and the steps that trigger the problem.
* **Translations:** send a pull request that edits `game-files/PlugY/LocalizedStrings.ini`. The file must stay UTF-16LE with its byte-order mark; many editors save as UTF-8 by default, so check the encoding before you commit. `STR_MOVED_ITEMS_PAGE` is a stash page name: every language needs one, at most 20 characters, with no `#` (the stash shows the page number there) and no leading or trailing spaces, and `ENG` stays `Moved items`. `scripts/build.ps1 -Test`, which CI runs, checks it.
* **Changelog:** give every user-facing change a fragment in `changelog.d/` (see [changelog.d/README.md](changelog.d/README.md)), so it reaches the release notes.

## License
PlugY4Kolbot is free software under the GNU General Public License v3 (see [LICENSE](LICENSE)), provided without any warranty. It is a fork of PlugY, The Survival Kit, created by Yohann Nicolas. This project is not affiliated with or endorsed by Blizzard Entertainment.

## Repository layout
* `src/plugy/`: the plugin, `PlugY.dll`. Its `d2/` folder holds the Diablo II structures and function addresses.
* `src/launcher/`: the `PlugY.exe` launcher.
* `src/patcher/`: `PatchD2File.exe` and `RestoreD2File.exe`, which make `Game.exe` load PlugY and undo that.
* `src/updater/`: `PlugY4KolbotUpdater.exe`.
* `src/common/`: code these programs share, including the version number (`PlugYVersion.h`) and the update check.
* `game-files/`: the files installed beside the programs: `PlugY.ini`, the `PlugY\` folder and the readmes.
* `installer/`: the NSIS installer script.
* `scripts/`: the build, package and release scripts.
* `tests/`: the native tests.
* `docs/`: the stash protocol reference.
* `changelog.d/`: the changelog fragments of the next release.

## Building from source
Prerequisites (Windows):
* Visual Studio 2022 with the "Desktop development with C++" workload, including the x86 build tools.
* NSIS 3.11 or later, for the installer.
* 7-Zip, for the release zips.
* Node.js (CI uses version 24), for the release scripts and for packaging a pre-release.
* PowerShell 7 (`pwsh`) or Windows PowerShell 5.1, which run the build scripts.

Build the plugin, the launcher, the updater and the D2 file patcher, then package them:
```
pwsh -NoProfile -File scripts/build.ps1 -Config Release
pwsh -NoProfile -File scripts/package.ps1
```
Add `-Test` to the build command to also build and run the native test suites (about 30-40 seconds more); it exits non-zero when a test fails, and CI always runs it.
With only Windows PowerShell 5.1, run the same scripts as `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1` (and likewise for `package.ps1`).
The first command finds Visual Studio on its own and writes `PlugY.dll`, `PlugY.exe`, `PlugY4KolbotUpdater.exe`, `PatchD2File.exe` and `RestoreD2File.exe` to `build/out/Release/`. The second writes the release assets to `dist/`, named after the version in `src/common/PlugYVersion.h`:
* `PlugY4Kolbot_v<version>.zip` (the files for a manual or D2Bot/kolbot install, including `PlugY4KolbotUpdater.exe`)
* `PlugY4Kolbot_v<version>_D2FilePatcher.zip`
* `PlugY4Kolbot_v<version>_Installer.exe`
* `PlugY4Kolbot_v<version>_symbols.zip` (debug symbols)
* `SHA256SUMS.txt`

Every push to `master` and every pull request runs the same build in CI, and the packaged files are attached to the run as an artifact.

## Releasing (maintainers)
1. Give every user-facing change a changelog fragment in `changelog.d/` (see [changelog.d/README.md](changelog.d/README.md)). Pull requests that change code are checked for one; the `no-changelog` label or `skip-changelog` in the PR title exempts changes users never see.
2. Bring the Traditional Chinese readme (`game-files/PlugY_The_Survival_Kit_-_讀我檔案.txt`) up to date with the English one in a pull request merged to `master` before the release. Once it's translated, run `git fetch`, then `node scripts/zh-readme-mark.mjs` in that pull request, and commit the `scripts/zh-readme-translated-to.txt` it writes; the pull request's title doesn't matter. The file holds two marks, both set to the `master` commit the branch is based on: `english`, the commit whose English readme the translation covers, and `zh-tw`, the commit the Traditional Chinese readme was last checked against. Before writing, the script lists the English readme commits it marks as translated, the Traditional Chinese readme edits it marks as checked, and the branch's own edits of either readme, and asks. English readme edits made in the pull request itself count as translated when it is squash-merged, so translate them there too. Run it again after merging `origin/master` into the branch and committing the merge, since the merge may bring in more of either; if that merge conflicts on the marker file, take `origin/master`'s version. If `origin/master` already records a mark the script would write (another catch-up from the same base merged first), it says so: merge `origin/master` in the same way and run it again. For an English change that needs no translation, `--english-only` moves only the English mark; `--zh-only` records Traditional Chinese readme edits you checked without translating anything. Other pull requests may leave the Traditional Chinese readme behind: it is UTF-16, which Git merges as a binary file, so two open pull requests that both edit it always conflict. `release.mjs` lists the English readme commits after the English mark, and the Traditional Chinese readme edits after its mark, including one merged while the catch-up was open (a binary merge that kept an older copy undoes a catch-up). It asks before releasing when it lists any, or when it can't read the file or measure from a mark (`--dry-run` only prints the warnings). Reverting a catch-up reverts the file with it, so the check measures from the catch-up before it again.
3. Run `node scripts/release.mjs` from an up-to-date `master` (add `--dry-run` to rehearse a release first). It bumps the version and, for a stable release, assembles the fragments into `CHANGELOG.md` (a pre-release leaves them pending for the next stable release). Then it commits, creates the tag `vX.Y.Z` (or a pre-release tag such as `vX.Y.Z-rc.N` / `-beta.N`) and pushes `master` and the tag together.
4. The tag push runs the release workflow, which checks the tag against `src/common/PlugYVersion.h` and that the tagged commit is on `master`, builds every binary and runs the native tests, packages every asset, attests their provenance and creates a **draft** GitHub release. Its notes are the changelog section for a stable release, or the pending fragments for a pre-release: the entries new since the previous pre-release first, when there is one, and every pending entry below them. A pre-release's zip and installer also ship a `CHANGELOG.md` with a section for it, built from those fragments; the repository's `CHANGELOG.md` is left as it is.
5. Review the draft and smoke-test its downloads.
6. Publish it: `gh release edit vX.Y.Z --draft=false --latest` for a stable release, or `gh release edit vX.Y.Z-rc.N --draft=false` for a pre-release.
