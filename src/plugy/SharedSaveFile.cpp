/*=================================================================
	File created by Yohann NICOLAS.

	Add an extra save file shared by all own characters.

=================================================================*/

#include "sharedSaveFile.h"
#include "infinityStash.h"
#include "customLibraries.h"
#include "common.h"
#include "sharedStashLegacyRules.h"
#include <stdio.h>

#define FILE_SHAREDSTASH 0x00535353	//"SSS "
#define BUFFER_SIZE 0x4000
#define FILE_VERSION 0x3230			//"02"
#define FILE_MIN_SIZE 10			// smallest valid file : tag, version "01", number of stash


static bool getSharedSaveFilePath(char* path, bool isHardcore)
{
	return getSaveFilePath(path, MAX_PATH, separateHardSoftStash && isHardcore ? "_LOD_HC_" : "_LOD_", sharedStashFilename, ".sss");
}

bool getSharedStashDatabasePath(char* path, bool isHardcore)
{
	return getSaveFilePath(path, MAX_PATH, separateHardSoftStash && isHardcore ? "_LOD_HC_" : "_LOD_", sharedStashFilename, ".sqlite3");
}

bool legacySharedStashAvailable(bool isHardcore)
{
	char path[MAX_PATH];
	if (!getSharedStashDatabasePath(path, isHardcore))
		return false;
	return sharedStashLegacyPathAvailable(path);
}


// The mutex is named after the file so every process sharing that file serializes on it.
// A holder can keep the lock about 15 s (read retries, replace retries, then its conflict file), so the default waits
// longer; a save that can give up and retry later passes a short wait instead of stalling the game.
static HANDLE lockSharedSaveFile(const char* path, DWORD waitMs = 20000)
{
	char fullPath[MAX_PATH];
	DWORD len = GetFullPathNameA(path, MAX_PATH, fullPath, NULL);
	if (!len || len >= MAX_PATH)
	{
		strncpy(fullPath, path, MAX_PATH-1);
		fullPath[MAX_PATH-1] = NULL;
	}
	// 8.3 and long spellings of the same save folder must name the same mutex
	char longPath[MAX_PATH];
	len = GetLongPathNameA(fullPath, longPath, MAX_PATH);
	if (len && len < MAX_PATH)
		strcpy(fullPath, longPath);
	_strlwr(fullPath);

	char mutexName[64];
	sprintf(mutexName, "Local\\PlugY_SharedStash_%08X", fnv1a((BYTE*)fullPath, strlen(fullPath)));
	HANDLE hMutex = CreateMutexA(NULL, FALSE, mutexName);
	if (!hMutex)
	{
		log_msg("Can't create mutex %s (error %u), continue without lock\n", mutexName, GetLastError());
		return NULL;
	}
	DWORD wait = WaitForSingleObject(hMutex, waitMs);
	if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
		return hMutex;
	log_msg("Can't lock %s for %s (wait result %u, error %u), continue without lock\n", mutexName, fullPath, wait, GetLastError());
	CloseHandle(hMutex);
	return NULL;
}


static void unlockSharedSaveFile(HANDLE hMutex)
{
	if (!hMutex) return;
	ReleaseMutex(hMutex);
	CloseHandle(hMutex);
}


// Returns NULL when the shared file exists but can't be read, so it is never replaced by an empty stash.
BYTE* readSharedSaveFile(char* name, DWORD* size, bool* fileExisted, char* filePath)
{
	char filename[MAX_PATH];
	BYTE* data=NULL;
	BYTE isHardCore=0;
	int found=0;

	*size = 0;
	if (fileExisted)
		*fileExisted = false;
	if (filePath)
		filePath[0] = NULL;

	if (separateHardSoftStash)//Get hardcore flag
	{
		if (getSaveFilePath(filename, MAX_PATH, name, ".d2s"))
		{
			log_msg("Normal file to read if it's hardcore character : %s\n",filename);
			FILE* file = fopen(filename, "rb");
			if (file)
			{
				fseek(file, 0x24, SEEK_SET);
				DWORD nbRead = fread(&isHardCore, 1, 1, file);
				isHardCore = (nbRead==1)? ((isHardCore & 4) == 4) : 0;
				fclose(file);
			}
		} else if (active_sharedStash)
			return NULL;
		log_msg("%s is a HardCore character = %d\n",name,isHardCore);
	}

	if (active_sharedStash)
	{
		if (!legacySharedStashAvailable(isHardCore != 0))
		{
			if (filePath)
				getSharedStashDatabasePath(filePath, isHardCore != 0);
			log_msg("Shared stash load refused: a SQLite stash exists or its path cannot be checked; legacy .sss fallback is disabled.\n");
			return NULL;
		}
		if (!getSharedSaveFilePath(filename, isHardCore != 0))
			return NULL;
		if (filePath)
			strcpy(filePath, filename);

		log_msg("Shared file to read : %s\n",filename);

		HANDLE hLock = lockSharedSaveFile(filename);
		if (!legacySharedStashAvailable(isHardCore != 0))
		{
			unlockSharedSaveFile(hLock);
			return NULL;
		}
		found = readSaveFileData(filename, &data, size, FILE_MIN_SIZE);
		unlockSharedSaveFile(hLock);
		if (found < 0)
			return NULL;
	}

	if (found)
	{
		if (fileExisted)
			*fileExisted = true;
	} else {
		log_msg("Can't open shared save file in mode \"rb\" (is not an error if it's the first start of the mod)\n");
		*size = 14;
		DWORD maxSize = 100;
		data = (BYTE*)D2FogMemAlloc(maxSize,__FILE__,__LINE__,0);
		if (!data)
		{
			log_msg("readSharedSaveFile : memory allocation failed\n");
			*size = 0;
			return NULL;
		}
		*((DWORD *)&data[0]) = FILE_SHAREDSTASH;
		*((WORD *)&data[4]) = FILE_VERSION;
		*((DWORD *)&data[6]) = 0;
		*((DWORD *)&data[10]) = 0;// number of stash

		TCustomDll* currentDll = customDlls;
		while (currentDll)
		{
			currentDll->initSharedSaveFile(&data, &maxSize, size);
			currentDll=currentDll->nextDll;
		}
	}

	return data;
}


int loadSharedSaveFile(Unit* ptChar, BYTE data[], DWORD maxSize, DWORD* consumedSize)
{
	if (consumedSize) *consumedSize = 0;
	if (!ptChar || !data || maxSize < FILE_MIN_SIZE) return 9;

	log_msg("Load shared file\n");

	DWORD curSize = 0;

	if (*(DWORD*)&data[curSize] != FILE_SHAREDSTASH)
	{
		log_msg("loadSharedSaveFile -> bad header\n");
		return 9;
	}
	curSize += 4;

	if (*(WORD *)&data[curSize] == 0x3130) {//"01"
		curSize += 2;
		PCPY->sharedGold = 0;
	} else if (*(WORD *)&data[curSize] == FILE_VERSION) {
		if (maxSize < 14) return 9;
		curSize += 2;
		PCPY->sharedGold = *(DWORD*)&data[curSize];
		curSize += 4;
	} else {
		log_msg("loadSharedSaveFile -> bad file version : %04X\n", *(WORD *)&data[curSize]);
		return 9;
	}

	int ret = loadStashList(ptChar, data, maxSize, &curSize, true);

	TCustomDll* currentDll = customDlls;
	while (!ret && currentDll)
	{
		ret = currentDll->loadSharedSaveFile(ptChar, data, maxSize, &curSize);
		currentDll=currentDll->nextDll;
	}

	PCPY->sharedStashIsOpened = true;
	if (!ret && consumedSize) *consumedSize = curSize;
	return ret;
}


void writeSharedSaveFile(char* name, BYTE* data, DWORD size, bool isHardcore)
{
	if (!legacySharedStashAvailable(isHardcore))
	{
		log_box("PlugY refused a legacy shared stash save: a SQLite stash exists or its path cannot be checked.\nThe .sss file was left unchanged. SQLite stashes require Diablo II 1.14d single player.");
		return;
	}
	char szSaveName[MAX_PATH];
	if (!getSharedSaveFilePath(szSaveName, isHardcore))
	{
		log_box("PlugY could not save the shared stash: the save path is too long.\nThe previous shared stash file was left unchanged.");
		return;
	}
	log_msg("Shared file for saving : %s\n", szSaveName);

	HANDLE hLock = lockSharedSaveFile(szSaveName);
	bool saved = legacySharedStashAvailable(isHardcore) && writeFileAtomic(szSaveName, data, size);
	unlockSharedSaveFile(hLock);
	// The box is raised only after unlocking, so a player reading it never blocks other games.
	if (!saved)
		log_box("PlugY could not save the shared stash %s (read-only, locked by another program, or disk full).\nThe file on disk was not changed. An unsaved copy may be kept next to it as a .tmp file.", szSaveName);
}


static bool isSaveFileMissing(const char* path)
{
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
	{
		DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}
	return !info.nFileSizeLow && !info.nFileSizeHigh;
}


void initSharedStashBaseline(Unit* ptChar, DWORD diskHash, bool diskExisted)
{
	PCPY->sharedStateFlags = 0;
	PCPY->sharedDiskExisted = diskExisted;
	PCPY->sharedDiskHash = diskExisted ? diskHash : 0;
	PCPY->sharedBaselineHash = 0;
	PCPY->sharedConflictStamp[0] = NULL;

	// taken after any item moved while loading: the move alone is not a change of this game
	DWORD baselineHash;
	if (!getSharedStashCanonicalHash(ptChar, &baselineHash))
	{
		log_msg("initSharedStashBaseline : can't compute the shared stash hash, every save counts as modified\n");
		return;
	}
	PCPY->sharedBaselineHash = baselineHash;
	PCPY->sharedBaselineValid = 1;
	log_msg("Shared stash baseline : %08X, file hash : %08X, file existed : %d\n", PCPY->sharedBaselineHash, PCPY->sharedDiskHash, diskExisted);
}


// Writes this game's version next to the shared file; one name per game, so later saves of the same game replace it.
static bool writeSharedConflictFile(Unit* ptChar, BYTE* data, DWORD size, bool isHardcore, char* szConflictName)
{
	if (!PCPY->sharedConflictStamp[0])
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		_snprintf(PCPY->sharedConflictStamp, sizeof(PCPY->sharedConflictStamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		PCPY->sharedConflictStamp[sizeof(PCPY->sharedConflictStamp)-1] = NULL;
	}
	char suffix[64];
	sprintf(suffix, ".conflict-%s-%u.sss", PCPY->sharedConflictStamp, GetCurrentProcessId());
	if (!getSaveFilePath(szConflictName, MAX_PATH, separateHardSoftStash && isHardcore ? "_LOD_HC_" : "_LOD_", sharedStashFilename, suffix))
	{
		strcpy(szConflictName, "(path too long)");
		return false;
	}
	return writeFileAtomic(szConflictName, data, size);
}


static const char* conflictTail(bool saved, const char* szConflictName, char* tail)
{
	sprintf(tail, saved ? "This game's version was saved to %s; merge the two by hand." : "This game's version could not be saved either (%s).", szConflictName);
	return tail;
}


// The save writes nothing to the shared file: the stash matches its load-time baseline and no conflict file was written
// (*unchanged), no item moved while loading waits to be saved, and the file is still on disk. path NULL: no valid path.
// writeSharedSaveFileIfChanged and sharedSaveIsUnchanged both decide through this one function, so they never drift.
static bool sharedSaveWritesNothing(Unit* ptChar, bool canonicalValid, DWORD canonicalHash, const char* path, bool* unchanged)
{
	// once this game wrote a conflict file it is kept current, or merging it later could duplicate items taken back
	*unchanged = PCPY->sharedBaselineValid && canonicalValid && canonicalHash == PCPY->sharedBaselineHash && !PCPY->sharedConflictStamp[0];
	return *unchanged && !PCPY->sharedMovedOnLoad && path && !isSaveFileMissing(path);
}


bool sharedSaveIsUnchanged(Unit* ptChar, bool isHardcore)
{
	if (!legacySharedStashAvailable(isHardcore))
		return false;
	DWORD canonicalHash;
	bool canonicalValid = getSharedStashCanonicalHash(ptChar, &canonicalHash);
	char path[MAX_PATH];
	bool unchanged;
	return sharedSaveWritesNothing(ptChar, canonicalValid, canonicalHash, getSharedSaveFilePath(path, isHardcore) ? path : NULL, &unchanged);
}


#ifdef PLUGY_TEST_HOOKS
// A dry run of writeSharedSaveFileIfChanged's no-write decision, spelled out apart from sharedSaveWritesNothing and
// checked against sharedSaveIsUnchanged on every save; the rig fails a run that logs a mismatch.
void checkSharedSaveParity(Unit* ptChar, bool isHardcore)
{
	DWORD canonicalHash;
	bool canonicalValid = getSharedStashCanonicalHash(ptChar, &canonicalHash);
	bool unchanged = PCPY->sharedBaselineValid && canonicalValid && canonicalHash == PCPY->sharedBaselineHash && !PCPY->sharedConflictStamp[0];
	bool onlyMovedOnLoad = unchanged && PCPY->sharedMovedOnLoad;
	char path[MAX_PATH];
	bool writesNothing = legacySharedStashAvailable(isHardcore) && getSharedSaveFilePath(path, isHardcore) && unchanged && !onlyMovedOnLoad && !isSaveFileMissing(path);
	bool query = sharedSaveIsUnchanged(ptChar, isHardcore);
	if (writesNothing != query)
		log_msg("!!! SHARED SAVE PARITY MISMATCH: dry run says %s, sharedSaveIsUnchanged says %s\n", writesNothing ? "unchanged" : "changed", query ? "unchanged" : "changed");
}
#endif


// Never overwrites a shared file that another process saved after this game loaded it. A save other than the leaving one
// writes only when that is safe, and otherwise leaves the file to a later save with no conflict file and no box: a box
// would stall a bot mid-game, and a conflict found then would turn shared moves off for the rest of the game.
// Returns true when the file holds this game's shared items (written, unchanged, or only moved while loading).
bool writeSharedSaveFileIfChanged(Unit* ptChar, BYTE* data, DWORD size, bool isHardcore, bool isLeaving)
{
	if (!legacySharedStashAvailable(isHardcore))
	{
		log_msg("Shared stash save refused: a SQLite stash exists or its path cannot be checked; legacy .sss writes are disabled.\n");
		return false;
	}
	DWORD newHash = fnv1a(data, size);
	DWORD canonicalHash;
	bool canonicalValid = getSharedStashCanonicalHash(ptChar, &canonicalHash);

	char szSaveName[MAX_PATH];
	bool pathValid = getSharedSaveFilePath(szSaveName, isHardcore);
	bool unchanged;
	bool writesNothing = sharedSaveWritesNothing(ptChar, canonicalValid, canonicalHash, pathValid ? szSaveName : NULL, &unchanged);
	// Items moved while loading are saved only over the file as it was loaded: several games loading the same file
	// move the same items, and the first one to save stores the move for all of them.
	bool onlyMovedOnLoad = unchanged && PCPY->sharedMovedOnLoad;

#ifdef PLUGY_TEST_HOOKS
	checkSharedSaveParity(ptChar, isHardcore);
#endif

	char message[1024];
	message[0] = NULL;
	// an unchanged stash already matches the file item for item: items moved while loading only changed places
	bool saved = onlyMovedOnLoad;
	if (!pathValid)
	{
		_snprintf(message, sizeof(message), "PlugY could not save the shared stash: the save path is too long.\nThe shared stash file on disk was not changed.");
	}
	else if (writesNothing)
	{
		log_msg("shared stash unchanged since load; not rewritten\n");
		return true;
	}
	else
	{
		HANDLE hLock = lockSharedSaveFile(szSaveName, isLeaving ? 20000 : 1000);
		if (!legacySharedStashAvailable(isHardcore))
		{
			unlockSharedSaveFile(hLock);
			log_msg("Shared stash save refused after locking: SQLite owns this stash.\n");
			return false;
		}
		char szConflictName[MAX_PATH];
		char tail[MAX_PATH + 64];
		const char* conflictResult;

		if (!hLock && onlyMovedOnLoad)
		{
			log_msg("shared stash busy; the items moved while loading are saved by a later save\n");
		}
		else if (!hLock && !isLeaving)
		{
			log_msg("shared stash busy; left for a later save\n");
		}
		else if (!hLock)
		{
			// Without the lock another game may be replacing the file right now, so only this game's own file is written.
			conflictResult = conflictTail(writeSharedConflictFile(ptChar, data, size, isHardcore, szConflictName), szConflictName, tail);
			_snprintf(message, sizeof(message), "PlugY could not lock the shared stash %s (another game kept it busy), so it was not overwritten.\n%s", szSaveName, conflictResult);
		} else {
			BYTE* diskData = NULL;
			DWORD diskSize = 0;
			DWORD diskHash = 0;
			int diskState = readSaveFileData(szSaveName, &diskData, &diskSize, FILE_MIN_SIZE);
			if (diskState > 0)
			{
				diskHash = fnv1a(diskData, diskSize);
				D2FogMemDeAlloc(diskData,__FILE__,__LINE__,0);
			}

			// A missing file has nothing left to protect; an unreadable one or one another game saved does.
			if (!diskState || (diskState > 0 && PCPY->sharedDiskExisted && diskHash == PCPY->sharedDiskHash))
			{
				log_msg("Shared file for saving : %s\n", szSaveName);
				if (writeFileAtomic(szSaveName, data, size))
				{
					saved = true;
					PCPY->sharedDiskHash = newHash;
					PCPY->sharedDiskExisted = 1;
					PCPY->sharedBaselineHash = canonicalValid ? canonicalHash : 0;
					PCPY->sharedBaselineValid = canonicalValid;
					PCPY->sharedMovedOnLoad = 0;
				} else if (onlyMovedOnLoad) {
					log_msg("can't save the items moved while loading; the shared stash file is unchanged\n");
				} else if (!isLeaving) {
					log_msg("can't save the shared stash %s now; left for a later save\n", szSaveName);
				} else {
					conflictResult = conflictTail(writeSharedConflictFile(ptChar, data, size, isHardcore, szConflictName), szConflictName, tail);
					_snprintf(message, sizeof(message), "PlugY could not save the shared stash %s (read-only, locked by another program, or disk full); it was not changed.\n%s", szSaveName, conflictResult);
				}
			} else if (onlyMovedOnLoad) {
				// this game changed nothing itself, and the newer file already has the move or other games' changes
				log_msg("another game saved the shared stash; this game only moved items while loading, nothing to save\n");
				PCPY->sharedMovedOnLoad = 0;
			} else if (!isLeaving) {
				log_msg("another game saved the shared stash after this game loaded it, or it can't be read; left to the save made when leaving\n");
			} else {
				conflictResult = conflictTail(writeSharedConflictFile(ptChar, data, size, isHardcore, szConflictName), szConflictName, tail);
				_snprintf(message, sizeof(message), "Another game saved the shared stash %s after this game loaded it (or it can no longer be read), so it was not overwritten.\n%s", szSaveName, conflictResult);
			}
		}
		unlockSharedSaveFile(hLock);
	}

	// Only the leaving save shows a box, once per game, since a box stalls the game until it is answered. Raised after
	// unlocking, so it never blocks other games.
	if (!message[0])
		return saved;
	message[sizeof(message)-1] = NULL;
	if (PCPY->sharedInConflict || !isLeaving)
		log_msg("%s\n", message);
	else
	{
		PCPY->sharedInConflict = 1;
		log_box("%s", message);
	}
	return saved;
}

void saveSharedSaveFile(Unit* ptChar, BYTE** data, DWORD* maxSize, DWORD* curSize)
{
	*(DWORD *)(*data + *curSize) = FILE_SHAREDSTASH;
	*curSize += 4;
	if (PCPY->sharedGold)
	{
		*(WORD *)(*data + *curSize) = FILE_VERSION;
		*curSize += 2;

		*(DWORD *)(*data + *curSize) = PCPY->sharedGold;
		*curSize += 4;
	} else {
		*(WORD *)(*data + *curSize) = 0x3130;
		*curSize += 2;
	}
	saveStashList(ptChar, PCPY->sharedStash, data, maxSize, curSize);

	TCustomDll* currentDll = customDlls;
	while (currentDll)
	{
		currentDll->saveSharedSaveFile(ptChar, data, maxSize, curSize);
		currentDll=currentDll->nextDll;
	}
}


/*================================= END OF FILE =================================*/
