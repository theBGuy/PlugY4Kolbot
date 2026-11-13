/*=================================================================
	File created by Yohann NICOLAS.

	Add an extra save file shared by all own characters.

=================================================================*/
#pragma once

#include "common.h"

// Once a database exists, legacy readers and writers must never fall back to .sss.
bool getSharedStashDatabasePath(char* path, bool isHardcore);
bool legacySharedStashAvailable(bool isHardcore);

// filePath (MAX_PATH chars, optional) receives the shared file's path, or "" when it couldn't be built.
BYTE* readSharedSaveFile(char* name, DWORD* size, bool* fileExisted = NULL, char* filePath = NULL);
void writeSharedSaveFile(char* name, BYTE* data, DWORD size, bool isHardcore);

// Migration checks consumedSize against the source size so opaque extension trailers cannot be dropped.
int loadSharedSaveFile(Unit* ptChar, BYTE* data, DWORD maxSize, DWORD* consumedSize = NULL);
void saveSharedSaveFile(Unit* ptChar, BYTE** data, DWORD* maxSize, DWORD* curSize);

void initSharedStashBaseline(Unit* ptChar, DWORD diskHash, bool diskExisted);
// isLeaving: the save made when the player leaves the game, the only one that writes a conflict file or shows a box.
// Returns true when the file holds this game's shared items.
bool writeSharedSaveFileIfChanged(Unit* ptChar, BYTE* data, DWORD size, bool isHardcore, bool isLeaving);
// Whether this save leaves the shared stash file as it is, by the same decision writeSharedSaveFileIfChanged makes;
// "changed" whenever an input is missing. Reads only and takes no lock.
bool sharedSaveIsUnchanged(Unit* ptChar, bool isHardcore);
#ifdef PLUGY_TEST_HOOKS
void checkSharedSaveParity(Unit* ptChar, bool isHardcore);
#endif

void backupSharedSaveFile();

/*================================= END OF FILE =================================*/
