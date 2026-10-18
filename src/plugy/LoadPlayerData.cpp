/*=================================================================
	File created by Yohann NICOLAS.
	Add support 1.13d by L'Autour.
	Add support 1.14d by haxifix.

	Load Player Custom Data.

=================================================================*/

#include "infinityStash.h"
#include "extendedSaveFile.h"
#include "sharedSaveFile.h"
#include "loadPlayerData.h"
#include "savePlayerData.h"
#include "saveTransfer.h"
#include "common.h"


/************************************ LOADING ***********************************/

// The game only says "Unable to enter game", so the player is told which PlugY file was refused.
static void showLoadError(const char* filePath, const char* extension)
{
	if (filePath[0])
		log_box("PlugY can't load %s: the file is unreadable, truncated or damaged.\nThe character was not loaded and no file was overwritten. Restore it from a backup, or close the program that locks it.", filePath);
	else
		log_box("PlugY can't load the %s file of this character: its path is too long.\nThe character was not loaded and no file was overwritten.", extension);
}

DWORD STDCALL LoadSPCustomData(Unit* ptChar)
{
	DWORD size = 0;
	BYTE* data;

	log_msg("--- Start LoadSPCustomData ---\n");
	active_sharedStash = active_SharedStashInMultiPlayer > 0;
	log_msg("active_sharedStash = %d : %d\n\n", active_sharedStash);

	if (!ptChar)
		{log_msg("LoadSPCustomData : ptChar == NULL\n");return 0x1B;}//Unknow failure
	if (ptChar->nUnitType != UNIT_PLAYER)
		{log_msg("LoadSPCustomData : ptChar->nUnitType != UNIT_PLAYER\n");return 0x1B;}//Unknow failure
	if (!PCPlayerData)
		{log_msg("LoadSPCustomData : PCPlayerData == NULL\n");return 0x1B;}//Unknow failure
	if (!PCGame)
		{log_msg("LoadSPCustomData : PCGame == NULL\n");return 0x1B;}//Unknow failure

	DWORD ret=0;
	if (PCGame->isLODGame)
	{
		log_msg("is LOD Game\n");
		data = readExtendedSaveFile(PCPlayerData->name, &size);
		if (!data)
		{
			log_msg("LoadSPCustomData : can't read the extended save file\n");
			ret = 0xE;//Unable to enter game, generic bad file
		} else {
			ret = loadExtendedSaveFile(ptChar, data, size);
			D2FogMemDeAlloc(data,__FILE__,__LINE__,0);
		}
		if (ret)
		{
			char filePath[MAX_PATH];
			if (!getSaveFilePath(filePath, MAX_PATH, PCPlayerData->name, ".d2x"))
				filePath[0] = NULL;
			showLoadError(filePath, ".d2x");
		}
		if (!ret && active_sharedStash)
		{
			bool sharedFileExisted = false;
			char filePath[MAX_PATH];
			data = readSharedSaveFile(PCPlayerData->name, &size, &sharedFileExisted, filePath);
			if (!data)
			{
				log_msg("LoadSPCustomData : can't read the shared save file\n");
				ret = 0xE;//Unable to enter game, generic bad file
			} else {
				DWORD sharedDiskHash = fnv1a(data, size);
				ret = loadSharedSaveFile(ptChar, data, size);
				D2FogMemDeAlloc(data,__FILE__,__LINE__,0);
				if (!ret)
					initSharedStashBaseline(ptChar, sharedDiskHash, sharedFileExisted);
			}
			if (ret)
				showLoadError(filePath, ".sss");
		}
		if (!ret)
			showMovedStashItemsNotice(ptChar);
	} else {
		log_msg("is not LOD Game\n");
	}

	log_msg("End LoadSPCustomData.\n\n");
	return ret;
}

// A killed save can leave the temp file of a journal write behind (<name>.pyj.<pid>[.<tick>].tmp, see writeFileAtomic).
// It is never the only copy of anything: the journal it was to replace, or the files that journal describes, hold the
// save. Called with the session lock held and recovery done, so no game of this character is writing one now.
static void deleteJournalTempFiles(const char* name)
{
	char folder[MAX_PATH];
	char pattern[MAX_PATH];
	char prefix[COMMIT_NAME_SIZE + 8];
	if (!getSaveFilePath(folder, MAX_PATH, "") || !formatPath(pattern, MAX_PATH, "%s%s.pyj.*.tmp", folder, name)
	 || !formatPath(prefix, sizeof(prefix), "%s.pyj.", name))
		return;
	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(pattern, &findData);
	if (hFind == INVALID_HANDLE_VALUE)
		return;
	size_t prefixLen = strlen(prefix);
	int deleted = 0;
	int kept = 0;
	do
	{
		// The pattern can also match 8.3 short names: only <name>.pyj.<...>.tmp itself goes.
		size_t len = strlen(findData.cFileName);
		if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || len <= prefixLen + 4
		 || _strnicmp(findData.cFileName, prefix, prefixLen) || _stricmp(findData.cFileName + len - 4, ".tmp"))
			continue;
		char path[MAX_PATH];
		if (formatPath(path, MAX_PATH, "%s%s", folder, findData.cFileName) && DeleteFileA(path))
			deleted++;
		else
			kept++;
	}
	while (FindNextFileA(hFind, &findData));
	FindClose(hFind);
	if (deleted || kept)
		log_msg("Leftover journal temp files of %s: %d deleted, %d could not be deleted\n", name, deleted, kept);
}

// The character whose last load met a journal or file it could not read ("" otherwise).
static char readErrorName[COMMIT_NAME_SIZE] = "";
// The character whose .d2s the game opened after its lock and recovery ("" otherwise): set by OpenSPCharacter_114,
// cleared once that load is built or fails.
static char openedName[COMMIT_NAME_SIZE] = "";

static void copyLoadName(char* out, const char* name)
{
	strncpy(out, name, COMMIT_NAME_SIZE - 1);
	out[COMMIT_NAME_SIZE - 1] = NULL;
}

// Every single-player load on 1.14d, whatever ActiveCrashSafeSaves says, before the game reads any file of the
// character: the character is locked, and a save that a crash interrupted is finished or undone first. settled: the
// lock had to be waited for, or recovery found a journal.
static bool prepareSPCharacterFiles(const char* name, bool* settled)
{
	*settled = false;
	// D2Bot reuses Game.exe. With the multi-page stash, the character played before already released its lock when its
	// server PlayerData was freed, after its last save; without it, that lock lasts until this process loads another
	// character (here), a load of it fails, or the process exits. It is never released by a leaving save, which may
	// still be running.
	releaseSessionLockUnlessNamed(name);
	bool waited = false;
	DWORD error = 0;
	if (!acquireSessionLock(name, 5000, &waited, &error))
	{
		char lockPath[MAX_PATH];
		if (!getSaveFilePath(lockPath, MAX_PATH, name, ".pylive"))
			strcpy(lockPath, "(path too long)");
		log_box("PlugY can't load %s: this character is already running in another game (or its lock file %s can't be opened, error %u).\nClose the other game, then load the character again.", name, lockPath, error);
		return false;
	}

	CharacterRecovery recovery;
	if (!recoverCharacterFiles(name, false, &recovery))
	{
		bool readError = recovery.verdict == COMMIT_VERDICT_READ_ERROR && !recovery.actionFailed;
		// Another program holding a file a moment is retried silently by the next load; a lasting error, or the same
		// one twice in a row, needs the player.
		bool transient = recovery.readError == ERROR_SHARING_VIOLATION || recovery.readError == ERROR_LOCK_VIOLATION;
		if (!readError || !transient || !_stricmp(readErrorName, name))
			showRecoveryRefusal(name, &recovery, true);
		else
			log_msg("Load of %s refused once: %s can't be read now (error %u)\n", name, recovery.file, recovery.readError);
		if (readError)
			copyLoadName(readErrorName, name);
		else
			readErrorName[0] = NULL;
		releaseSessionLockNamed(name);
		return false;
	}
	readErrorName[0] = NULL;
	deleteJournalTempFiles(name);
	*settled = waited || recovery.verdict != COMMIT_VERDICT_NONE;
	return true;
}

#ifdef PLUGY_TEST_HOOKS
// PLUGY_LOAD_HOLD=<file>: a load waits at the open of the .d2s while that file exists, so a test can act meanwhile.
static void holdLoadForTest()
{
	char path[MAX_PATH];
	DWORD len = GetEnvironmentVariableA("PLUGY_LOAD_HOLD", path, sizeof(path));
	if (!len || len >= sizeof(path) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		return;
	log_msg("Test hook: load held\n");
	while (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
		Sleep(20);
	log_msg("Test hook: load released\n");
}
#endif

typedef void* (__cdecl *TGameOpenFileCdecl)(const char* path, const char* mode);
typedef size_t (__cdecl *TGameReadFileCdecl)(void* buffer, size_t size, size_t count, void* file);

// The open of the .d2s of a single-player load, name being the character the server loads. The game's own fopen opens
// it, since the game's fread and fclose take the FILE*. NULL fails the load with 0xE before any player is built.
void* STDCALL OpenSPCharacter_114(const char* name, const char* path, const char* mode)
{
	openedName[0] = NULL;
#ifdef PLUGY_TEST_HOOKS
	holdLoadForTest();
#endif
	// No .d2s means no character to load: the game fails as it always did, and recovery doesn't archive the journal of
	// a character whose file is only out of place.
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
	{
		DWORD error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
		{
			log_msg("Load of %s: %s is missing\n", name, path);
			return ((TGameOpenFileCdecl)D2GameOpenFile)(path, mode);
		}
	}
	bool settled;
	if (!prepareSPCharacterFiles(name, &settled))
		return NULL;
	SetLastError(0);
	void* file = ((TGameOpenFileCdecl)D2GameOpenFile)(path, mode);
	if (!file)
	{
		log_msg("Load of %s: the game can't open %s (last error %u)\n", name, path, GetLastError());
		releaseSessionLockNamed(name);
		return NULL;
	}
	copyLoadName(openedName, name);
	return file;
}

// A load that fails once its .d2s was open plays nothing: the character's lock goes.
void STDCALL abandonSPCharacterLoad()
{
	if (openedName[0])
		releaseSessionLockNamed(openedName);
	openedName[0] = NULL;
}

// The read of the .d2s the open let through, by the game's own fread. A read that returns nothing (an empty file, or
// another program locking its bytes) takes the game's no-file exit past 0x53447A, so the load is abandoned here.
size_t __cdecl ReadSPCharacter_114(void* buffer, size_t size, size_t count, void* file)
{
	size_t done = ((TGameReadFileCdecl)D2GameReadFile)(buffer, size, count, file);
	if (!done && openedName[0])
	{
		log_msg("Load of %s: its .d2s read nothing\n", openedName);
		abandonSPCharacterLoad();
	}
	return done;
}

// Once the game built the player from the .d2s that OpenSPCharacter_114 let it read.
DWORD STDCALL LoadSPCharacter_114(Unit* ptChar)
{
	if (ptChar && ptChar->nUnitType == UNIT_PLAYER && PCPlayerData)
	{
		char* name = PCPlayerData->name;
		// The game names the player and its .d2s path from the same client record and refuses a file whose header holds
		// another name, so only an open that bypassed OpenSPCharacter_114 lands here. Its lock and recovery come after
		// the game's read: when they had to wait or found a journal, the player may stand on an outdated .d2s.
		if (_stricmp(openedName, name))
		{
			log_msg("Load of %s: its .d2s was opened without the session lock\n", name);
			bool settled;
			if (!prepareSPCharacterFiles(name, &settled))
			{
				openedName[0] = NULL;
				return 0xE;
			}
			copyLoadName(openedName, name);
			if (settled)
			{
				log_msg("Load of %s refused once: its files were settled after the game read them\n", name);
				abandonSPCharacterLoad();
				return 0xE;
			}
		}
		setSessionLockOwner(PCPlayerData);
		markCharacterLoaded(name, PCGame);
	}
	else
		abandonSPCharacterLoad();
	DWORD ret = active_PlayerCustomData ? LoadSPCustomData(ptChar) : 0;
	if (ret)
		abandonSPCharacterLoad();
	openedName[0] = NULL;
	return ret;
}

// Replaces the call of the game's fopen(path, "rb") at 0x534410 that opens the .d2s, ESI holding the name the server
// loads. NULL takes vanilla's no-file exit: the load fails with 0xE and no player is built. Installed by
// Install_CharacterCommit.
FCT_ASM (caller_OpenSPCharacter_114 )
	PUSH DWORD PTR SS:[ESP+8]
	PUSH DWORD PTR SS:[ESP+8]
	PUSH ESI
	CALL OpenSPCharacter_114
	RETN
}}

// Replaces MOV EDI,EAX / TEST EDI,EDI / JNZ 0x534493 after the parser (EAX 0: the player is built from the .d2s): a
// nonzero result takes vanilla's failure exit 0x534493, which returns EAX, with the code in EDI and EAX. Installed by
// Install_CharacterCommit.
FCT_ASM (caller_LoadSPCharacter_114 )
	MOV EDI,EAX
	TEST EDI,EDI
	JNZ failed_LoadSPCharacter
	PUSH DWORD PTR SS:[ESP+0x1C]
	CALL LoadSPCharacter_114
	MOV EDI,EAX
	TEST EDI,EDI
	JNZ JMP_LoadSPCharacter
	RETN
failed_LoadSPCharacter:
	CALL abandonSPCharacterLoad
	MOV EAX,EDI
JMP_LoadSPCharacter:
	ADD DWORD PTR SS:[ESP],0x14
	RETN
}}

FCT_ASM (caller_LoadSPPlayerCustomData )
	MOV ESI,EAX
	TEST ESI,ESI
	JNZ JMP_LoadSPPlayerCustomData
	PUSH DWORD PTR SS:[ESP+0x14]
	CALL LoadSPCustomData
	MOV ESI,EAX
	TEST ESI,ESI
	JNZ JMP_LoadSPPlayerCustomData
	RETN
JMP_LoadSPPlayerCustomData:
	ADD DWORD PTR SS:[ESP],0x17
	RETN
}}


/*
return value :
0:ok
1:Unable to enter game. Bad character version
2:Unable to enter game. Bad character quest data
3:Unable to enter game. Bad character waypoint data
4:Unable to enter game. Bad character stats data
5:Unable to enter game. Bad character skills data
6:Unable to enter game
7:Unable to enter game. Bad inventory data
8:Unable to enter game. Bad dead bodies
9:Unable to enter game. Bad header
0A:Unable to enter game. bad hireables
0B:Unable to enter game, bad intro data
0C:Unable to enter game. bad item
0D:Unable to enter game, bad dead body item
0E:Unable to enter game, generic bad file
0F:Game is full
10:Version do not match....
11:Unable to enter game,Your character must kill Baal to play nightmare game
12:Unable to enter game,Your character must kill Baal in nightmare difficulty to play hell game
13:Unable to enter game, a normal character cannot join a game create an harcore character.
14:Unable to enter game, an harcore character cannot join a game create by a normal character.
15:A dead character cannot join or create any game
16:unknown failure
17:Unable to enter game. A D2 Character cannot join an expansion character
18:Expansion cannot join D2
19:Failures to join game
1A:Unable to enter game
1B:unknown failure
*/
enum {
	TS_SAVE_PERSONAL=0,
	TS_SAVE_SHARED,
	TS_PUTGOLD,
	TS_TAKEGOLD
};

#pragma pack(1)
typedef struct {
	BYTE	packID;
	BYTE	packSize;
	DWORD	finalSize:31;
	DWORD	isCustom:1;
	BYTE	type;
	BYTE	data[0xFF];
} t_rcvMsg;
#pragma pack()

struct s_MPSaveFile
{
	DWORD clientID;
	int sizeExtended;
	int curExtended;
	BYTE* dataExtended;
	int sizeShared;
	int curShared;
	BYTE* dataShared;
	s_MPSaveFile* prev;
	s_MPSaveFile* next;
	bool completed;
	// A refused chunk: the rest of this client's transfer is dropped, and LoadMPCustomData refuses the character.
	bool failed;
};

static BYTE customPackID = 0x6C;
static s_MPSaveFile * receivedSaveFiles = NULL;

void freeCurrentCF(DWORD memoryPool, s_MPSaveFile** curSF)
{
	if (*curSF == NULL) return;
	D2FreeMem(memoryPool, (*curSF)->dataExtended,__FILE__,__LINE__,0);
	D2FreeMem(memoryPool, (*curSF)->dataShared,__FILE__,__LINE__,0);
	if ((*curSF)->next)
		(*curSF)->next->prev = (*curSF)->prev;
	if ((*curSF)->prev)
		(*curSF)->prev->next = (*curSF)->next;
	else
		receivedSaveFiles = (*curSF)->next;
	D2FreeMem(memoryPool, *curSF,__FILE__,__LINE__,0);
	*curSF = NULL;
}

void discardClientSaveFiles(DWORD clientID)
{
	s_MPSaveFile* curSF = receivedSaveFiles;
	while (curSF && (clientID != curSF->clientID))
		curSF = curSF->next;
	if (!curSF) return;
	log_msg("Discard the save files received from client %u\n", clientID);
	// Pool 0, the pool ReceiveSaveFiles allocates the entry from.
	freeCurrentCF(0, &curSF);
}


void sendData(BYTE* data, DWORD size, bool isShared)
{
	t_rcvMsg pack;
	// This header is 7 bytes, so packSize+7 sends one byte past the payload: with pack zeroed once, that byte is 0 or
	// an earlier chunk's data, never stack.
	ZeroMemory(&pack, sizeof(pack));
//	d2_assert(size >= 0x40000000, "size of file > 0x40000000", __FILE__, __LINE__);

	pack.packID = customPackID;
	pack.finalSize = size;
	pack.isCustom = true;
	pack.type = isShared ? TS_SAVE_SHARED : TS_SAVE_PERSONAL;

	DWORD sended = 0;
	while (sended < size)
	{
		pack.packSize = (BYTE) (size - sended > 0xFE ? 0xFF : size - sended + 1);
		CopyMemory(pack.data, &data[sended], pack.packSize - 1);
		log_msg("Loading Send Packet: \ttype=%X\t finalSize=%X\t packSize=%02X\t data=%08X\n", pack.type, pack.finalSize, pack.packSize, pack.data);
		D2SendToServer(pack.packSize+7, 0, &pack);
		sended += pack.packSize -1;
	}
	log_msg("\n");
}


char joinedCharName[17] = "";

void FASTCALL SendSaveFiles (char* ptPath, DWORD maxsize, char* name)
{
	D2FogGetSavePath(ptPath,maxsize);
	// The client's own pick, kept for the save the server sends back when it leaves (SaveMPPlayerCustomData).
	strncpy(joinedCharName, name, sizeof(joinedCharName) - 1);
	joinedCharName[sizeof(joinedCharName) - 1] = '\0';

	log_msg("\n--- Start SendSaveFiles ---\n");
	active_sharedStash = active_SharedStashInMultiPlayer == 2;
	log_msg("active_sharedStash = %d : %d\n\n", active_sharedStash);

	// Send Extended Save File
	log_msg("Send Extended Save File\n");
	DWORD sizeExtended = 0;
	BYTE* dataExtended = readExtendedSaveFile(name, &sizeExtended);
	if (!dataExtended)
	{
		// Without the extended file the server refuses the character, so the shared file is not sent either.
		log_msg("Can't read the extended save file, no save file is sent\n");
		log_msg("End SendSaveFiles.\n\n");
		return;
	}
	sendData(dataExtended, sizeExtended, false);
	D2FogMemDeAlloc(dataExtended,__FILE__,__LINE__,0);

	// Send Shared Save File
	//if (active_sharedStash)
	{
		log_msg("Send Shared Save File\n");
		DWORD sizeShared = 0;
		BYTE* dataShared = readSharedSaveFile(name, &sizeShared);
		if (dataShared)
		{
			sendData(dataShared, sizeShared, true);
			D2FogMemDeAlloc(dataShared,__FILE__,__LINE__,0);
		} else
			log_msg("Can't read the shared save file, it is not sent\n");
	}

	// Ending load
	log_msg("End SendSaveFiles.\n\n");
}



DWORD STDCALL ReceiveSaveFiles (DWORD clientID, t_rcvMsg* msg)
{
	if( (msg->packID != customPackID) || !msg->isCustom) return msg->packID;

	log_msg("Loading Receive Packet: clientID=%d\t type=%X\t finalSize=%X\t packSize=%02X\t data=%08X\n", clientID, msg->type, msg->finalSize, msg->packSize, msg->data);
	active_sharedStash = active_SharedStashInMultiPlayer == 2;
	log_msg("active_sharedStash = %d : %d\n\n", active_sharedStash);

	bool isShared;

	switch (msg->type)
	{
	case TS_SAVE_PERSONAL :	isShared = false;break;
	case TS_SAVE_SHARED :	isShared = true;break;
//	case TS_PUTGOLD :		putGold(ptChar, (DWORD)msg->data); return 0;
//	case TS_TAKEGOLD :		takeGold(ptChar, (DWORD)msg->data); return 0;
	default: return 0;//return msg->packID;
	}

	NetClient* ptClient = ptClientTable[clientID & 0xFF];

	s_MPSaveFile* curSF = receivedSaveFiles;
	while (curSF && (clientID != curSF->clientID) )
		curSF = curSF->next;

	if (curSF && curSF->completed)
		freeCurrentCF(0 * PClientGame->memoryPool, &curSF);


	if (!curSF)
	{
		curSF = (s_MPSaveFile *)D2AllocMem(0 * PClientGame->memoryPool, sizeof(s_MPSaveFile),__FILE__,__LINE__,0);
		ZeroMemory(curSF, sizeof(s_MPSaveFile));
		curSF->clientID = clientID;
		curSF->next = receivedSaveFiles;
		if (receivedSaveFiles) receivedSaveFiles->prev = curSF;
		receivedSaveFiles = curSF;
	}

	log_msg("curSF = %08X\tcurSF->completed = %d\n", curSF, curSF->completed);

	// A refused chunk is still consumed (packID+10) so the game never handles it as one of its own packets.
	if (curSF->failed)
	{
		log_msg("ReceiveSaveFiles : the transfer of client %d was refused, chunk dropped\n", clientID);
		return msg->packID+10;
	}

	int& size = isShared ? curSF->sizeShared : curSF->sizeExtended;
	int& cur = isShared ? curSF->curShared : curSF->curExtended;
	BYTE*& data = isShared ? curSF->dataShared : curSF->dataExtended;
	const char* refused = NULL;
	switch (checkSaveChunk(size, cur, msg->packSize, msg->finalSize, SAVE_TRANSFER_MAX_SIZE))
	{
	case SAVE_CHUNK_START:
		size = msg->finalSize;
		data = (BYTE *)D2AllocMem(0 * PClientGame->memoryPool, size,__FILE__,__LINE__,0);
		if (!data)
			refused = "memory allocation failed";
		break;
	case SAVE_CHUNK_APPEND:
		break;
	default:
		refused = "invalid chunk header or size";
	}
	if (refused)
	{
		// The entry stays, not completed: LoadMPCustomData refuses the character (0xE) and frees it.
		log_msg("ReceiveSaveFiles : %s file of client %d refused (%s), the rest of its transfer is dropped\n", isShared ? "shared" : "extended", clientID, refused);
		if (curSF->dataExtended)
			D2FreeMem(0 * PClientGame->memoryPool, curSF->dataExtended,__FILE__,__LINE__,0);
		if (curSF->dataShared)
			D2FreeMem(0 * PClientGame->memoryPool, curSF->dataShared,__FILE__,__LINE__,0);
		curSF->dataExtended = NULL;
		curSF->dataShared = NULL;
		curSF->sizeExtended = 0;
		curSF->curExtended = 0;
		curSF->sizeShared = 0;
		curSF->curShared = 0;
		curSF->failed = true;
		return msg->packID+10;
	}

	DWORD payload = msg->packSize - 1;
	CopyMemory(&data[cur], msg->data, payload);
	cur += payload;

	if( curSF->sizeExtended && (curSF->sizeExtended == curSF->curExtended) && curSF->sizeShared && (curSF->sizeShared == curSF->curShared) )
	{
		curSF->completed = true;
		log_msg("ReceiveSaveFiles completed.\n");
	}
	log_msg("End Load Receive Packet\n");

	return msg->packID+10;
}

/*
typedef int (STDCALL * t_fct_recv)(SOCKET s, char *buf, int len, int flags);
t_fct_recv fct_recv;
int STDCALL ReceiveSaveFiles_9(DWORD clientID, SOCKET s, char *buf, int len, int flags)
{
	t_rcvMsg* msg = (t_rcvMsg*) buf;
	int nb = fct_recv(s,buf,len,flags);
	if ((nb<6) || (msg->packID != customPackID) || !msg->isCustom || (msg->packSize+7 != nb))
		return nb;
	ReceiveSaveFiles(clientID,msg);
	msg->packSize = 1;
	return 7;
}*/

DWORD STDCALL LoadMPCustomData(Unit* ptChar)
{
	log_msg("Start LoadMPCustomData\n");
	active_sharedStash = active_SharedStashInMultiPlayer == 2;
	log_msg("active_sharedStash = %d : %d\n\n", active_sharedStash);

	if (!ptChar) return NULL;
	if (!ptChar)
		{log_msg("LoadMPCustomData : ptChar == NULL\n");return 0x1B;}//Unknow failure
	if (ptChar->nUnitType != UNIT_PLAYER)
		{log_msg("LoadMPCustomData : ptChar->nUnitType != UNIT_PLAYER\n");return 0x1B;}//Unknow failure
	if (!PCPlayerData)
		{log_msg("LoadMPCustomData : PCPlayerData == NULL\n");return 0x1B;}//Unknow failure

	NetClient* ptClient = D2GetClient(ptChar,__FILE__,__LINE__);

	s_MPSaveFile* curSF = receivedSaveFiles;
	while (curSF && (ptClient->clientID != curSF->clientID) )
		curSF = curSF->next;

	DWORD ret=0;

	if (!curSF)
	{
		log_msg("Server has received no data from extra save files of character %s\n", PCPlayerData->name);
		ret = 0xE;//Unable to enter game, generic bad file
	}

	if (!ret && !curSF->completed)
	{
		log_msg("Server hasn't receive all data from extra save files of character %s\n",PCPlayerData->name);
		ret = 0xE;//Unable to enter game, generic bad file
	}

	if (!PCGame)
	{
		log_msg("LoadMPCustomData : PCGame == NULL\n");
		if (!ret)
			ret = 0x1B;//Unknow failure
	}
	else if (PCGame->isLODGame)
	{
		log_msg("is LOD Game\n");
		if (!ret)
			ret = loadExtendedSaveFile(ptChar, curSF->dataExtended, curSF->sizeExtended);
		if (!ret && active_sharedStash)
			ret = loadSharedSaveFile(ptChar, curSF->dataShared, curSF->sizeShared);
	} else {
		log_msg("is not LOD Game\n");
	}

	// Pool 0, the pool ReceiveSaveFiles allocates the entry from: Fog releases a block only through the pool that owns it.
	freeCurrentCF(0, &curSF);

	log_msg("--- End LoadMPCustomData. ---\n\n");

	return ret;
}

FCT_ASM ( caller_LoadMPPlayerCustomData_114 )
	PUSH DWORD PTR SS:[EDI]
	CALL LoadMPCustomData
	TEST EAX,EAX
	JNZ JMP_LoadMPlayerCustomData
	CMP DWORD PTR DS:[EDI],0
	JNZ Continue_LoadMP
	ADD DWORD PTR SS:[ESP],0x1D
Continue_LoadMP:
	RETN
JMP_LoadMPlayerCustomData:
	SUB DWORD PTR SS:[ESP],0x10
	RETN
}}


FCT_ASM ( caller_LoadMPPlayerCustomData_111 )
	PUSH DWORD PTR SS:[EBX]
	CALL LoadMPCustomData
	TEST EAX,EAX
	JNZ JMP_LoadMPlayerCustomData
	CMP DWORD PTR DS:[EBX],0
	JNZ Continue_LoadMP
	ADD DWORD PTR SS:[ESP],0x21
Continue_LoadMP:
	RETN
JMP_LoadMPlayerCustomData:
	SUB DWORD PTR SS:[ESP],0x12
	RETN
}}


FCT_ASM ( caller_LoadMPPlayerCustomData )
	PUSH DWORD PTR SS:[EDI]
	CALL LoadMPCustomData
	TEST EAX,EAX
	JNZ JMP_LoadMPlayerCustomData
	CMP DWORD PTR DS:[EDI],0
	JNZ Continue_LoadMP
	ADD DWORD PTR SS:[ESP],0x46
Continue_LoadMP:
	RETN
JMP_LoadMPlayerCustomData:
	SUB DWORD PTR SS:[ESP],0xD
	RETN
}}

FCT_ASM ( caller_LoadMPPlayerCustomData_9 )
	PUSH DWORD PTR SS:[EDI]
	CALL LoadMPCustomData
	TEST EAX,EAX
	JNZ JMP_LoadMPlayerCustomData
	CMP DWORD PTR DS:[EDI],0
	JNZ Continue_LoadMP
	ADD DWORD PTR SS:[ESP],0x1D
Continue_LoadMP:
	RETN
JMP_LoadMPlayerCustomData:
	SUB DWORD PTR SS:[ESP],0x13
	RETN
}}


FCT_ASM ( caller_SendSaveFiles_111 )
	POP EAX
	PUSH DWORD PTR CS:[EAX+0x01] //"name"
	PUSH EAX
	JMP SendSaveFiles
}}

FCT_ASM ( caller_SendSaveFiles )
	POP EAX
	PUSH DWORD PTR CS:[EAX+0x0F] //"name"
	PUSH EAX
	JMP SendSaveFiles
}}

FCT_ASM ( caller_ReceiveSaveFiles_114 )
	PUSH ECX
	PUSH EDX
	LEA EBX,DWORD PTR DS:[ESI+4]
	PUSH EBX		//Message
	MOV EBX,DWORD PTR SS:[ESI]
	PUSH EBX		//ID client
	CALL ReceiveSaveFiles
	POP EDX
	POP ECX
	RETN
}}


FCT_ASM ( caller_ReceiveSaveFiles_111 )
	PUSH ECX
	PUSH EDX
	LEA EBX,DWORD PTR DS:[EBP+4]
	PUSH EBX		//Message
	MOV EBX,DWORD PTR SS:[EBP]
	PUSH EBX		//ID client
	CALL ReceiveSaveFiles
	POP EDX
	POP ECX
	RETN
}}


FCT_ASM ( caller_ReceiveSaveFiles )
	PUSH ECX
	PUSH EDX
	LEA EDI,DWORD PTR DS:[ESI+4]
	PUSH EDI		//Message
	MOV EDI,DWORD PTR DS:[ESI]
	PUSH EDI		//ID client
	CALL ReceiveSaveFiles
	POP EDX
	POP ECX
	RETN
}}


FCT_ASM ( caller_BugFix109b )
	MOV EAX,DWORD PTR DS:[EDI+0xBD0]
	TEST EAX,EAX
	JE go_to_default
	MOV EAX,DWORD PTR SS:[ESP+0x20]
	MOV DWORD PTR SS:[ESP+0x24],EAX
	RETN
go_to_default:
	SUB DWORD PTR SS:[ESP],0x4B
	RETN
}}

FCT_ASM ( caller_BugFix109d )
	MOV EAX,DWORD PTR DS:[ESI+0xAB0]
	TEST EAX,EAX
	JE go_to_default
	MOV EAX,DWORD PTR SS:[ESP+0x20]
	MOV DWORD PTR SS:[ESP+0x24],EAX
	RETN
go_to_default:
	SUB DWORD PTR SS:[ESP],0x4B
	RETN
}}

void Install_LoadPlayerData()
{
	static int isInstalled = false;
	if (isInstalled) return;

	log_msg("Patch D2Game & D2Client for load Player's custom data. (LoadPlayerData)\n");

	// Load SP player custom data. On 1.14d Install_CharacterCommit owns this site: its stub loads the custom data too.
	if (version_D2Game != V114d)
	{
		mem_seek R8(D2Game, 5046F, 5086F, 5CB0F, BB8ED, 278CD, 465BD, 5638D, 3BCCD, 0000);
		memt_byte( 0x8B, 0xE8); // CALL caller_LoadSPPlayerCustomData
		MEMT_REF4( 0x75F685F0, caller_LoadSPPlayerCustomData);
		memt_byte( 0x16, 0x90); // NOP
	}
	//6FC8CB0F   8BF0             MOV ESI,EAX
	//6FC8CB11   85F6             TEST ESI,ESI
	//6FC8CB13   75 16            JNZ SHORT D2Game.6FC8CB2B
	//0203B8ED  |> 8BF0           MOV ESI,EAX
	//0203B8EF  |. 85F6           TEST ESI,ESI
	//0203B8F1  |. 75 16          JNZ SHORT D2Game.0203B909
	//01F978CD  |> 8BF0           MOV ESI,EAX
	//01F978CF  |. 85F6           TEST ESI,ESI
	//01F978D1  |. 75 16          JNZ SHORT D2Game.01F978E9
	//6FC665BD  |> 8BF0           MOV ESI,EAX
	//6FC665BF  |. 85F6           TEST ESI,ESI
	//6FC665C1  |. 75 16          JNZ SHORT D2Game.6FC665D9
	//6FC7638D  |> 8BF0           MOV ESI,EAX
	//6FC7638F  |. 85F6           TEST ESI,ESI
	//6FC76391  |. 75 16          JNZ SHORT D2Game.6FC763A9
	//6FC5BCCD  |> 8BF0           MOV ESI,EAX
	//6FC5BCCF  |. 85F6           TEST ESI,ESI
	//6FC5BCD1  |. 75 16          JNZ SHORT D2Game.6FC5BCE9

	// Load MP player custom data.
	mem_seek R8(D2Game, 50790, 50B90, 5CC66, BB777, 27757, 46447, 56217, 3BB57, 134572);
	memt_byte( 0x83, 0xE8);
	MEMT_REF4( version_D2Game >= V114d ? 0x1D74003F : version_D2Game >= V111 ? 0x2174003B : version_D2Game == V110 ? 0x4674003F : 0x1D74003F, version_D2Game >= V114d ? caller_LoadMPPlayerCustomData_114 : version_D2Game >= V111 ? caller_LoadMPPlayerCustomData_111 : version_D2Game == V110 ? caller_LoadMPPlayerCustomData: caller_LoadMPPlayerCustomData_9);
	//6FC8CC66  . 833F 00         CMP DWORD PTR DS:[EDI],0
	//6FC8CC69  . 74 46           JE SHORT D2Game.6FC8CCB1
	//0203B777  |> 833B 00        CMP DWORD PTR DS:[EBX],0
	//0203B77A  |. 74 21          JE SHORT D2Game.0203B79D
	//01F97757  |> 833B 00        CMP DWORD PTR DS:[EBX],0
	//01F9775A  |. 74 21          JE SHORT D2Game.01F9777D
	//6FC66447  |> 833B 00        CMP DWORD PTR DS:[EBX],0
	//6FC6644A  |. 74 21          JE SHORT D2Game.6FC6646D
	//6FC76217  |> 833B 00        CMP DWORD PTR DS:[EBX],0
	//6FC7621A  |. 74 21          JE SHORT D2Game.6FC7623D
	//6FC5BB57  |> 833B 00        CMP DWORD PTR DS:[EBX],0
	//6FC5BB5A  |. 74 21          JE SHORT D2Game.6FC5BB7D
	//00534572  |> 833F 00        CMP DWORD PTR DS:[EDI],0
	//00534575  |. 74 1D          JE SHORT Game.00534594

	// Send save files to Server.
	mem_seek R8(D2Client, CF42, CF32, D5A2, 733FC, 5DFDC, 7933C, 1457C, B638C, 7807E);
	MEMJ_REF4( D2FogGetSavePath, version_D2Game >= V111 ? caller_SendSaveFiles_111 : caller_SendSaveFiles);
	//6FAAD5A1  |. E8 88D10B00    CALL <JMP.&Fog.#10115>
	//6FB233FB  |. E8 CA8AF9FF    CALL <JMP.&Fog.#10115>
	//6FB0DFDB  |. E8 C6DEFAFF    CALL <JMP.&Fog.#10115>
	//6FB2933B  |. E8 6A2CF9FF    CALL <JMP.&Fog.#10115>
	//6FAC457B  |. E8 187AFFFF    CALL <JMP.&Fog.#10115>
	//6FB6638B  |. E8 2E5BF5FF    CALL <JMP.&Fog.#10115>
	//0047807D  |. E8 CEEFF8FF    CALL Game.00407050

	// Receive save files from client.
	mem_seek R8(D2Game, 183A, 183A, 191A, 376E9, 703D9, 624D9, CAF39, D53E9, 13F114);
	memt_byte( 0x8B ,0xE8);
	if ( version_D2Game >= V111 ) {
		MEMT_REF4( version_D2Game >= V114d ? 0x46B60F1E : 0xB60F005D, version_D2Game >= V114d ? caller_ReceiveSaveFiles_114 : caller_ReceiveSaveFiles_111);
		if (version_D2Game < V114a) memt_byte( 0x45 ,0x90);
		memt_byte( 0x04 ,0x90);
		//01FB76E9  |. 8B5D 00        MOV EBX,DWORD PTR SS:[EBP]
		//01FB76EC  |. 0FB645 04      MOVZX EAX,BYTE PTR SS:[EBP+4]
		//01FE03D9  |. 8B5D 00        MOV EBX,DWORD PTR SS:[EBP]
		//01FE03DC  |. 0FB645 04      MOVZX EAX,BYTE PTR SS:[EBP+4]
		//6FC824D9  |. 8B5D 00        MOV EBX,DWORD PTR SS:[EBP]
		//6FC824DC  |. 0FB645 04      MOVZX EAX,BYTE PTR SS:[EBP+4]
		//6FCEAF39  |. 8B5D 00        MOV EBX,DWORD PTR SS:[EBP]
		//6FCEAF3C  |. 0FB645 04      MOVZX EAX,BYTE PTR SS:[EBP+4]
		//6FCF53E9  |. 8B5D 00        MOV EBX,DWORD PTR SS:[EBP]
		//6FCF53EC  |. 0FB645 04      MOVZX EAX,BYTE PTR SS:[EBP+4]
		//0053F114  |. 8B1E           MOV EBX,DWORD PTR DS:[ESI]
		//0053F116  |. 0FB646 04      MOVZX EAX,BYTE PTR DS:[ESI+4]
	} else {
		MEMT_REF4( 0x04468A3E, caller_ReceiveSaveFiles);
		//6FC3191A  |. 8B3E           MOV EDI,DWORD PTR DS:[ESI]
		//6FC3191C  |. 8A46 04        MOV AL,BYTE PTR DS:[ESI+4]
	}

	if ( version_Fog <= V109d )
	{
		mem_seek R8(Fog, 47DE, 45AE, 0000, 0000, 0000, 0000, 0000, 0000, 0000);
		memt_byte( 0x8B ,0xE8);
		MEMT_REF4( 0x891C2444, version_Fog == V109b? caller_BugFix109b : caller_BugFix109d);
		memt_byte( 0x44 ,0x90);
		memt_byte( 0x24 ,0x90);
		memt_byte( 0x20 ,0x90);
		//6FF545AE  |> 8B4424 1C      |MOV EAX,DWORD PTR SS:[ESP+1C]           ;  Case 2 of switch 6FF5454C
		//6FF545B2  |. 894424 20      |MOV DWORD PTR SS:[ESP+20],EAX
	}

	if ( version_D2Game == V109b || version_D2Game == V109d )
		customPackID -= 3;
	else if ( version_D2Game == V110 )
		customPackID --;

	if ( version_D2Game == V114d )
	{
		// Keep the stash items the inventory loader can't place instead of letting it free them.
		mem_seek R8(D2Game, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 1336A7);
		MEMC_REF4( D2LoadPlaceItem, placeLoadedItem);
		//005336A6  |. E8 65DBFFFF    CALL Game.00531210
	}

	log_msg("\n");

	isInstalled = true;
}

/*================================= END OF FILE =================================*/
