/*=================================================================
	File created by Yohann NICOLAS.
	Add support 1.13d by L'Autour.

    Save Player Custom Data.

=================================================================*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "infinityStash.h"
#include "extendedSaveFile.h"
#include "sharedSaveFile.h"
#include "loadPlayerData.h"
#include "savePlayerData.h"
#include "saveTransfer.h"
#include "common.h"

bool active_AutoBackup = false;
int maxBackupPerCharacter = 20;

/*backup
Save SP :
00530441  |. E8 CAC5FFFF    CALL Game.0052CA10
	0052CA5A  |. E8 A1590000    |CALL Game.00532400                      ; \Game.00532400
		005324EB  |. E8 50FDFFFF    CALL Game.00532240                       ; \Game.00532240
			00532275  |. E8 B6C95256    CALL PlugY.caller_SaveSPPlayerCustomData
00530503  |. E8 98980000    |CALL Game.00539DA0
	00539E03   . E8 F82AFFFF    CALL Game.0052C900
		0052C93B  |. E8 C05A0000    CALL Game.00532400                       ; \Game.00532400
			005324EB  |. E8 50FDFFFF    CALL Game.00532240                       ; \Game.00532240
				00532275  |. E8 B6C95256    CALL PlugY.caller_SaveSPPlayerCustomData
Save MP :
00530441  |. E8 CAC5FFFF    CALL Game.0052CA10
	0052CA5A  |. E8 A1590000    |CALL Game.00532400                      ; \Game.00532400
		005324FD  |. E8 3EFEFFFF    CALL Game.00532340                       ; \Game.00532340
			00532399  |. E8 62D15256    CALL PlugY.caller_SendSaveFilesToSave_11>
00530503  |. E8 98980000    |CALL Game.00539DA0
	00539E03   . E8 F82AFFFF    CALL Game.0052C900
		0052C93B  |. E8 C05A0000    CALL Game.00532400                       ; \Game.00532400
			005324FD  |. E8 3EFEFFFF    CALL Game.00532340                       ; \Game.00532340
*/
static const char* backupExtensions[] = {".d2s", ".d2x", ".ma0", ".ma1", ".ma2", ".ma3", ".map"};
static const int nbBackupExtensions = sizeof(backupExtensions)/sizeof(backupExtensions[0]);

// True for the backup folders of this character only : <name>_YYYYMMDD_HHMMSS.
static bool isBackupFolder(WIN32_FIND_DATA* findData, const char* name)
{
	if (!(findData->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return false;
	const char* folder = findData->cFileName;
	int len = strlen(name);
	if (_strnicmp(folder, name, len) || (int)strlen(folder) != len + 16)
		return false;
	for (int i = 0; i < 16; i++)
	{
		char c = folder[len + i];
		if ((i == 0 || i == 9) ? c != '_' : (c < '0' || c > '9'))
			return false;
	}
	return true;
}

void backupSaveFiles(char* name, int isHardCoreGame)
{
	if (!active_AutoBackup)
		return;

	char savePath[MAX_PATH];
	char backupPath[MAX_PATH];
	char szSaveName[MAX_PATH];
	char szBackupName[MAX_PATH];

	D2FogGetSavePath(savePath, MAX_PATH);
	savePath[MAX_PATH-1] = NULL;

	// backupfiles
	time_t t = time(NULL);
	struct tm* tm = localtime(&t);
	if (!tm || !formatPath(backupPath, MAX_PATH, "%sbackup", savePath))
		return;
	CreateDirectory(backupPath, NULL);
	if (!formatPath(backupPath, MAX_PATH, "%sbackup\\%s_%04d%02d%02d_%02d%02d%02d\\", savePath, name, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec))
		return;
	CreateDirectory(backupPath, NULL);
	log_msg("backup directory:%s isHardcore:%d\n", backupPath, isHardCoreGame);

	for (int i = 0; i < nbBackupExtensions; i++)
		if (formatPath(szSaveName,   MAX_PATH, "%s%s%s", savePath,   name, backupExtensions[i])
		 && formatPath(szBackupName, MAX_PATH, "%s%s%s", backupPath, name, backupExtensions[i]))
			CopyFile(szSaveName, szBackupName, true);

	if (separateHardSoftStash && isHardCoreGame != 0)
	{
		if (formatPath(szSaveName,   MAX_PATH, "%s%s%s.sss", savePath,   "_LOD_HC_", sharedStashFilename)
		 && formatPath(szBackupName, MAX_PATH, "%s%s%s.sss", backupPath, "_LOD_HC_", sharedStashFilename))
			CopyFile(szSaveName, szBackupName, true);
	}
	if (!separateHardSoftStash || isHardCoreGame != 1)
	{
		if (formatPath(szSaveName,   MAX_PATH, "%s%s%s.sss", savePath,   separateHardSoftStash && isHardCoreGame != 0? "_LOD_HC_" : "_LOD_", sharedStashFilename)
		 && formatPath(szBackupName, MAX_PATH, "%s%s%s.sss", backupPath, separateHardSoftStash && isHardCoreGame != 0? "_LOD_HC_" : "_LOD_", sharedStashFilename))
			CopyFile(szSaveName, szBackupName, true);
	}

	// delete older backup
	if (maxBackupPerCharacter > 0)
	{
		char szSearch[MAX_PATH];
		if (!formatPath(szSearch, MAX_PATH, "%sbackup\\%s_*", savePath, name))
			return;
		WIN32_FIND_DATA FindFileData;

		// Count nb backup folders
		int nbFolders = 0;
		HANDLE hFind = FindFirstFile(szSearch, &FindFileData);
		if (hFind != INVALID_HANDLE_VALUE)
		{
			do
				nbFolders += isBackupFolder(&FindFileData, name);
			while (FindNextFile(hFind, &FindFileData));
			FindClose(hFind);
		}

		// delete nbFolders - maxfolder
		log_msg("Backup nbFolders: %d\n", nbFolders);
		nbFolders -= maxBackupPerCharacter;
		if (nbFolders <= 0)
			return;

		// NTFS lists names in sorted order and the _YYYYMMDD_HHMMSS suffix sorts chronologically, so the oldest come first.
		hFind = FindFirstFile(szSearch, &FindFileData);
		if (hFind == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (isBackupFolder(&FindFileData, name)
			 && formatPath(backupPath, MAX_PATH, "%sbackup\\%s\\", savePath, FindFileData.cFileName))
			{
				log_msg("Delete backup : %s\n", backupPath);

				for (int i = 0; i < nbBackupExtensions; i++)
					if (formatPath(szBackupName, MAX_PATH, "%s%s%s", backupPath, name, backupExtensions[i]))
						DeleteFile(szBackupName);
				if (formatPath(szBackupName, MAX_PATH, "%s_LOD_%s.sss", backupPath, sharedStashFilename))
					DeleteFile(szBackupName);
				if (formatPath(szBackupName, MAX_PATH, "%s_LOD_HC_%s.sss", backupPath, sharedStashFilename))
					DeleteFile(szBackupName);
				RemoveDirectory(backupPath);
				nbFolders--;
			}
		}
		while (nbFolders > 0 && FindNextFile(hFind, &FindFileData));
		FindClose(hFind);
	}
}

/*================================ CRASH-SAFE SAVES ================================*/
// 1.14d single player with ActiveCrashSafeSaves: every .d2s write goes through CharacterCommit_114 (the late hook)
// instead of vanilla's truncate in place. A save that changes no shared stash commits the .d2s and the .d2x together
// through the journal <name>.pyj (src/common/CharacterCommit.h); any other save replaces the .d2s whole, alone.

bool active_CrashSafeSaves = true;
// Only with the late hook installed may the entry hook leave the .d2s, and the .d2x of a journaled save, to it.
static bool lateHookInstalled = false;
// true: a leaving save that can't commit writes nothing, like a mid-game one. Only test builds set it.
static bool allOrNothingLeaveSave = false;

// readSaveFileData's window (x 50 ms) for a file another process is replacing.
#define SAVE_READ_RETRIES 100
#define LEAVING_LOCK_WAIT 2000
#define COMMIT_RETRY_INTERVAL 2000

#ifdef PLUGY_TEST_HOOKS
// The rig's kill matrix: PLUGY_KILL_AT=KP_START..KP_DONE ends the process right after that commit step.
static void killPoint(const char* point)
{
	char wanted[32];
	DWORD len = GetEnvironmentVariableA("PLUGY_KILL_AT", wanted, sizeof(wanted));
	if (!len || len >= sizeof(wanted) || _stricmp(wanted, point))
		return;
	log_msg("Test hook: killed at %s\n", point);
	TerminateProcess(GetCurrentProcess(), 9);
}

static const char* commitResultName(CommitResult result)
{
	switch (result)
	{
	case COMMIT_RESULT_PENDING: return "PENDING";
	case COMMIT_RESULT_ABORTED: return "ABORTED";
	case COMMIT_RESULT_COMMITTED_INCOMPLETE: return "COMMITTED_INCOMPLETE";
	case COMMIT_RESULT_DONE: return "DONE";
	}
	return "?";
}
#else
#define killPoint(point) ((void)0)
#endif

static CommitResult lastSaveState = COMMIT_RESULT_DONE;
static DWORD lastSaveSeq = 0;
// The seq of the last commit that passed its commit point; the next commit journals committedSeq + 1.
static DWORD committedSeq = 0;

static void setSaveResult(CommitResult state, DWORD seq)
{
	lastSaveState = state;
	lastSaveSeq = seq;
#ifdef PLUGY_TEST_HOOKS
	if (state != COMMIT_RESULT_PENDING)
		log_msg("Test hook: save result %s, seq %u\n", commitResultName(state), seq);
#endif
}

// What the entry hook prepared for the late hook of the same save.
enum PendingKind
{
	PENDING_NONE,		// no entry hook ran this save
	PENDING_PLAIN,		// the .d2s alone, replaced whole (the entry hook already wrote whatever else this save writes)
	PENDING_JOURNALED,	// the .d2s and d2xPost, committed together
};

struct PendingSave
{
	PendingKind kind;
	bool isLeaving;
	char name[COMMIT_NAME_SIZE];
	BYTE* d2xPost;	// malloc'd: it must not depend on the game's memory pool
	DWORD d2xPostSize;
	// PENDING_PLAIN: the entry hook wrote the .d2x or changed the shared stash file, so the character file is the one
	// still missing if the late hook can't write it.
	bool stashWritten;
	Game* game;	// NULL when no entry hook ran
};
static PendingSave pendingSave;

static void dropPendingSave()
{
	free(pendingSave.d2xPost);
	ZeroMemory(&pendingSave, sizeof(pendingSave));
}

// D2SaveSPChar reaches its end without the late hook (SPCharSaveEnabled cleared, an empty serializer result), so a save
// still PENDING once the game moved on saved nothing.
static void resolveStalePending()
{
	dropPendingSave();
	setSaveResult(COMMIT_RESULT_ABORTED, lastSaveSeq);
}

// A commit past its commit point whose replaces are not all done yet: retried by the tick.
static bool retryPending = false;
static char retryName[COMMIT_NAME_SIZE] = "";
static CommitTxn retryTxn;
static DWORD lastRetryTick = 0;

// One count per game, bumped by the tick when the Game* changes or its frame counter restarts: a new game can reuse the
// address of the one before. What belongs to one game (the character it plays, its leaving-save box) carries it.
static DWORD gameGeneration = 1;
static Game* tickGame = NULL;
static DWORD tickFrame = 0;

// The character the current game loaded or saved: recovery never archives the files of a character played here, and a
// character of the same name made later in this process (SoloPlay deletes and remakes characters) is a new one.
static char playedName[COMMIT_NAME_SIZE] = "";
static DWORD playedGeneration = 0;
// A load or a first save can run before the tick first sees its game: that tick moves the mark to the new count.
static Game* playedLoadGame = NULL;

// A box stalls the game: a save shows one at most per game, and only at the leaving save.
static DWORD leavingBoxGeneration = 0;

static bool firstLeavingBoxThisGame()
{
	if (leavingBoxGeneration == gameGeneration)
		return false;
	leavingBoxGeneration = gameGeneration;
	return true;
}

static void copyName(char* out, const char* name)
{
	strncpy(out, name, COMMIT_NAME_SIZE - 1);
	out[COMMIT_NAME_SIZE - 1] = NULL;
}

// A game the tick has not reached yet is a new one, even at a reused address: its frame counter restarted.
static bool tickSawGame(Game* game)
{
	return game == tickGame && game->gameFrame >= tickFrame;
}

// game NULL (no entry hook ran): the mark belongs to the game the tick last saw.
static void markPlayed(const char* name, Game* game)
{
	copyName(playedName, name);
	playedGeneration = gameGeneration;
	playedLoadGame = game && !tickSawGame(game) ? game : NULL;
}

static bool isPlayedHere(const char* name, Game* game)
{
	if (!playedName[0] || _stricmp(playedName, name))
		return false;
	if (game && !tickSawGame(game))
		return playedLoadGame == game;
	return playedGeneration == gameGeneration;
}

// Every server frame, from the tick.
static void noteGameFrame(Game* game)
{
	if (game != tickGame || game->gameFrame < tickFrame)
	{
		gameGeneration++;
		if (playedLoadGame == game)
			playedGeneration = gameGeneration;
	}
	playedLoadGame = NULL;
	tickGame = game;
	tickFrame = game->gameFrame;
}

// ABSENT only for ERROR_FILE_NOT_FOUND: a missing folder or any other error is UNREADABLE, with its code in *error. A
// 0-byte file is PRESENT with *data NULL (readSaveFileData reports it as missing). *data is malloc'd. retries: 50 ms
// waits on a file held elsewhere. maxRead: read at most that many bytes from the start (0: the whole file).
static CommitFileState readDiskFile(const char* path, BYTE** data, DWORD* size, int retries, DWORD* error, DWORD maxRead = 0)
{
	*data = NULL;
	*size = 0;
	*error = 0;
	HANDLE hFile;
	for (int retry = 0; ; retry++)
	{
		hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE)
			break;
		*error = GetLastError();
		if (*error == ERROR_FILE_NOT_FOUND)
		{
			*error = 0;
			return COMMIT_FILE_ABSENT;
		}
		if ((*error != ERROR_SHARING_VIOLATION && *error != ERROR_ACCESS_DENIED) || retry >= retries)
		{
			log_msg("Can't open %s (error %u)\n", path, *error);
			return COMMIT_FILE_UNREADABLE;
		}
		Sleep(50);
	}
	// An open that succeeds on a retry leaves the failed attempt's error behind; the reads below test *error.
	*error = 0;
	DWORD sizeHigh = 0;
	DWORD fileSize = GetFileSize(hFile, &sizeHigh);
	if (fileSize == INVALID_FILE_SIZE)
		*error = GetLastError() ? GetLastError() : ERROR_FILE_TOO_LARGE;
	else if (maxRead && (sizeHigh || fileSize > maxRead))
	{
		fileSize = maxRead;
		sizeHigh = 0;
	}
	// Larger files are refused as unreadable rather than allocated.
	else if (sizeHigh || fileSize > COMMIT_MAX_FILE_SIZE)
		*error = ERROR_FILE_TOO_LARGE;
	BYTE* buffer = !*error && fileSize ? (BYTE*)malloc(fileSize) : NULL;
	if (!*error && fileSize && !buffer)
		*error = ERROR_NOT_ENOUGH_MEMORY;
	DWORD total = 0;
	while (!*error && total < fileSize)
	{
		DWORD nbRead = 0;
		if (!ReadFile(hFile, buffer + total, fileSize - total, &nbRead, NULL))
			*error = GetLastError() ? GetLastError() : ERROR_READ_FAULT;
		else if (!nbRead)
			*error = ERROR_HANDLE_EOF;
		total += nbRead;
	}
	CloseHandle(hFile);
	if (*error)
	{
		log_msg("Can't read %s (error %u)\n", path, *error);
		free(buffer);
		return COMMIT_FILE_UNREADABLE;
	}
	*data = buffer;
	*size = fileSize;
	return COMMIT_FILE_PRESENT;
}

static CommitDiskFile diskFile(CommitFileState state, const BYTE* data, DWORD size)
{
	if (state == COMMIT_FILE_PRESENT)
		return CommitFilePresent(data, size);
	return state == COMMIT_FILE_ABSENT ? CommitFileAbsent() : CommitFileUnreadable();
}

struct CommitPaths
{
	char d2s[MAX_PATH];
	char d2x[MAX_PATH];
	char pyj[MAX_PATH];
};

static bool getCommitPaths(const char* name, CommitPaths* paths)
{
	return getSaveFilePath(paths->d2s, MAX_PATH, name, ".d2s")
		&& getSaveFilePath(paths->d2x, MAX_PATH, name, ".d2x")
		&& getSaveFilePath(paths->pyj, MAX_PATH, name, ".pyj");
}

static bool deleteIfPresent(const char* path)
{
	return DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

static bool archiveFile(const char* path, const char* suffix)
{
	char archive[MAX_PATH];
	// The caller logs GetLastError, so the failures before MoveFileEx set their own.
	if (!suffix[0])
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	if (!formatPath(archive, MAX_PATH, "%s%s", path, suffix))
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return false;
	}
	if (!MoveFileExA(path, archive, MOVEFILE_WRITE_THROUGH))
		return false;
	log_msg("%s archived as %s\n", path, archive);
	return true;
}

// One action of a recovery plan; failedFile receives the file it could not change.
static bool runRecoveryAction(CommitAction action, const CommitPaths* paths, const CommitJournal* journal,
	const char* suffix, bool retryWrites, char* failedFile)
{
	const char* target = paths->pyj;
	bool ok = false;
	switch (action)
	{
	case COMMIT_ACTION_WRITE_D2S_POST:
		target = paths->d2s;
		ok = writeFileAtomic(target, journal->d2s.postBytes, journal->d2s.postSize, retryWrites);
		break;
	case COMMIT_ACTION_WRITE_D2X_POST:
		target = paths->d2x;
		ok = writeFileAtomic(target, journal->d2x.postBytes, journal->d2x.postSize, retryWrites);
		break;
	case COMMIT_ACTION_WRITE_D2X_PRE:
		target = paths->d2x;
		ok = writeFileAtomic(target, journal->d2x.preBytes, journal->d2x.preSize, retryWrites);
		break;
	case COMMIT_ACTION_DELETE_D2X:
		target = paths->d2x;
		ok = deleteIfPresent(target);
		break;
	case COMMIT_ACTION_ARCHIVE_D2X:
		target = paths->d2x;
		ok = archiveFile(target, suffix);
		break;
	case COMMIT_ACTION_ARCHIVE_JOURNAL:
		ok = archiveFile(target, suffix);
		break;
	case COMMIT_ACTION_DELETE_JOURNAL:
		ok = deleteIfPresent(target);
		break;
	}
	if (!ok)
	{
		log_msg("Crash-safe save: recovery action %d on %s failed (error %u)\n", action, target, GetLastError());
		strncpy(failedFile, target, MAX_PATH - 1);
		failedFile[MAX_PATH - 1] = NULL;
	}
	return ok;
}

// Reads the journal and the files its verdict needs, decides with the verdict table, and runs the plan in order up to
// its first failed action. plan and actionsDone feed CommitTxnRecovered. readRetries and retryWrites are off for the
// tick's retries, which must never stall a frame.
static bool runRecovery(const char* name, bool loadedHere, int readRetries, bool retryWrites, CharacterRecovery* out,
	CommitRecovery* plan, int* actionsDone)
{
	ZeroMemory(out, sizeof(*out));
	*actionsDone = 0;
	CommitDiskFile none = CommitFileAbsent();
	CommitPaths paths;
	if (!getCommitPaths(name, &paths))
	{
		// Without a path no journal can be looked at: refused like an unreadable one.
		CommitDecideRecovery(COMMIT_FILE_UNREADABLE, NULL, 0, loadedHere, &none, &none, plan);
		out->verdict = plan->verdict;
		out->readError = ERROR_FILENAME_EXCED_RANGE;
		strcpy(out->file, "(a save path that is too long)");
		return false;
	}

	// The journal buffer outlives the actions: WRITE actions take their bytes from it.
	BYTE* journal = NULL;
	DWORD journalSize = 0;
	DWORD error;
	CommitFileState journalState = readDiskFile(paths.pyj, &journal, &journalSize, readRetries, &error);
	const char* unreadable = journalState == COMMIT_FILE_UNREADABLE ? paths.pyj : NULL;
	DWORD unreadableError = error;
	CommitDiskFile d2sFile = none;
	CommitDiskFile d2xFile = none;
	if (journalState != COMMIT_FILE_UNREADABLE)
	{
		BYTE* data = NULL;
		DWORD size = 0;
		CommitFileState state;
		if (journalState == COMMIT_FILE_PRESENT)
		{
			state = readDiskFile(paths.d2s, &data, &size, readRetries, &error);
			d2sFile = diskFile(state, data, size);
			free(data);
			if (state == COMMIT_FILE_UNREADABLE)
			{
				unreadable = paths.d2s;
				unreadableError = error;
			}
		}
		// Without a journal only the .d2x fence counts, and it lies in the first 6 bytes: the size and hash taken from
		// them are never looked at.
		state = readDiskFile(paths.d2x, &data, &size, readRetries, &error,
			journalState == COMMIT_FILE_PRESENT ? 0 : COMMIT_FENCE_STUB_SIZE);
		d2xFile = diskFile(state, data, size);
		free(data);
		if (state == COMMIT_FILE_UNREADABLE && !unreadable)
		{
			unreadable = paths.d2x;
			unreadableError = error;
		}
	}
	CommitDecideRecovery(journalState, journal, journalSize, loadedHere, &d2sFile, &d2xFile, plan);
	if (plan->verdict == COMMIT_VERDICT_READ_ERROR && unreadable)
	{
		strcpy(out->file, unreadable);
		out->readError = unreadableError;
	}
	else if (plan->verdict == COMMIT_VERDICT_SKIP)
		strcpy(out->file, paths.d2s);
	out->verdict = plan->verdict;
	out->cause = plan->cause;
	out->parse = plan->parse;
	out->d2xFenced = d2xFile.state == COMMIT_FILE_PRESENT && d2xFile.fenced;
	if (plan->verdict != COMMIT_VERDICT_NONE)
		log_msg("Crash-safe save: journal of %s: %s (%s, seq %u, %d action(s))\n", name, CommitVerdictName(plan->verdict),
			plan->parse != COMMIT_PARSE_OK ? CommitParseStatusName(plan->parse) : plan->committed ? "committed" : "prepared",
			plan->journal.seq, plan->actionCount);

	char suffix[32];
	SYSTEMTIME now;
	GetLocalTime(&now);
	if (!CommitOrphanSuffix(&now, suffix, sizeof(suffix)))
		suffix[0] = NULL;
	bool ok = true;
	for (int i = 0; ok && i < plan->actionCount; i++)
	{
		ok = runRecoveryAction(plan->actions[i], &paths, &plan->journal, suffix, retryWrites, out->file);
		if (ok)
			(*actionsDone)++;
	}
	free(journal);
	out->actionFailed = !ok;
	if (plan->verdict == COMMIT_VERDICT_QUARANTINE)
		strcpy(out->file, plan->cause == COMMIT_QUARANTINE_FENCE_WITHOUT_JOURNAL ? paths.d2x : paths.pyj);
	return ok && CommitVerdictProceeds(plan->verdict);
}

bool recoverCharacterFiles(const char* name, bool loadedHere, CharacterRecovery* out)
{
	CommitRecovery plan;
	int actionsDone;
	if (!runRecovery(name, loadedHere, SAVE_READ_RETRIES, true, out, &plan, &actionsDone))
		return false;
	// This character's journal is resolved: a retry of it has nothing left to do.
	if (retryPending && !_stricmp(retryName, name))
		retryPending = false;
	return true;
}

void showRecoveryRefusal(const char* name, const CharacterRecovery* recovery, bool atLoad)
{
	const char* refused = atLoad ? "can't load" : "could not save";
	const char* outcome = atLoad ? "The character was not loaded." : "This game's progress since its last save was not saved.";
	if (recovery->verdict == COMMIT_VERDICT_READ_ERROR && !recovery->actionFailed)
		log_box("PlugY %s %s: %s can't be read (error %u).\n%s Close the program that holds that file, or fix its permissions%s.",
			refused, name, recovery->file, recovery->readError, outcome, atLoad ? ", then load the character again" : "");
	else if (recovery->verdict == COMMIT_VERDICT_SKIP)
		log_box("PlugY %s %s: its character file %s is no longer in the save folder.\n%s",
			refused, name, recovery->file, outcome);
	else if (recovery->actionFailed)
		log_box("PlugY %s %s: finishing its interrupted save needs to change %s, which failed (read-only, locked by another program, or disk full).\n%s Fix that file, then load the character again.",
			refused, name, recovery->file, outcome);
	else if (recovery->cause == COMMIT_QUARANTINE_FENCE_WITHOUT_JOURNAL)
		log_box("PlugY %s %s: its stash file %s carries a save mark, but no save journal is left to finish that save.\n%s Restore the stash file from a backup, or remove the mark as the readme says: the file still holds the stash of the character's last finished save.",
			refused, name, recovery->file, outcome);
	else if (recovery->d2xFenced)
	{
		// Deleting the journal alone would leave its fenced .d2x behind, which refuses the character in turn.
		char d2xPath[MAX_PATH];
		if (!getSaveFilePath(d2xPath, MAX_PATH, name, ".d2x"))
			formatPath(d2xPath, MAX_PATH, "%s.d2x", name);
		if (recovery->cause == COMMIT_QUARANTINE_PARSE)
			log_box("PlugY %s %s: its save journal %s can't be read back (%s).\n%s Delete that file only if you accept losing the save it holds. Its stash file %s carries a save mark, so remove that mark too (see the readme), or the character stays refused.",
				refused, name, recovery->file, CommitParseStatusName(recovery->parse), outcome, d2xPath);
		else
			log_box("PlugY %s %s: the character files match neither side of the save in its journal %s.\n%s Restore the character from a backup, or delete that file only if you accept losing the save it holds. Its stash file %s carries a save mark, so remove that mark too (see the readme), or the character stays refused.",
				refused, name, recovery->file, outcome, d2xPath);
	}
	else if (recovery->cause == COMMIT_QUARANTINE_PARSE)
		log_box("PlugY %s %s: its save journal %s can't be read back (%s).\n%s Delete that file only if you accept losing the save it holds.",
			refused, name, recovery->file, CommitParseStatusName(recovery->parse), outcome);
	else
		log_box("PlugY %s %s: the character files match neither side of the save in its journal %s.\n%s Restore the character from a backup, or delete that file only if you accept losing the save it holds.",
			refused, name, recovery->file, outcome);
}

static HANDLE sessionLock = INVALID_HANDLE_VALUE;
static char sessionLockName[COMMIT_NAME_SIZE] = "";
static PlayerData* sessionLockOwner = NULL;

static bool holdsSessionLock(const char* name)
{
	return sessionLock != INVALID_HANDLE_VALUE && !_stricmp(sessionLockName, name);
}

static void releaseSessionLock()
{
	if (sessionLock == INVALID_HANDLE_VALUE)
		return;
	CloseHandle(sessionLock);
	log_msg("Session lock of %s released\n", sessionLockName);
	sessionLock = INVALID_HANDLE_VALUE;
	sessionLockName[0] = NULL;
	sessionLockOwner = NULL;
}

// Opens <name>.pylive as a lock without touching the held one. True: *handle is the open lock, or INVALID_HANDLE_VALUE
// when the character goes on without one (see below). False: another game holds it, or it can't be opened.
static bool openSessionLockFile(const char* name, DWORD waitMs, HANDLE* handle, bool* waited, DWORD* error)
{
	*handle = INVALID_HANDLE_VALUE;
	*waited = false;
	*error = 0;
	char path[MAX_PATH];
	if (!getSaveFilePath(path, MAX_PATH, name, ".pylive"))
	{
		*error = ERROR_FILENAME_EXCED_RANGE;
		return false;
	}
	DWORD start = GetTickCount();
	bool recheckedMissing = false;
	for (;;)
	{
		// Share mode 0 makes the lock; reading is enough to hold it, so a .pylive frozen read-only with the saves opens.
		HANDLE hLock = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hLock != INVALID_HANDLE_VALUE)
		{
			*handle = hLock;
			*error = 0;
			return true;
		}
		*error = GetLastError();
		// Access denied on a read-only file, or with no file at all (a save folder that refuses new files), is a
		// permission no other game holds: the character goes on without the lock rather than staying refused for good.
		if (*error == ERROR_ACCESS_DENIED)
		{
			DWORD attributes = GetFileAttributesA(path);
			bool missing = attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
			// A file pending deletion denies access too and can be gone by now: one more open tells it apart.
			if (missing && !recheckedMissing)
			{
				recheckedMissing = true;
				continue;
			}
			if (missing || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)))
			{
				log_msg("Can't %s the session lock %s (error %u): %s goes on without it\n", missing ? "create" : "open the read-only",
					path, *error, name);
				*error = 0;
				return true;
			}
		}
		// Another game holds the file (sharing violation) or just closed it while it is pending deletion (access denied).
		if ((*error != ERROR_SHARING_VIOLATION && *error != ERROR_ACCESS_DENIED) || GetTickCount() - start >= waitMs)
		{
			log_msg("Can't take the session lock %s (error %u)\n", path, *error);
			return false;
		}
		*waited = true;
		Sleep(50);
	}
}

// Makes hLock (INVALID_HANDLE_VALUE: none) the one session lock, under name.
static void adoptSessionLock(const char* name, HANDLE hLock, bool waited)
{
	releaseSessionLock();
	if (hLock == INVALID_HANDLE_VALUE)
		return;
	sessionLock = hLock;
	copyName(sessionLockName, name);
	log_msg("Session lock of %s taken%s\n", name, waited ? " once another game released it" : "");
}

bool acquireSessionLock(const char* name, DWORD waitMs, bool* waited, DWORD* error)
{
	*waited = false;
	*error = 0;
	if (holdsSessionLock(name))
		return true;
	HANDLE hLock;
	if (!openSessionLockFile(name, waitMs, &hLock, waited, error))
		return false;
	adoptSessionLock(name, hLock, *waited);
	return true;
}

void releaseSessionLockUnlessNamed(const char* name)
{
	if (sessionLock != INVALID_HANDLE_VALUE && _stricmp(sessionLockName, name))
		releaseSessionLock();
}

void releaseSessionLockNamed(const char* name)
{
	if (holdsSessionLock(name))
		releaseSessionLock();
}

void setSessionLockOwner(PlayerData* playerData)
{
	sessionLockOwner = playerData;
}

// The game that played the character is over, even before the tick sees the next one.
void releaseSessionLockOf(PlayerData* playerData)
{
	if (!playerData || playerData != sessionLockOwner)
		return;
	playedName[0] = NULL;
	releaseSessionLock();
	sessionLockOwner = NULL;
}

void markCharacterLoaded(const char* name, Game* ptGame)
{
	markPlayed(name, ptGame);
	// The load's own recovery resolved this character's journal; another character's waits for its own next load.
	retryPending = false;
	// A new game: no result or seq of the previous one may be read as this game's.
	lastSaveState = COMMIT_RESULT_DONE;
	lastSaveSeq = 0;
}

// The new name's lock, taken by characterRenameAllowed before any file moves: a rename whose saves could never take
// their lock is refused instead. INVALID_HANDLE_VALUE with renameLockReady: the new name goes on without a lock.
static HANDLE renameLock = INVALID_HANDLE_VALUE;
static bool renameLockReady = false;

void cancelCharacterRename()
{
	if (renameLock != INVALID_HANDLE_VALUE)
		CloseHandle(renameLock);
	renameLock = INVALID_HANDLE_VALUE;
	renameLockReady = false;
}

// An unfinished save always leaves its .pyj on disk (a retry runs only while the journal is believed there), so the
// journal files are the test, with the new name's lock.
bool characterRenameAllowed(const char* oldName, const char* newName)
{
	cancelCharacterRename();
	const char* names[2] = {oldName, newName};
	for (int i = 0; i < 2; i++)
	{
		char path[MAX_PATH];
		if (!getSaveFilePath(path, MAX_PATH, names[i], ".pyj"))
			return false;
		if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
		{
			log_msg("Rename Character refused : %s exists or can't be checked\n", path);
			return false;
		}
	}
	// A rename that only changes case keeps the lock this process already holds on that file.
	bool waited;
	DWORD error;
	if (!holdsSessionLock(newName) && !openSessionLockFile(newName, 0, &renameLock, &waited, &error))
	{
		log_msg("Rename Character refused : the lock of %s is held by another game or can't be opened (error %u)\n", newName, error);
		return false;
	}
	renameLockReady = true;
	return true;
}

void renameCharacterCommitFiles(const char* oldName, const char* newName)
{
	char oldPath[MAX_PATH];
	char newPath[MAX_PATH];
	if (getSaveFilePath(oldPath, MAX_PATH, oldName, ".pyj") && getSaveFilePath(newPath, MAX_PATH, newName, ".pyj")
	 && MoveFileA(oldPath, newPath))
		log_msg("%s moved to %s\n", oldPath, newPath);

	bool heldOld = holdsSessionLock(oldName);
	// The load sets the owner even when the character runs without a lock, so it is kept either way.
	PlayerData* owner = sessionLockOwner;
	if (holdsSessionLock(newName))
		copyName(sessionLockName, newName);
	else
	{
		if (heldOld)
			releaseSessionLock();
		if (getSaveFilePath(oldPath, MAX_PATH, oldName, ".pylive"))
			DeleteFileA(oldPath);
		if (renameLockReady)
			adoptSessionLock(newName, renameLock, false);
		else
		{
			bool waited;
			DWORD error;
			acquireSessionLock(newName, 0, &waited, &error);
		}
		renameLock = INVALID_HANDLE_VALUE;
		sessionLockOwner = owner;
	}
	renameLockReady = false;
	if (isPlayedHere(oldName, NULL))
		copyName(playedName, newName);
}

// Step 0, before this save reads or writes any file: the session lock, then the character's journal resolved by the
// verdict table. The lock comes first so a journal is only ever resolved by the game that holds the character. False
// skips the save: another game holds the character, or a journal stays open. game: NULL when no entry hook ran.
static bool commitStepZero(const char* name, bool isLeaving, PlayerData* owner, Game* game)
{
	if (!holdsSessionLock(name))
	{
		bool waited;
		DWORD error;
		// A mid-game save never stalls on the lock; the leaving save matters most and waits a moment.
		if (!acquireSessionLock(name, isLeaving ? LEAVING_LOCK_WAIT : 0, &waited, &error))
		{
			log_msg("Save of %s skipped: another game holds the character\n", name);
			if (isLeaving && firstLeavingBoxThisGame())
				log_box("PlugY could not save %s: another game is running this character (or its lock file can't be opened, error %u).\nThis game's progress since its last save was not saved.", name, error);
			return false;
		}
		sessionLockOwner = owner;
	}
	CharacterRecovery recovery;
	if (!recoverCharacterFiles(name, isPlayedHere(name, game), &recovery))
	{
		log_msg("Save of %s skipped: its save journal stays open (%s)\n", name, CommitVerdictName(recovery.verdict));
		if (isLeaving && firstLeavingBoxThisGame())
			showRecoveryRefusal(name, &recovery, false);
		return false;
	}
	killPoint("KP_START");
	return true;
}

// The journaled path: the .d2x is only serialized here and the late hook commits it with the .d2s. The shared stash
// writes nothing this save, so no previous .d2x is kept to put back after a failed shared save.
static bool collectExtendedSaveFile(Unit* ptChar, bool isLeaving, PendingSave* collector)
{
	DWORD curSizeExt = 0;
	DWORD maxSizeExt = 0x4000;
	BYTE* dataExt = (BYTE *)D2AllocMem(PCGame->memoryPool, maxSizeExt,__FILE__,__LINE__,0);
	if (dataExt)
	{
		saveExtendedSaveFile(ptChar, &dataExt, &maxSizeExt, &curSizeExt);
		collector->d2xPost = curSizeExt ? (BYTE*)malloc(curSizeExt) : NULL;
		if (collector->d2xPost)
		{
			memcpy(collector->d2xPost, dataExt, curSizeExt);
			collector->d2xPostSize = curSizeExt;
		}
		D2FreeMem(PCGame->memoryPool, dataExt,__FILE__,__LINE__,0);
	}
	if (!collector->d2xPost)
	{
		log_msg("Error : Memory allocation Extended SaveFile, extended save file not saved\n");
		if (!isLeaving)
		{
			log_msg("Stash not saved: the character and the shared stash wait for the next save too.\n\n");
			return false;
		}
		// The leaving save lets the .d2s through without the stash, as the stash writer always did.
		log_msg("End saving.\n\n");
		return true;
	}
	if (active_sharedStash && PCPY->sharedStashIsOpened)
		log_msg("shared stash unchanged since load; not rewritten\n");
	collector->kind = PENDING_JOURNALED;
	log_msg("End saving: the stash is committed with the character.\n\n");
	return true;
}

// isLeaving: the save made as the player leaves the game. The other saves report failures to the log only, since a box
// would stall a bot mid-game, and are all or nothing: false tells the game to skip its .d2s too, so a game stopped at
// any moment reloads a character, stash and shared stash that match. The leaving save always lets the .d2s through.
// collector: the 1.14d entry hook with the late hook installed. A save whose shared stash writes nothing then leaves its
// .d2x to the late hook's commit; any other runs as without it, and the late hook replaces the .d2s whole.
static bool saveSPPlayerFiles(Unit* ptChar, bool isLeaving, PendingSave* collector)
{
	if (!D2isLODGame()) return true;//D2Game but in SP so np

	log_msg("\n--- Start SaveSPPlayerCustomData (leaving:%d) ---\n", isLeaving);

	NetClient* ptClient = D2GetClient(ptChar,__FILE__,__LINE__);
	// One backup per game, at its first save: it holds the files as the game loaded them, and autosaves don't rotate
	// older games' backups away. PYPlayerData exists only with the multi-page stash, so backup-only installs keep
	// backing up when leaving.
	if (active_PlayerCustomData ? !PCPY->backupDoneThisGame : isLeaving)
	{
		backupSaveFiles(PCPlayerData->name, ptClient->isHardCoreGame);
		if (active_PlayerCustomData)
			PCPY->backupDoneThisGame = 1;
	}

	if (active_PlayerCustomData)
	{
		bool sharedOpened = active_sharedStash && PCPY->sharedStashIsOpened;
#ifdef PLUGY_TEST_HOOKS
		if (collector && sharedOpened)
			checkSharedSaveParity(ptChar, ptClient->isHardCoreGame != 0);
#endif
		if (collector && PCPY->selfStashIsOpened && (!sharedOpened || sharedSaveIsUnchanged(ptChar, ptClient->isHardCoreGame != 0)))
			return collectExtendedSaveFile(ptChar, isLeaving, collector);

		// Mid-game the .d2x is saved first, keeping a copy of the previous one to put back when the shared stash, which
		// another game can hold, can't be saved after it: the skipped save then leaves every file as it was.
		BYTE* previousExt = NULL;
		DWORD previousExtSize = 0;
		int previousExtState = 0;
		if (!isLeaving && active_sharedStash && PCPY->sharedStashIsOpened && PCPY->selfStashIsOpened)
		{
			previousExtState = readExtendedSaveFileCopy(PCPlayerData->name, &previousExt, &previousExtSize);
			if (previousExtState < 0)
			{
				log_msg("Stash file not readable: the character and the shared stash wait for the next save too.\n\n");
				return false;
			}
		}

		bool extendedSaved = false;
		if (PCPY->selfStashIsOpened)
		{
			DWORD curSizeExt = 0;
			DWORD maxSizeExt = 0x4000;
			BYTE* dataExt = (BYTE *)D2AllocMem(PCGame->memoryPool, maxSizeExt,__FILE__,__LINE__,0);
			if (!dataExt)
				log_msg("Error : Memory allocation Extended SaveFile, extended save file not saved\n");
			else
			{
				saveExtendedSaveFile(ptChar, &dataExt, &maxSizeExt, &curSizeExt);
				extendedSaved = writeExtendedSaveFile(PCPlayerData->name, dataExt, curSizeExt, isLeaving);
				D2FreeMem(PCGame->memoryPool, dataExt,__FILE__,__LINE__,0);
			}
			if (!extendedSaved && !isLeaving)
			{
				if (previousExt)
					D2FogMemDeAlloc(previousExt,__FILE__,__LINE__,0);
				log_msg("Stash not saved: the character and the shared stash wait for the next save too.\n\n");
				return false;
			}
		}
		if (collector && extendedSaved)
			collector->stashWritten = true;

		bool skipSave = false;
		if (active_sharedStash && PCPY->sharedStashIsOpened)
		{
			bool sharedSaved = false;
			// The shared writer records the file's hash only when it replaces the file; its other true returns write nothing.
			DWORD sharedDiskHash = PCPY->sharedDiskHash;
			DWORD sharedDiskExisted = PCPY->sharedDiskExisted;
			DWORD curSizeShr = 0;
			DWORD maxSizeShr = 0x4000;
			BYTE* dataShr = (BYTE *)D2AllocMem(PCGame->memoryPool, maxSizeShr,__FILE__,__LINE__,0);
			if (!dataShr)
				log_msg("Error : Memory allocation Shared SaveFile, shared stash not saved\n");
			else
			{
				saveSharedSaveFile(ptChar, &dataShr, &maxSizeShr, &curSizeShr);
				sharedSaved = writeSharedSaveFileIfChanged(ptChar, dataShr, curSizeShr, ptClient->isHardCoreGame != 0, isLeaving);
				D2FreeMem(PCGame->memoryPool, dataShr,__FILE__,__LINE__,0);
			}
			skipSave = !sharedSaved && !isLeaving;
			if (collector && (PCPY->sharedDiskHash != sharedDiskHash || PCPY->sharedDiskExisted != sharedDiskExisted))
				collector->stashWritten = true;
			// A .d2x that can't be put back stays, and the character is saved with it: only moves to or from the shared
			// stash since the last save can then differ, where skipping would also undo moves between it and the inventory.
			if (skipSave && extendedSaved && !restoreExtendedSaveFile(PCPlayerData->name, previousExtState, previousExt, previousExtSize))
			{
				log_msg("Shared stash not saved and the stash file can't be put back: the character is saved with the new stash.\n");
				skipSave = false;
			}
		}
		if (previousExt)
			D2FogMemDeAlloc(previousExt,__FILE__,__LINE__,0);
		if (skipSave)
		{
			log_msg("Shared stash not saved: the character and its stash wait for the next save too.\n\n");
			return false;
		}
	}
	log_msg("End saving.\n\n");
	return true;
}

// Also the TCP/IP host's fallback in SaveMPPlayerCustomData: no collector, so the .d2x is written at once.
void STDCALL SaveSPPlayerCustomData(Unit* ptChar)
{
	saveSPPlayerFiles(ptChar, true, NULL);
}

// 1.14d calls D2SaveSPChar for every .d2s save: the autosave every 8192 frames (about 5.5 min), death, /save, leaving
// the game and closing it. Loading takes every stash page from the .d2x, so each .d2s write needs the PlugY files
// written with it. Returns 0 when the game must skip this .d2s write.
DWORD STDCALL SaveSPPlayerCustomData_114(Unit* ptChar, DWORD isLeaving)
{
	// Once the game clears this flag D2SaveSPChar writes no .d2s, and a newer stash would no longer match the character.
	if (!SPCharSaveEnabled)
	{
		log_msg("SaveSPPlayerCustomData : the game saves no character file now, so neither does PlugY\n");
		dropPendingSave();
		setSaveResult(COMMIT_RESULT_ABORTED, lastSaveSeq);
		return 1;
	}
	if (!lateHookInstalled)
		return saveSPPlayerFiles(ptChar, isLeaving != 0, NULL) ? 1 : 0;

	// Step 0 comes first on every path: no file of this save is read or written before a journal left open is resolved.
	dropPendingSave();
	if (!commitStepZero(PCPlayerData->name, isLeaving != 0, PCPlayerData, PCGame))
	{
		setSaveResult(COMMIT_RESULT_ABORTED, lastSaveSeq);
		return 0;
	}
	pendingSave.kind = PENDING_PLAIN;
	pendingSave.isLeaving = isLeaving != 0;
	pendingSave.game = PCGame;
	copyName(pendingSave.name, PCPlayerData->name);
	if (!saveSPPlayerFiles(ptChar, isLeaving != 0, &pendingSave))
	{
		dropPendingSave();
		setSaveResult(COMMIT_RESULT_ABORTED, lastSaveSeq);
		return 0;
	}
	setSaveResult(COMMIT_RESULT_PENDING, lastSaveSeq);
	return 1;
}

// A leaving save that ends with its character file unsaved loses the game's progress since the last save: the player is
// told, once per game. Mid-game saves only log, since a box would stall a bot and a later save comes anyway.
static void showLeavingSaveLost(const char* name, const char* file, const char* reason)
{
	if (firstLeavingBoxThisGame())
		log_box("PlugY could not save %s when leaving the game: %s %s.\nThis game's progress since its last save was not saved.", name, file, reason);
}

static bool isReadOnly(const char* path)
{
	DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY);
}

// A .d2s frozen read-only on purpose would box at the end of every game: once per character per process instead. The
// names are this box's own, so it and the .d2x writer's once-per-character box never silence each other; more frozen
// characters than slots in one process only repeat a box.
#define FROZEN_BOXED_NAMES 32
static char frozenBoxedNames[FROZEN_BOXED_NAMES][COMMIT_NAME_SIZE];
static int frozenBoxedNext = 0;

static bool frozenBoxShown(const char* name)
{
	for (int i = 0; i < FROZEN_BOXED_NAMES; i++)
		if (!_stricmp(frozenBoxedNames[i], name))
			return true;
	return false;
}

// stashWritten: this save wrote the stash files without the character file.
static void showLeavingSaveFrozen(const char* name, const char* d2sPath, bool stashWritten)
{
	if (frozenBoxShown(name) || !firstLeavingBoxThisGame())
		return;
	copyName(frozenBoxedNames[frozenBoxedNext], name);
	frozenBoxedNext = (frozenBoxedNext + 1) % FROZEN_BOXED_NAMES;
	if (stashWritten)
		log_box("PlugY did not save %s when leaving the game: its character file %s is read-only. Its stash files were saved, so the character and its stash may not match.\nClear the read-only flag to save this character again. PlugY says this once per character until Diablo II is closed.", name, d2sPath);
	else
		log_box("PlugY did not save %s when leaving the game: its character file %s is read-only, so neither it nor its stash file was saved.\nClear the read-only flag to save this character again. PlugY says this once per character until Diablo II is closed.", name, d2sPath);
}

// stashWritten: this save already wrote the .d2x or changed the shared stash file, so only the character file is missing.
// game: NULL when no entry hook ran.
static void writePlainSave(const char* name, const char* d2sPath, const BYTE* d2s, DWORD d2sSize, bool isLeaving,
	bool stashWritten, Game* game)
{
	if (writeFileAtomic(d2sPath, d2s, d2sSize))
	{
		markPlayed(name, game);
		setSaveResult(COMMIT_RESULT_DONE, committedSeq);
	}
	else
	{
		log_msg("Could not save %s\n", d2sPath);
		setSaveResult(COMMIT_RESULT_ABORTED, committedSeq);
		if (isLeaving && isReadOnly(d2sPath))
			showLeavingSaveFrozen(name, d2sPath, stashWritten);
		else if (isLeaving && !stashWritten)
			showLeavingSaveLost(name, d2sPath, "can't be written (read-only, locked by another program, or disk full)");
		else if (isLeaving && firstLeavingBoxThisGame())
			log_box("PlugY could not save %s when leaving the game: its character file %s can't be written (read-only, locked by another program, or disk full).\nIts stash files were saved, so the character and its stash may not match until its next successful save.", name, d2sPath);
	}
}

// The stash writer's order without a journal: the .d2x, then the .d2s, each replaced whole. For a leaving save whose
// commit could not run, so a leaving player loses nothing when the journal machinery itself fails, and for a .d2s path
// recovery would not find.
static void writeStashThenCharacter(char* name, const char* d2sPath, const BYTE* d2s, DWORD d2sSize, bool isLeaving)
{
	log_msg("Crash-safe save of %s not possible: the stash file then the character are saved one after the other\n", name);
	bool extendedSaved = writeExtendedSaveFile(name, pendingSave.d2xPost, pendingSave.d2xPostSize, isLeaving);
	if (!extendedSaved && !isLeaving)
	{
		log_msg("Stash not saved: the character waits for the next save too.\n\n");
		setSaveResult(COMMIT_RESULT_ABORTED, committedSeq);
		return;
	}
	writePlainSave(name, d2sPath, d2s, d2sSize, isLeaving, extendedSaved, pendingSave.game);
}

// Steps 2 to 7, from txn->next on. A failure at or before the commit point resolves the journal from the disk at
// once (a replace reported as failed may have landed), into *recovery; one after it leaves the rest to recovery.
static void runCommitSteps(CommitTxn* txn, const char* name, const CommitPaths* paths, BYTE* journal,
	DWORD journalSize, const BYTE* fenced, DWORD fencedSize, const BYTE* d2s, DWORD d2sSize, CharacterRecovery* recovery)
{
	while (txn->next != COMMIT_STEP_END)
	{
		const char* point;
		bool ok;
		switch (txn->next)
		{
		case COMMIT_STEP_PREPARE:
			ok = writeFileAtomic(paths->pyj, journal, journalSize);
			point = "KP_PREPARED";
			break;
		case COMMIT_STEP_FENCE:
			ok = writeFileAtomic(paths->d2x, fenced, fencedSize);
			point = "KP_FENCED";
			break;
		case COMMIT_STEP_COMMIT:
			ok = CommitJournalSetState(journal, journalSize, COMMIT_COMMITTED)
				&& writeFileAtomic(paths->pyj, journal, journalSize);
			point = "KP_COMMITTED";
			break;
		case COMMIT_STEP_D2S:
			ok = writeFileAtomic(paths->d2s, d2s, d2sSize);
			point = "KP_D2S";
			break;
		case COMMIT_STEP_D2X:
			ok = writeFileAtomic(paths->d2x, pendingSave.d2xPost, pendingSave.d2xPostSize);
			point = "KP_D2X";
			break;
		default:
			ok = deleteIfPresent(paths->pyj);
			point = "KP_DONE";
			break;
		}
		if (!ok)
		{
			log_msg("Crash-safe save of %s: step %d (%s) failed\n", name, txn->next + 2, CommitStepName(txn->next));
			if (CommitTxnStepFailed(txn) == COMMIT_RECOVER_NOW)
			{
				CommitRecovery plan;
				int actionsDone;
				runRecovery(name, true, SAVE_READ_RETRIES, true, recovery, &plan, &actionsDone);
				CommitTxnRecovered(txn, &plan, actionsDone, &committedSeq);
			}
			return;
		}
		CommitTxnStepDone(txn, &committedSeq);
		killPoint(point);
	}
}

// The journaled save: step 0 already ran in the entry hook.
static void commitJournaledSave(const char* d2sPath, const BYTE* d2s, DWORD d2sSize)
{
	char* name = pendingSave.name;
	CommitFailure failure = COMMIT_FAIL_OTHER;
	CommitTxn txn;
	CommitTxnBegin(&txn, committedSeq);
	BYTE* journal = NULL;
	BYTE* fenced = NULL;
	BYTE* d2xPre = NULL;
	DWORD journalSize = 0;
	DWORD fencedSize = 0;
	DWORD d2xPreSize = 0;
	bool prepared = false;
	bool d2sReadOnly = false;
	CommitPaths paths;
	if (!getCommitPaths(name, &paths))
		log_msg("Crash-safe save of %s: a save path is too long\n", name);
	else
	{
		// Step 1. A read-only .d2s was frozen by the user on purpose, and an unreadable one can't be journaled: the whole
		// save is refused, the stash included.
		BYTE* d2sPre = NULL;
		DWORD d2sPreSize = 0;
		DWORD error;
		CommitFileState d2sState = readDiskFile(paths.d2s, &d2sPre, &d2sPreSize, SAVE_READ_RETRIES, &error);
		CommitDiskFile d2sPreFile = diskFile(d2sState, d2sPre, d2sPreSize);
		free(d2sPre);
		d2sReadOnly = isReadOnly(paths.d2s);
		CommitFileState d2xState = readDiskFile(paths.d2x, &d2xPre, &d2xPreSize, SAVE_READ_RETRIES, &error);
		if (d2sReadOnly || d2sState == COMMIT_FILE_UNREADABLE)
		{
			failure = COMMIT_FAIL_D2S_PRECHECK;
			if (d2sReadOnly)
				log_msg("%s is read-only: neither it nor the stash file is saved\n", paths.d2s);
			else
				log_msg("%s can't be read: neither it nor the stash file is saved\n", paths.d2s);
		}
		else if (d2xState == COMMIT_FILE_UNREADABLE)
			log_msg("Stash file not readable: the character and its stash wait for the next save too.\n");
		else
		{
			CommitBuildInput in = {name, txn.seq, d2sPreFile, d2s, d2sSize, d2xPre, d2xPreSize,
				pendingSave.d2xPost, pendingSave.d2xPostSize, 0};
			journalSize = CommitJournalSize(&in);
			fencedSize = CommitFencedSize(d2xPre, d2xPreSize);
			journal = journalSize ? (BYTE*)malloc(journalSize) : NULL;
			fenced = (BYTE*)malloc(fencedSize);
			prepared = journal && fenced && CommitJournalEncode(&in, journal, journalSize)
				&& CommitFence(d2xPre, d2xPreSize, fenced, fencedSize);
			if (!prepared)
				log_msg("Crash-safe save of %s: its journal can't be built (.d2s %u bytes, .d2x %u then %u bytes, limit %u)\n",
					name, d2sSize, d2xPreSize, pendingSave.d2xPostSize, COMMIT_MAX_FILE_SIZE);
		}
	}
	CharacterRecovery recovery;
	ZeroMemory(&recovery, sizeof(recovery));
	if (prepared)
		runCommitSteps(&txn, name, &paths, journal, journalSize, fenced, fencedSize, d2s, d2sSize, &recovery);
	else
		txn.result = COMMIT_RESULT_ABORTED;
	free(journal);
	free(fenced);
	free(d2xPre);

	setSaveResult(txn.result, txn.seq);
	if (txn.result != COMMIT_RESULT_ABORTED)
		markPlayed(name, pendingSave.game);
	if (txn.result == COMMIT_RESULT_DONE)
		log_msg("Crash-safe save of %s done (seq %u)\n", name, txn.seq);
	if (txn.journalOnDisk)
	{
		// Past the commit point the replaces resume; before it a rollback whose restore failed finishes.
		log_msg("Crash-safe save of %s (seq %u) %s: its journal stays until a retry, the next save or the next load resolves it\n",
			name, txn.seq, txn.committed ? "committed but not finished on disk" : "not done");
		retryPending = true;
		copyName(retryName, name);
		retryTxn = txn;
		lastRetryTick = GetTickCount();
	}
	if (txn.result != COMMIT_RESULT_ABORTED)
		return;
	if (CommitLeaveFallbackAllowed(pendingSave.isLeaving, allOrNothingLeaveSave, txn.journalOnDisk, failure))
	{
		writeStashThenCharacter(name, d2sPath, d2s, d2sSize, true);
		return;
	}
	log_msg("Crash-safe save of %s not done: the files stay as the last save left them\n", name);
	if (!pendingSave.isLeaving || allOrNothingLeaveSave)
		return;
	if (failure == COMMIT_FAIL_D2S_PRECHECK && d2sReadOnly)
		showLeavingSaveFrozen(name, paths.d2s, false);
	else if (failure == COMMIT_FAIL_D2S_PRECHECK)
		showLeavingSaveLost(name, paths.d2s, "can't be read, so neither it nor the stash file was saved");
	else if (recovery.verdict != COMMIT_VERDICT_NONE || recovery.actionFailed)
	{
		if (firstLeavingBoxThisGame())
			showRecoveryRefusal(name, &recovery, false);
	}
	else
		showLeavingSaveLost(name, paths.pyj, "could not be resolved");
}

// Last component of the vanilla save path, without ".d2s".
static bool nameFromSavePath(const char* path, char* name)
{
	const char* start = path;
	for (const char* p = path; *p; p++)
		if (*p == '\\' || *p == '/')
			start = p + 1;
	size_t len = strlen(start);
	if (len <= 4 || len - 4 >= COMMIT_NAME_SIZE || _stricmp(start + len - 4, ".d2s"))
		return false;
	memcpy(name, start, len - 4);
	name[len - 4] = NULL;
	return true;
}

// Every .d2s write of D2SaveSPChar once the late hook is installed; vanilla's fopen "wb", fwrite and fclose never run.
// d2s/d2sSize: the serializer's buffer, the very bytes vanilla would write. ptChar: the player D2SaveSPChar saves.
void STDCALL CharacterCommit_114(const char* d2sPath, const BYTE* d2s, DWORD d2sSize, DWORD isLeaving, Unit* ptChar)
{
	if (pendingSave.kind == PENDING_NONE)
	{
		// No entry hook ran (Install_SavePlayerData is only installed with backups or the multi-page stash), so step 0
		// runs here, and with no .d2x part the .d2s alone is replaced.
		char name[COMMIT_NAME_SIZE];
		if (!nameFromSavePath(d2sPath, name))
		{
			log_msg("Crash-safe save: no character name in %s, nothing saved\n", d2sPath);
			setSaveResult(COMMIT_RESULT_ABORTED, lastSaveSeq);
			return;
		}
		// A lock this save takes, as a new character's first save does, ends with the saved player's PlayerData.
		PlayerData* owner = ptChar && ptChar->nUnitType == UNIT_PLAYER && PCPlayerData
			&& !_stricmp(PCPlayerData->name, name) ? PCPlayerData : NULL;
		if (!commitStepZero(name, isLeaving != 0, owner, NULL))
			setSaveResult(COMMIT_RESULT_ABORTED, lastSaveSeq);
		else
			writePlainSave(name, d2sPath, d2s, d2sSize, isLeaving != 0, false, NULL);
		return;
	}
	// Recovery finds the .d2s by name in the save folder, so a journal must name the file the game writes.
	char expected[MAX_PATH];
	bool samePath = getSaveFilePath(expected, MAX_PATH, pendingSave.name, ".d2s") && !_stricmp(expected, d2sPath);
	if (pendingSave.kind == PENDING_JOURNALED && samePath)
		commitJournaledSave(d2sPath, d2s, d2sSize);
	else if (pendingSave.kind == PENDING_JOURNALED)
	{
		log_msg("Crash-safe save: the game saves %s, not the file of %s, so nothing is journaled\n", d2sPath, pendingSave.name);
		writeStashThenCharacter(pendingSave.name, d2sPath, d2s, d2sSize, pendingSave.isLeaving);
	}
	else
		writePlainSave(pendingSave.name, d2sPath, d2s, d2sSize, pendingSave.isLeaving, pendingSave.stashWritten, pendingSave.game);
	dropPendingSave();
}

// Resolves a journal a save of this process left open, through recovery: past the commit point the replaces resume in
// order from the first unfinished one; before it, a rollback whose restore failed finishes. Never waits on a held file.
static void retryCommit()
{
	// Once this process released the character another game may play it: only the lock holder writes its files.
	if (!holdsSessionLock(retryName))
	{
		log_msg("Crash-safe save of %s: its next load finishes the save\n", retryName);
		retryPending = false;
		return;
	}
	CharacterRecovery recovery;
	CommitRecovery plan;
	int actionsDone;
	bool proceeds = runRecovery(retryName, true, 0, false, &recovery, &plan, &actionsDone);
	CommitTxnRecovered(&retryTxn, &plan, actionsDone, &committedSeq);
	if (lastSaveState == COMMIT_RESULT_COMMITTED_INCOMPLETE && lastSaveSeq == retryTxn.seq && retryTxn.result != lastSaveState)
		setSaveResult(retryTxn.result, retryTxn.seq);
	if (proceeds)
	{
		log_msg("Crash-safe save of %s: journal seq %u resolved (%s)\n", retryName, retryTxn.seq, CommitVerdictName(plan.verdict));
		retryPending = false;
	}
	// A quarantine needs the player, through the next load's box: retrying it every 2 s would only fill the log.
	else if (plan.verdict == COMMIT_VERDICT_QUARANTINE || plan.verdict == COMMIT_VERDICT_SKIP)
	{
		log_msg("Crash-safe save of %s stays unfinished: its next load reports it\n", retryName);
		retryPending = false;
	}
}

static bool realmDatabaseLogged = false;

// Every server frame: cheap while nothing is pending.
void STDCALL CharacterCommitTick(Game* ptGame)
{
	noteGameFrame(ptGame);
	if (lastSaveState == COMMIT_RESULT_PENDING)
		resolveStalePending();
	// Single player is assumed to have no realm database: with one, 0x532400 saves without D2SaveSPChar.
	if (!realmDatabaseLogged && *ptRealmDatabase)
	{
		realmDatabaseLogged = true;
		log_msg("!!! Realm database global 0x883D50 is set (%08X): saves bypass D2SaveSPChar\n", *ptRealmDatabase);
	}
	if (!retryPending)
		return;
	DWORD now = GetTickCount();
	if (now - lastRetryTick < COMMIT_RETRY_INTERVAL)
		return;
	lastRetryTick = now;
	retryCommit();
}

// D2SaveSPChar(ECX game, EDX player, EBX name; stack: interaction flag, leaving flag), so at entry [ESP+8] is the
// leaving flag, set only on the save made when the player leaves the game. A skipped save returns as D2SaveSPChar
// would, popping its two stack arguments.
FCT_ASM ( caller_SaveSPPlayerCustomData_114 )
	PUSH ECX
	PUSH EDX
	PUSH DWORD PTR SS:[ESP+0x10]
	PUSH EDX
	CALL SaveSPPlayerCustomData_114
	POP EDX
	POP ECX
	TEST EAX,EAX
	JE skipSave
	JMP D2SaveSPChar
skipSave:
	RETN 8
}}

// At 0x5322CE, after the serializer filled the buffer and its size (> 0: the JLE at 0x5322CC) went to EDI, NOP + CALL
// replace LEA EAX,[EBP-0x1F8], so the CALL returns to 0x5322D4. The exit to 0x532327 skips vanilla's fopen/fwrite/
// fclose and joins the epilogue the size <= 0 branch takes from the same stack depth (/GS check, RETN 8).
// [EBP+0xC] is the leaving flag; the stdcall handler keeps EBX and EBP. [EBP-0x2604] is the saved player: the only
// caller 0x5324EB passes it in EDX and EDI, the save entry hook keeps EDI, and the prologue pushes it (0x53225F).
FCT_ASM ( caller_CharacterCommit_114 )
	PUSH DWORD PTR SS:[EBP-0x2604]
	PUSH DWORD PTR SS:[EBP+0xC]
	PUSH EDI
	LEA EAX,DWORD PTR SS:[EBP-0x25F8]
	PUSH EAX
	LEA EAX,DWORD PTR SS:[EBP-0x1F8]
	PUSH EAX
	CALL CharacterCommit_114
	ADD DWORD PTR SS:[ESP],0x53
	RETN
}}

// Replaces CALL 0x52D440 at 0x52D8F9, its only call site. 0x52D440 takes the Game* in EBX, which the stdcall handler
// keeps; the JMP enters it with the return address the original CALL would have pushed.
FCT_ASM ( caller_CharacterCommitTick_114 )
	PUSH ECX
	PUSH EDX
	PUSH EBX
	CALL CharacterCommitTick
	POP EDX
	POP ECX
	JMP D2GameAutosaveTick
}}

FCT_ASM ( caller_SaveSPPlayerCustomData_111 )
	MOV EAX,DWORD PTR SS:[ESP+0x14]
	TEST EAX,EAX
	JE next
	PUSH DWORD PTR SS:[ESP+0x8]
	CALL SaveSPPlayerCustomData
next:
	JMP D2SaveSPChar
}}


/*FCT_ASM ( caller_SaveSPPlayerCustomData_111 )
	CALL D2FogGetSavePath
	PUSH DWORD PTR SS:[ESP+0x2608]
	CALL SaveSPPlayerCustomData
	RETN
}}*/

FCT_ASM ( caller_SaveSPPlayerCustomData )
	CALL D2FogGetSavePath
	PUSH EDI
	CALL SaveSPPlayerCustomData
	RETN
}}

FCT_ASM ( caller_SaveSPPlayerCustomData_9 )
	CALL D2FogGetSavePath
	PUSH ESI
	CALL SaveSPPlayerCustomData
	RETN
}}


//////////////////////////////////////////////////////////////////////////////////////////////////

#define TC_SAVE_PERSONAL 0
#define TC_SAVE_SHARED 1

#pragma pack(1)
typedef struct {
	BYTE	packID;
	BYTE	packSize;
	BYTE	init;
	DWORD	finalSize:31;
	DWORD	isCustom:1;
	BYTE	type;
	BYTE	data[0xFF];
} t_rcvMsg;
#pragma pack()

struct s_MPSaveFile
{
	int		sizeExtended;
	int		curExtended;
	BYTE*	dataExtended;
	int		sizeShared;
	int		curShared;
	BYTE*	dataShared;
};
static s_MPSaveFile receivedSaveFiles;
// Set once a chunk of this save is refused: its remaining chunks are dropped until this save is used or the next one
// starts (init); discardReceivedSaveFiles clears it.
static bool receiveFailed = false;
static BYTE customPackID = 0xB2;

// Drops the PlugY save data received so far: a later save must never use data left over from this one.
static void discardReceivedSaveFiles()
{
	D2FogMemDeAlloc(receivedSaveFiles.dataExtended,__FILE__,__LINE__,0);
	D2FogMemDeAlloc(receivedSaveFiles.dataShared,__FILE__,__LINE__,0);
	ZeroMemory(&receivedSaveFiles, sizeof(receivedSaveFiles));
	receiveFailed = false;
}
//static s_MPSaveFile* receivedSaveFiles = NULL;


struct s_dataToSend
{
	s_dataToSend*	next;
	int		clientID;
	int		sizeExtended;
	int		curExtended;
	BYTE*	dataExtended;
	int		sizeShared;
	int		curShared;
	BYTE*	dataShared;
	bool	init;
	DWORD	memoryPool;		// the pool of the game whose save the buffers hold
};
// Nodes come from pool 0: client ids restart in every game, so a node outlives the game it was made in.
s_dataToSend* ptDataToSend=NULL;

static void freeDataToSendBuffers(s_dataToSend* dataToSend)
{
	if (dataToSend->dataExtended)
		D2FreeMem(dataToSend->memoryPool, dataToSend->dataExtended,__FILE__,__LINE__,0);
	if (dataToSend->dataShared)
		D2FreeMem(dataToSend->memoryPool, dataToSend->dataShared,__FILE__,__LINE__,0);
	dataToSend->dataExtended = NULL;
	dataToSend->dataShared = NULL;
}

void discardClientSaveTransfer(DWORD clientID)
{
	s_dataToSend** link = &ptDataToSend;
	while (*link && ((DWORD)(*link)->clientID != clientID))
		link = &(*link)->next;
	s_dataToSend* dataToSend = *link;
	if (!dataToSend) return;
	if (dataToSend->dataExtended || dataToSend->dataShared)
		log_msg("Discard the save files not yet sent to client %u\n", clientID);
	freeDataToSendBuffers(dataToSend);
	*link = dataToSend->next;
	D2FreeMem(0, dataToSend,__FILE__,__LINE__,0);
}

/*
void sendDataToSave(DWORD clientID, BYTE* data, DWORD size, bool isShared)
{
	t_rcvMsg pack;
	d2_assert(size >= 0x40000000, "size of file > 0x40000000", __FILE__, __LINE__);

	pack.packID = customPackID;
	pack.init = !isShared;
	pack.finalSize = size;
	pack.type = isShared ? TC_SAVE_SHARED : TC_SAVE_PERSONAL;
	pack.isCustom = true;

	DWORD sended = 0;
	while (sended < size)
	{
		pack.packSize = (BYTE) (size - sended > 0xFE ? 0xFE : size - sended);
		CopyMemory(pack.data,  &data[sended], pack.packSize);
		log_msg("send: ID=%02X\tsize=%02X\tinit=%02X\ttotalsize=%08X\ttype=%d\n", pack.packID, pack.init, pack.packSize, pack.finalSize, pack.type);
		pack.packSize++;
		D2SendToClient(0, clientID, (BYTE*)&pack, pack.packSize+7);
		pack.init = false;
		sended += pack.packSize -1;
	}
	log_msg("\n");
}*/

void STDCALL SendSaveFilesToSave( Unit* ptChar )
{
//	if (!D2isLODGame()) return;
	log_msg("\n--- Start SendSaveFilesToSave ---\n");

	DWORD curSizeExt = 0;
	BYTE* dataExt = NULL;
	DWORD curSizeShr = 0;
	BYTE* dataShr = NULL;

	if (PCPY->selfStashIsOpened)
	{
		DWORD maxSizeExt = 0x4000;
		dataExt = (BYTE *)D2AllocMem(PCGame->memoryPool, maxSizeExt,__FILE__,__LINE__,0);
		d2_assert(!dataExt, "Error : Memory allocation Extended SaveFile", __FILE__, __LINE__);
		saveExtendedSaveFile(ptChar, &dataExt, &maxSizeExt, &curSizeExt);
	}

	if (active_sharedStash && PCPY->sharedStashIsOpened)
	{
		DWORD maxSizeShr = 0x4000;
		dataShr = (BYTE *)D2AllocMem(PCGame->memoryPool, maxSizeShr,__FILE__,__LINE__,0);
		d2_assert(!dataShr, "Error : Memory allocation Shared SaveFile", __FILE__, __LINE__);
		saveSharedSaveFile(ptChar, &dataShr, &maxSizeShr, &curSizeShr);
	}

	NetClient* ptClient = D2GetClient(ptChar,__FILE__,__LINE__);
	s_dataToSend* dataToSend = ptDataToSend;
	while (dataToSend && (dataToSend->clientID != ptClient->clientID))
		dataToSend = dataToSend->next;
	if (!dataToSend)
	{
		dataToSend = (s_dataToSend*) D2AllocMem(0, sizeof(s_dataToSend), __FILE__, __LINE__, 0);
		ZeroMemory(dataToSend, sizeof(s_dataToSend));
		dataToSend->next = ptDataToSend;
		ptDataToSend = dataToSend;
	}
	// This save replaces one not yet sent, which vanilla makes when it saves twice in a row (a player leaving).
	freeDataToSendBuffers(dataToSend);
	dataToSend->memoryPool = PCGame->memoryPool;

	//Sending savefiles
	dataToSend->clientID = ptClient->clientID;
	dataToSend->init = 1;
	dataToSend->sizeExtended = curSizeExt;
	dataToSend->curExtended = 0;
	dataToSend->dataExtended = dataExt;
	dataToSend->sizeShared = curSizeShr;
	dataToSend->curShared = 0;
	dataToSend->dataShared = dataShr;
	log_msg("SendSaveFilesToSave : clientID=%d\t init=%d\t sizeExtended=%d\t curExtended=%d\t dataExtended=%d\t sizeShared=%d\t curShared=%d\t dataShared=%08X\n",
		dataToSend->clientID, dataToSend->init, dataToSend->sizeExtended, dataToSend->curExtended, dataToSend->dataExtended, dataToSend->sizeShared, dataToSend->curShared, dataToSend->dataShared);

/*	NetClient* ptClient = D2GetClient(ptChar,__FILE__,__LINE__);
	if (dataExt)
	{
		sendDataToSave(ptClient->clientID, dataExt, curSizeExt, false);
		D2FreeMem(PCGame->memoryPool, dataExt,__FILE__,__LINE__,0);
	}
	if (dataShr)
	{
		sendDataToSave(ptClient->clientID, dataShr, curSizeShr, true);
		D2FreeMem(PCGame->memoryPool, dataShr,__FILE__,__LINE__,0);
	}
*/
	log_msg("SendSaveFilesToSave : End\n\n");
}


DWORD STDCALL ManageNextPacketToSend(NetClient* ptClient)
{
	log_msg("ManageNextPacketToSend\n");
	s_dataToSend* dataToSend = ptDataToSend;
	while (dataToSend && (dataToSend->clientID != ptClient->clientID))
		dataToSend = dataToSend->next;
	if (!dataToSend) return 1;

	log_msg("ManageNextPacketToSend : clientID=%d\t init=%d\t sizeExtended=%d\t curExtended=%d\t dataExtended=%08X\t sizeShared=%d\t curShared=%d\t dataShared=%08X\n",
		dataToSend->clientID, dataToSend->init, dataToSend->sizeExtended, dataToSend->curExtended, dataToSend->dataExtended, dataToSend->sizeShared, dataToSend->curShared, dataToSend->dataShared);

	if (dataToSend->sizeExtended && dataToSend->dataExtended && (dataToSend->curExtended < dataToSend->sizeExtended))
	{
		DWORD remainingData = dataToSend->sizeExtended - dataToSend->curExtended;
		t_rcvMsg* msg = (t_rcvMsg*)D2AllocMem(PClientGame->memoryPool, sizeof(t_rcvMsg),__FILE__,__LINE__,0);
		msg->packID = customPackID;
		msg->init = dataToSend->init;
		msg->finalSize = dataToSend->sizeExtended;
		msg->type = TC_SAVE_PERSONAL;
		msg->isCustom = true;
		msg->packSize = remainingData > 0xFE ? 0xFF : (BYTE)remainingData + 1;
		CopyMemory(msg->data,  &dataToSend->dataExtended[dataToSend->curExtended], msg->packSize - 1);
		log_msg("ManageNextPacketToSend : type=%d\t init=%d\t finalSize=%d\t packSize=%d\t data=%08X\n", msg->type, msg->init, msg->finalSize, msg->packSize, msg->data);
		D2SendToClient(0, dataToSend->clientID, msg, msg->packSize+7);
		dataToSend->init = false;
		dataToSend->curExtended += msg->packSize - 1;
		D2FreeMem(PClientGame->memoryPool, msg,__FILE__,__LINE__,0);
		if (dataToSend->curExtended == dataToSend->sizeExtended)
		{
			dataToSend->sizeExtended = 0;
			dataToSend->curExtended = 0;
			D2FreeMem(dataToSend->memoryPool, dataToSend->dataExtended,__FILE__,__LINE__,0);
			dataToSend->dataExtended = NULL;
		}
		log_msg("ManageNextPacketToSend : End\n");
		return 0;//ManageNextPacketToSend(ptClient);//return 0;
	}

	if (dataToSend->sizeShared && dataToSend->dataShared && (dataToSend->curShared < dataToSend->sizeShared))
	{
		DWORD remainingData = dataToSend->sizeShared - dataToSend->curShared;
		t_rcvMsg* msg = (t_rcvMsg*)D2AllocMem(PClientGame->memoryPool, sizeof(t_rcvMsg),__FILE__,__LINE__,0);
		msg->packID = customPackID;
		msg->init = dataToSend->init;
		msg->finalSize = dataToSend->sizeShared;
		msg->type = TC_SAVE_SHARED;
		msg->isCustom = true;
		msg->packSize = remainingData > 0xFE ? 0xFF : (BYTE)remainingData + 1;
		CopyMemory(msg->data,  &dataToSend->dataShared[dataToSend->curShared], msg->packSize - 1);
		log_msg("ManageNextPacketToSend : type=%d\t init=%d\t finalSize=%d\t packSize=%d\t data=%08X\n", msg->type, msg->init, msg->finalSize, msg->packSize, msg->data);
		D2SendToClient(0, dataToSend->clientID, msg, msg->packSize+7);
		dataToSend->init = false;
		dataToSend->curShared += msg->packSize - 1;
		D2FreeMem(PClientGame->memoryPool, msg,__FILE__,__LINE__,0);
		if (dataToSend->curShared == dataToSend->sizeShared)
		{
			dataToSend->sizeShared = 0;
			dataToSend->curShared = 0;
			D2FreeMem(dataToSend->memoryPool, dataToSend->dataShared,__FILE__,__LINE__,0);
			dataToSend->dataShared = NULL;
		}
		log_msg("ManageNextPacketToSend : End\n");
		return 0;//ManageNextPacketToSend(ptClient);//return 0;
	}

	log_msg("ManageNextPacketToSend : End\n");
	return 1;
}


DWORD STDCALL ReceiveSaveFilesToSave(t_rcvMsg* msg)
{
	if( (msg->packID != customPackID) || !msg->isCustom) return 0;

	log_msg("ReceiveSaveFilesToSave : Receive Packet - type=%d\t init=%d\t finalSize=%d\t packSize=%d\t data=%08X\n", msg->type, msg->init, msg->finalSize, msg->packSize, msg->data);

	bool isShared;

	if ( msg->type == TC_SAVE_PERSONAL)
		isShared = false;
	else if (msg->type == TC_SAVE_SHARED)
		isShared = true;
	else return 0;


	if (msg->init)
		discardReceivedSaveFiles();

	log_msg("ReceiveSaveFilesToSave : sizeExtended=%d\t curExtended=%d\t dataExtended=%08X\t sizeShared=%d\t curShared=%d\t dataShared=%08X\n", receivedSaveFiles.sizeExtended, receivedSaveFiles.curExtended, receivedSaveFiles.dataExtended, receivedSaveFiles.sizeShared, receivedSaveFiles.curShared, receivedSaveFiles.dataShared);

	// A refused chunk is still consumed (return 1): it is ours, and vanilla's dispatcher must never see it.
	if (receiveFailed)
	{
		log_msg("ReceiveSaveFilesToSave : this save was refused, chunk dropped\n");
		return 1;
	}

	// No per-file init rule: init marks only the first packet of the whole save, the shared file starts without it.
	int& size = isShared ? receivedSaveFiles.sizeShared : receivedSaveFiles.sizeExtended;
	int& cur = isShared ? receivedSaveFiles.curShared : receivedSaveFiles.curExtended;
	BYTE*& data = isShared ? receivedSaveFiles.dataShared : receivedSaveFiles.dataExtended;
	const char* refused = NULL;
	switch (checkSaveChunk(size, cur, msg->packSize, msg->finalSize, SAVE_TRANSFER_MAX_SIZE))
	{
	case SAVE_CHUNK_START:
		size = msg->finalSize;
		data = (BYTE *)D2FogMemAlloc(size,__FILE__,__LINE__,0);
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
		log_msg("ReceiveSaveFilesToSave : %s file refused (%s), the rest of this save is dropped\n", isShared ? "shared" : "extended", refused);
		discardReceivedSaveFiles();
		receiveFailed = true;
		return 1;
	}

	DWORD payload = msg->packSize - 1;
	CopyMemory(&data[cur], msg->data, payload);
	cur += payload;
	log_msg("ReceiveSaveFilesToSave : End\n");
	return 1;
}


// The name becomes a file path, so only character-name characters pass: letters, '-' and '_' (renameCharacter accepts
// a leading '_'), 2 to 15 of them.
static bool isValidCharName(const char* name)
{
	int len = 0;
	for (; name[len]; len++)
	{
		char c = name[len];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_'))
			return false;
	}
	return len >= 2 && len <= 15;
}

void STDCALL SaveMPPlayerCustomData(BYTE* dataD2Savefile )
{
	log_msg("SaveMPPlayerCustomData - Start.\n");

	// Runs before vanilla's magic/version/length checks: only 8 bytes are length-checked, the name read assumes a full image.
	if (*(DWORD*)dataD2Savefile != 0xAA55AA55)
	{
		log_msg("SaveMPPlayerCustomData - not a .d2s image, nothing saved\n");
		discardReceivedSaveFiles();
		log_msg("--- SaveMPPlayerCustomData End ---\n\n");
		return;
	}
	// Saved under the character this client joined with, the name vanilla gives the .d2s: the host supplies the image,
	// and the client player is already gone when a leaving client receives its save.
	Unit* ptChar = D2GetClientPlayer();
	char name[sizeof(joinedCharName)];
	CopyMemory(name, joinedCharName, sizeof(name));
	// A backup-only install (ActiveAutoBackup without custom data) never runs SendSaveFiles: it names the backup after
	// the player, as before, and skips it once the player is gone.
	if (!name[0] && ptChar)
	{
		strncpy(name, PCPlayerData->name, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
	}
	if (!isValidCharName(name))
	{
		log_msg("SaveMPPlayerCustomData - no character name to save under, nothing saved\n");
		discardReceivedSaveFiles();
		log_msg("--- SaveMPPlayerCustomData End ---\n\n");
		return;
	}
	if (strncmp(name, (char*)dataD2Savefile + 0x14, 16))
		log_msg("SaveMPPlayerCustomData - the save image names %.16s, saved as %s\n", (char*)dataD2Savefile + 0x14, name);
	bool isHardcore = (dataD2Savefile[0x24] & 4) == 4;

	backupSaveFiles(name, isHardcore);

	if (active_PlayerCustomData)
	{
		if( receivedSaveFiles.sizeExtended && (receivedSaveFiles.sizeExtended == receivedSaveFiles.curExtended) && (!active_sharedStash || receivedSaveFiles.sizeShared && (receivedSaveFiles.sizeShared == receivedSaveFiles.curShared)) )
		{
			log_msg("SaveMPPlayerCustomData - Saving can start\n");
			writeExtendedSaveFile(name, receivedSaveFiles.dataExtended, receivedSaveFiles.sizeExtended);
			if (active_sharedStash)
				writeSharedSaveFile(name, receivedSaveFiles.dataShared, receivedSaveFiles.sizeShared, isHardcore);//6FBAB9A4-6FAA0000
			log_msg("SaveMPPlayerCustomData - Receive Savefiles completed\n\n");
		} else if (ptChar && !strcmp(PCPlayerData->name, name)) {
			log_box("Connection error : save with client data.");
			SaveSPPlayerCustomData(ptChar);
		} else {
			// Nothing to save from a player that is gone.
			log_msg("SaveMPPlayerCustomData - PlugY data for %s did not arrive, .d2x not saved\n", name);
		}
	}
	discardReceivedSaveFiles();
	log_msg("--- SaveMPPlayerCustomData End ---\n\n");
}





/*
FCT_ASM ( caller_SendSaveFilesToSave_9 )
	POP ECX
	PUSH EAX
	PUSH ECX
	PUSH EBX
	CALL SendSaveFilesToSave
	MOV EDX,EBX
	MOV ECX,EDI
	RETN
}}

FCT_ASM ( caller_SendSaveFilesToSave )
	POP ECX
	PUSH EAX
	PUSH ECX
	PUSH EBX
	CALL SendSaveFilesToSave
	MOV EDX,EBX
	MOV ECX,EBP
	RETN
}}
*/

FCT_ASM( caller_ManageNextPacketToSend )
	PUSH ESI
	CALL ManageNextPacketToSend
	TEST EAX,EAX
	JNZ end_caller_ManageNextPacketToSend
	XOR ECX,ECX
	RETN
end_caller_ManageNextPacketToSend:
	MOV ECX,DWORD PTR DS:[ESI+0x17C]
	RETN
}}

FCT_ASM( caller_ManageNextPacketToSend_9 )
	PUSH ESI
	CALL ManageNextPacketToSend
	TEST EAX,EAX
	JNZ end_caller_ManageNextPacketToSend_9
	MOV DWORD PTR DS:[ESI+0x14C],5
	POP ESI
	POP ESI
	RETN
end_caller_ManageNextPacketToSend_9:
	MOV EAX,DWORD PTR DS:[ESI+0x150]
	RETN
}}

FCT_ASM( caller_ManageNextPacketToSend_9d )
	PUSH ESI
	CALL ManageNextPacketToSend
	TEST EAX,EAX
	JNZ end_caller_ManageNextPacketToSend_9d
	MOV DWORD PTR DS:[ESI+0x170],5
	POP ESI
	POP ESI
	RETN
end_caller_ManageNextPacketToSend_9d:
	MOV EAX,DWORD PTR DS:[ESI+0x174]
	RETN
}}

FCT_ASM( caller_SendSaveFilesToSave_114 )
	PUSH DWORD PTR SS:[ESP+0x4]
	CALL SendSaveFilesToSave
	MOV EDI,DWORD PTR SS:[EBP-0x2008]
	RETN
}}

FCT_ASM( caller_SendSaveFilesToSave_111 )
	PUSH DWORD PTR SS:[ESP+0x2014]
	CALL SendSaveFilesToSave
	MOV EAX,DWORD PTR SS:[ESP+0x8]
	TEST EAX,EAX
	RETN
}}

FCT_ASM( caller_SendSaveFilesToSave )
	PUSH EDI
	CALL SendSaveFilesToSave
	MOV EDI,DWORD PTR SS:[ESP+0x14]
	TEST EDI,EDI
	RETN
}}

FCT_ASM( caller_SendSaveFilesToSave_9 )
	PUSH ESI
	CALL SendSaveFilesToSave
	MOV ESI,DWORD PTR SS:[ESP+0x14]
	TEST ESI,ESI
	RETN
}}

FCT_ASM ( caller_ReceivedSaveFilesToSave_114 )
	CMP EDX,-1
	JE continue_rcvFct
	PUSH ECX
	PUSH EDX
	PUSH ECX
	CALL ReceiveSaveFilesToSave
	POP EDX
	POP ECX
	TEST EAX,EAX
	JE continue_rcvFct
	RETN
continue_rcvFct:
	JMP D2ReceivePacket
}}

FCT_ASM ( caller_ReceivedSaveFilesToSave_111 )
	LEA EAX,DWORD PTR SS:[ESP+0x10]
	PUSH EAX
	CALL ReceiveSaveFilesToSave
	TEST EAX,EAX
	JE continue_rcvFct
	ADD DWORD PTR SS:[ESP],0x3D
	RETN
continue_rcvFct:
	MOVZX EAX,BYTE PTR SS:[ESP+0x10]
	RETN
}}

FCT_ASM ( caller_ReceivedSaveFilesToSave )
	CMP EDX,-1
	JE continue_rcvFct
	PUSH ECX
	PUSH EDX
	PUSH ECX
	CALL ReceiveSaveFilesToSave
	POP EDX
	POP ECX
	TEST EAX,EAX
	JE continue_rcvFct
	XOR EAX,EAX
	ADD ESP,4
	RETN
continue_rcvFct:
	POP EAX
	SUB ESP,0x5F4//5F4
	JMP EAX
//	JMP DWORD PTR SS:[ESP+0x5F0]
}}

FCT_ASM ( caller_SaveMPPlayerCustomData_111 )
	PUSH EAX
	PUSH ECX
	PUSH EAX
	CALL SaveMPPlayerCustomData
	POP ECX
	POP EAX
	CMP ECX,0xAA55AA55
	RETN
}}

FCT_ASM ( caller_SaveMPPlayerCustomData )
	PUSH ECX
	PUSH ECX
	CALL SaveMPPlayerCustomData
	POP ECX
	MOV EAX,DWORD PTR DS:[ECX]
	MOV EDX,DWORD PTR DS:[ECX+4]
	RETN
}}


void Install_SavePlayerData()
{
	static int isInstalled = false;
	if (isInstalled) return;

	log_msg("Patch D2Game & D2Client for save Player's custom data. (SavePlayerData)\n");

	//Save single player custom data.
	if (version_D2Game >= V111)
	{
		mem_seek R8(D2Game, 0000, 0000, 0000, B9801, 25911, 44601, 543D1, 39CD1, 1324EC);
		MEMC_REF4( D2SaveSPChar, version_D2Game >= V114a ? caller_SaveSPPlayerCustomData_114 : version_D2Game >= V111 ? caller_SaveSPPlayerCustomData_111 : version_D2Game != V109b ? caller_SaveSPPlayerCustomData : caller_SaveSPPlayerCustomData_9);
		//6FCD9800  |. E8 3BFBFFFF    CALL D2Game.6FCD9340
		//6FC45910  |. E8 3BFBFFFF    CALL D2Game.6FC45450	
		//6FC64600  |. E8 3BFBFFFF    CALL D2Game.6FC64140
		//6FC743D0  |. E8 3BFBFFFF    CALL D2Game.6FC73F10
		//6FC59CD0  |. E8 3BFBFFFF    CALL D2Game.6FC59810
		//005324EB  |. E8 50FDFFFF    CALL Game.00532240                       ; \Game.00532240
	}
	else
	{
		mem_seek R8(D2Game, 4DF04, 4E304, 5A624, B9365, 25475, 44165, 53F35, 39835, 132276);
		MEMJ_REF4( D2FogGetSavePath, /*version_D2Game >= V114a ? caller_SaveSPPlayerCustomData : version_D2Game >= V111 ? caller_SaveSPPlayerCustomData_111 :*/ version_D2Game != V109b ? caller_SaveSPPlayerCustomData : caller_SaveSPPlayerCustomData_9);
		//6FC8A623   E8 3E210900      CALL <JMP.&Fog.#10115>
		//02039364  |. E8 3B0FF5FF    CALL <JMP.&Fog.#10115>
		//01F95474  |. E8 C34EFEFF    CALL <JMP.&Fog.#10115>
		//6FC64164  |. E8 EB61FCFF    CALL <JMP.&Fog.#10115>
		//6FC73F34  |. E8 DD63FBFF    CALL <JMP.&Fog.#10115>
		//6FC59834  |. E8 FB0AFDFF    CALL <JMP.&Fog.#10115>
		//00532275  |. E8 D64DEDFF    CALL Game.00407050
	}

	if (active_PlayerCustomData)
	{
		//Send SaveFiles
		mem_seek R8(D2Game, 4DFFA, 4E3FA, 5A720, B92DB, 253EB, 440DB, 53EAB, 397AB, 132398);
		memt_byte( 0x8B ,0x90); // NOP
		memt_byte( version_D2Game >= V114d ? 0xBD : version_D2Game >= V111 ? 0x44 : version_D2Game != V109b ? 0x7C : 0x74, 0xE8); // CALL
		MEMT_REF4( version_D2Game >= V114d ? 0xFFFFDFF8 : version_D2Game >= V111 ? 0xC0850424 : version_D2Game != V109b ? 0xFF851024 : 0xF6851024, version_D2Game >= V114d ? caller_SendSaveFilesToSave_114 : version_D2Game >= V111 ? caller_SendSaveFilesToSave_111 : version_D2Game != V109b ? caller_SendSaveFilesToSave : caller_SendSaveFilesToSave_9);
		//6FC8A720  |. 8B7C24 10      MOV EDI,DWORD PTR SS:[ESP+10]
		//6FC8A724  |. 85FF           TEST EDI,EDI
		//020392DB  |. 8B4424 04      MOV EAX,DWORD PTR SS:[ESP+4]
		//020392DF  |. 85C0           TEST EAX,EAX
		//01F953EB  |. 8B4424 04      MOV EAX,DWORD PTR SS:[ESP+4]
		//01F953EF  |. 85C0           TEST EAX,EAX
		//6FC640DB  |. 8B4424 04      MOV EAX,DWORD PTR SS:[ESP+4]
		//6FC640DF  |. 85C0           TEST EAX,EAX
		//6FC73EAB  |. 8B4424 04      MOV EAX,DWORD PTR SS:[ESP+4]
		//6FC73EAF  |. 85C0           TEST EAX,EAX
		//6FC597AB  |. 8B4424 04      MOV EAX,DWORD PTR SS:[ESP+4]
		//6FC597AF  |. 85C0           TEST EAX,EAX
		//00532398  |. 8BBD F8DFFFFF  MOV EDI,DWORD PTR SS:[EBP-2008]

		mem_seek R8(D2Game, 7993, 7A13, 7BBB, E2943, E6D83, A89D3, 2D173, BEDD3, 12E13E);
		memt_byte( 0x8B ,0x90); // NOP
		memt_byte( version_D2Game >= V110 ? 0x8E : 0x86 ,0xE8); // CALL
		MEMT_REF4( version_D2Game >= V110 ? 0x0000017C : version_D2Game == V109d ? 0x0000174 : 0x00000150, version_D2Game >= V110 ? caller_ManageNextPacketToSend : version_D2Game == V109d ? caller_ManageNextPacketToSend_9d : caller_ManageNextPacketToSend_9);
		//6FC37BBB  |> 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]
		//02062943  |. 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]
		//02056D83  |. 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]
		//6FCC89D3  |. 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]
		//6FC4D173  |. 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]
		//6FCDEDD3  |. 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]
		//0052E13E  |> 8B8E 7C010000  MOV ECX,DWORD PTR DS:[ESI+17C]

		//	mem_seek R7(D2Game, 7B82, 7C05, 7DCF, 0000, 0000, 0000, 0000);//117FC,	117EC,	11DBC
		//	memt_byte( 0x8B ,0xE8); // CALL
		//	MEMT_REF4( (version_D2Game == V110) ?  0x50CD8BD3 : 0x50CF8BD3, (version_D2Game == V110) ? caller_SendSaveFilesToSave : caller_SendSaveFilesToSave_9);
			//6FC37DCF  |. 8BD3           |MOV EDX,EBX
			//6FC37DD1  |. 8BCD           |MOV ECX,EBP
			//6FC37DD3  |. 50             |PUSH EAX

		//Received SaveFiles
		mem_seek R8(D2Client, 116F0, 116E0, 11CB0, 89246, 32076, 7BCD6, 43946, 448E6, 4C70D);
		if ( version_D2Game >= V114a )
		{
			MEMC_REF4( D2ReceivePacket, caller_ReceivedSaveFilesToSave_114);
			//0044C70C  |. E8 3F010100    |CALL Game.0045C850
			// Two other client receive sites call the same dispatcher; a leaving client gets its save through one of them.
			mem_seek R8(D2Client, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 4BB4F);
			MEMC_REF4( D2ReceivePacket, caller_ReceivedSaveFilesToSave_114);
			//0044BB4E  |. E8 FD0C0100    |CALL Game.0045C850
			mem_seek R8(D2Client, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 4BD5B);
			MEMC_REF4( D2ReceivePacket, caller_ReceivedSaveFilesToSave_114);
			//0044BD5A  |. E8 F10A0100    |CALL Game.0045C850
		}
		else if ( version_D2Game >= V111 )
		{
			memt_byte( 0x0F ,0xE8);
			MEMT_REF4( 0x0C2444B6, caller_ReceivedSaveFilesToSave_111);
			//6FB39246  |. 0FB64424 0C    |MOVZX EAX,BYTE PTR SS:[ESP+C]
			//6FAE2076  |. 0FB64424 0C    |MOVZX EAX,BYTE PTR SS:[ESP+C]
			//6FB2BCD6  |. 0FB64424 0C    |MOVZX EAX,BYTE PTR SS:[ESP+C]
			//6FAF3946  |. 0FB64424 0C    |MOVZX EAX,BYTE PTR SS:[ESP+C]
			//6FAF48E6  |. 0FB64424 0C    |MOVZX EAX,BYTE PTR SS:[ESP+C]
		} else {
			//Received SaveFiles
			memt_byte( 0x81 ,0x90); // NOP
			memt_byte( 0xEC ,0xE8); // CALL
			MEMT_REF4( 0x000005F4, caller_ReceivedSaveFilesToSave);
			//6FAB1CB0  |$ 81EC F4050000  SUB ESP,5F4
		}
	}

	if ( version_D2Game >= V111 )
	{
		// Save multiplayer player custom data.
		mem_seek R8(D2Client, 117FC, 117EC, 11DBC, 99AE2, BD7F2, 64A22, AC572, 829C2, 5C565);
		memt_byte( 0x81 ,0xE8); // CALL
		MEMT_REF4( 0x55AA55F9, caller_SaveMPPlayerCustomData_111);
		memt_byte( 0xAA ,0x90); // CALL
		//6FB49AE2  |. 81F9 55AA55AA  CMP ECX,AA55AA55
		//6FB6D7F2  |. 81F9 55AA55AA  CMP ECX,AA55AA55
		//6FB14A22  |. 81F9 55AA55AA  CMP ECX,AA55AA55
		//6FB5C572  |. 81F9 55AA55AA  CMP ECX,AA55AA55
		//6FB329C2  |. 81F9 55AA55AA  CMP ECX,AA55AA55
		//0045C565  |. 81F9 55AA55AA  CMP ECX,AA55AA55
	} else {
		// Save multiplayer player custom data.
		mem_seek R8(D2Client, 117FC, 117EC, 11DBC, 99AE2, BD7F2, 64A22, 0000, 0000, 0000);
		memt_byte( 0x8B ,0xE8); // CALL
		MEMT_REF4( 0x04518B01, caller_SaveMPPlayerCustomData);
		//6FAB1DBC  |. 8B01           MOV EAX,DWORD PTR DS:[ECX]
		//6FAB1DBE  |. 8B51 04        MOV EDX,DWORD PTR DS:[ECX+4]
	}

	if ( version_D2Game == V109b || version_D2Game == V109d )
		customPackID = 0xAB;
	else if ( version_D2Game >= V111 )
		customPackID++;

	log_msg("\n");

	isInstalled = true;
}

// Owns every crash-safe save patch. The load sites, the tick and the PlayerData free are installed whatever
// ActiveCrashSafeSaves says, so a save a crash interrupted is always finished and a character always runs in one game
// at a time.
void Install_CharacterCommit()
{
	static int isInstalled = false;
	if (isInstalled) return;
	isInstalled = true;

	if (version_D2Game != V114d)
	{
		log_msg("Crash-safe saves need Diablo II 1.14d: not installed. (CharacterCommit)\n\n");
		return;
	}
	// The session lock ends with the server PlayerData that owns it, even with the multi-page stash off.
	Install_PlayerDataFree();
	log_msg("Patch D2Game for crash-safe single-player saves. (CharacterCommit)\n");

#ifdef PLUGY_TEST_HOOKS
	char flag[8];
	DWORD len = GetEnvironmentVariableA("PLUGY_ALL_OR_NOTHING_LEAVE", flag, sizeof(flag));
	allOrNothingLeaveSave = len && len < sizeof(flag) && atoi(flag) != 0;
	log_msg("Test hook: allOrNothingLeaveSave = %d\n", allOrNothingLeaveSave);
#endif

	if (active_CrashSafeSaves)
	{
		// The .d2s write of D2SaveSPChar.
		mem_seek R8(D2Game, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 1322CE);
		memt_byte( 0x8D, 0x90); // NOP
		memt_byte( 0x85, 0xE8); // CALL
		MEMT_REF4( 0xFFFFFE08, caller_CharacterCommit_114);
		//005322CE  |. 8D85 08FEFFFF  LEA EAX,DWORD PTR SS:[EBP-1F8]
		lateHookInstalled = true;
	}

	// The game's per-frame autosave tick.
	mem_seek R8(D2Game, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 12D8FA);
	MEMC_REF4( D2GameAutosaveTick, caller_CharacterCommitTick_114);
	//0052D8F9  |. E8 42FBFFFF    CALL Game.0052D440

	// Load SP player, at the open of the .d2s: the session lock and recovery come before the game reads the character.
	mem_seek R8(D2Game, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 134411);
	MEMC_REF4( D2GameOpenFile, caller_OpenSPCharacter_114);
	//00534410  |. E8 85DF1400    CALL Game.0068239A

	// Load SP player, the read of the .d2s: a read of nothing fails the load before the built-player site below.
	mem_seek R8(D2Game, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 13442E);
	MEMC_REF4( D2GameReadFile, ReadSPCharacter_114);
	//0053442D  |. E8 50031500    CALL Game.00684782

	// Load SP player, once the player is built: its lock owner, then the custom data when it is on (Install_LoadPlayerData
	// leaves this site to this installer on 1.14d).
	mem_seek R8(D2Game, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000, 13447A);
	memt_byte( 0x8B, 0xE8); // CALL
	MEMT_REF4( 0x75FF85F8, caller_LoadSPCharacter_114);
	memt_byte( 0x13, 0x90); // NOP
	//0053447A  |. 8BF8           MOV EDI,EAX
	//0053447C  |. 85FF           TEST EDI,EDI
	//0053447E  |. 75 13          JNZ SHORT Game.00534493

	log_msg("\n");
}

/*================================= END OF FILE =================================*/
