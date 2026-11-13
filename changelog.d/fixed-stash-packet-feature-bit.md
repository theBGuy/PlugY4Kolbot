- **Bots see the stash packet feature with transactions on.** The `PACKET` bit
  now accepts our transaction wrapper only when its saved handler is the real
  stash packet handler. A slot owned by another handler is not accepted.
