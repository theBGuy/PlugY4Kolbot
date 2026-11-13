- **Shared stash transactions (1.14d, single player).** Games on one PC now use
  one SQLite stash. Mouse and bot actions have atomic item and cell ownership.
  Cube contents, sockets and gold use the same rules. Other games get each
  change without a new join. The old `.sss` is imported once and kept as a
  backup. Old builds cannot open the migrated stash. A failed commit keeps its
  lock and can be tried again; the game action runs once. Saving keeps private
  cube items out of the shared view. Character files and the database still
  have separate crash recovery.
