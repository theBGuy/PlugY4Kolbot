- **Stash close keeps object clicks working.** Closing the stash no longer needs
  a shared database transaction. A busy or stale shared stash cannot block the
  close and leave the server's object interaction open. Gold and cube actions
  still use the transaction checks.
