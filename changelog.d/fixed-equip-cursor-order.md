- **Equipment swaps keep the game's packet order.** PlugY no longer sends an
  extra cursor update after native equip actions. That update could remove the
  old body item before the game's swap update and cause a client crash when
  putting on an upgrade. Equipment actions still use the transaction guards.
