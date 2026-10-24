- **Crash-safe character saves.** On Diablo II 1.14d, a single-player save that
  doesn't touch the shared stash now lands whole: whenever the game crashes or a
  bot manager kills it, the next load finds the complete new save or the
  complete previous one, with the character and its PlugY stash (`.d2x`)
  matching. After a kill late in a save, the next load finishes that save and
  goes straight into the game with it. Two single-player games can't run the
  same character at once; as soon as one game is done with it, another can load
  it. On by default for new installs and upgrades; setting
  `ActiveCrashSafeSaves=0` in the `[SAVEPATH]` section of `PlugY.ini` brings
  back the old save writer and still keeps each character to one game at a time.
