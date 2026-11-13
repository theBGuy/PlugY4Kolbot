- **Closed games release shared stash locks after a write failure.** If the
  database is busy when a game ends, PlugY keeps the session cleanup record and
  tries again from the menu. Other games can use the stash once the database
  is ready; the owning Diablo process does not have to exit first.
