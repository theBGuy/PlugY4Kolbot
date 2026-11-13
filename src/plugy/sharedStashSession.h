#pragma once

#include "common.h"

// All entry points run on the owning server game thread. Native handlers must
// call Before/After as a pair, even when the native handler reports failure.
bool Install_SharedStashSession();
bool SharedStashLoad(Unit* ptChar, bool isHardcore);
bool SharedStashActive(const Unit* ptChar);
void SharedStashClientReady(Unit* ptChar);
bool SharedStashBeforeAction(Game* ptGame, Unit* ptChar, const BYTE* packet, DWORD size);
void SharedStashAfterAction(Unit* ptChar);
void SharedStashTick(Game* ptGame);
bool SharedStashCanSave(Unit* ptChar);
bool SharedStashBeginSave(Unit* ptChar);
void SharedStashEndSave(Unit* ptChar);
void SharedStashBeginClose(Unit* ptChar);
void SharedStashClose(PlayerData* owner);
void SharedStashAfterClose(PlayerData* owner);
Unit* SharedStashNextItemToFree(Unit* ptChar);
