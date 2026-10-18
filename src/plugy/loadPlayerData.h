/*=================================================================
	File created by Yohann NICOLAS.

	Load Player Custom Data.

=================================================================*/
#pragma once

#include "common.h"

// The character this client last joined a multiplayer game with ("" before the first join).
extern char joinedCharName[17];

void Install_LoadPlayerData();

// Frees what a host received from clientID's save upload, once its connection is gone (a later client may reuse
// the id, and must not append to the stale entry). Server thread only, like ReceiveSaveFiles.
void discardClientSaveFiles(DWORD clientID);

// The 1.14d single-player load sites, installed by Install_CharacterCommit: the .d2s open, its read (the game's fread
// itself, cdecl), and the built player.
void caller_OpenSPCharacter_114();
size_t __cdecl ReadSPCharacter_114(void* buffer, size_t size, size_t count, void* file);
void caller_LoadSPCharacter_114();

/*================================= END OF FILE =================================*/
