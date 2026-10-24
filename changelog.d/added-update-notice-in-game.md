- **Update notice in the game.** When a newer release is out, the main menu
  shows **Update available** and the new version above the PlugY version (with
  `ActivePrintPlugYVersion=1`, the default). On Diablo II 1.14d, the first game
  after a newer release is found also prints one chat line, once per launch,
  telling you to run `PlugY4KolbotUpdater.exe`, so you see it even when a bot
  leaves the main menu at once. Two new options in the `[GENERAL]` section of
  `PlugY.ini` control it: `CheckForUpdates` (1 by default; set 0 and the game
  never contacts GitHub) and `UpdateChannel` (`stable` by default; `prerelease`
  also offers alpha, beta and rc builds). The check never delays the game and
  asks api.github.com at most once a day per Windows user and update channel,
  sending only a `PlugY4Kolbot/<version>` User-Agent.
