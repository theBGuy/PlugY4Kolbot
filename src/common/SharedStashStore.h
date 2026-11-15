/*=================================================================
	Live shared stash, the SQLite store: one database per shared
	stash, its schema, the write lock and every read and write the
	game and the tool make. No PlugY or game types.
	The statement and transaction wrappers, the status mapping and
	the cells table come from xrip's shared stash store (GPL-3.0).
=================================================================*/
#pragma once

#include "SharedStashFormat.h"

struct sqlite3;

// "PYLV". The schema pragma and the open gate both come from this constant.
#define LIVE_APPLICATION_ID 1348029526
static_assert(LIVE_APPLICATION_ID == 0x50594C56, "PYLV");
#define LIVE_USER_VERSION 1
// PENDING ops per character; the op that would pass it answers BUSY.
#define LIVE_OPS_CAP 1024
// error() holds at most this many characters, the terminating NUL included; a longer message is cut.
#define SHARED_STASH_ERROR_SIZE 256

namespace SharedStash
{
	// One line, no newline. NULL (the default) is silent. Process-wide: set it before any Store is used.
	typedef void (*LogSink)(const char* line);
	void SetLogSink(LogSink sink);

	// Whether a database path is on a local volume and reached by one path. A \\server\share spelling is refused
	// before any I/O. canonical (MAX_PATH chars, may be NULL): the final folder path and the file name, not
	// lower-cased.
	enum LocalPath { LOCAL_OK, LOCAL_UNC, LOCAL_NOT_LOCAL_DRIVE, LOCAL_REPARSE, LOCAL_HARD_LINK, LOCAL_ERROR };
	LocalPath LocalDatabasePath(const char* ansiPath, char* canonical);
	// The ANSI code page path as UTF-8, the encoding sqlite3_open_v2 takes.
	bool SqlitePath(const char* ansiPath, std::string* utf8);
	// The final folder path (short names, case and links resolved) joined to the file name as given, lower-cased:
	// the input of LiveStashLockName. Only the folder must exist. out: MAX_PATH chars.
	bool CanonicalDatabasePath(const char* ansiPath, char* out);

	// The database write lock held by a caller across several steps. Windows mutex ownership belongs to the thread
	// and is recursive, so the Store's own per-call lock nests inside it on the same thread. OK with owned() false:
	// the lock is unavailable on this machine; each Store write still serializes through SQLite's file lock, but the
	// steps are not one exclusive section.
	class WriteLock
	{
	public:
		WriteLock();
		~WriteLock();
		Status acquire(const char* ansiDbPath, DWORD waitMs);
		void release();
		bool owned() const;
	private:
		WriteLock(const WriteLock&);
		WriteLock& operator=(const WriteLock&);
		HANDLE mutex_;
		bool owned_;
		bool acquired_;
	};

	struct LockHold;

	// No exception leaves a public function; every failure sets error(). No call keeps a transaction, a statement or
	// the write lock after it returns. One thread at a time per Store. A Store with static storage in a DLL must be
	// closed before the process exits: its destructor would otherwise checkpoint and close under the loader lock.
	class Store
	{
	public:
		Store();
		~Store();
		Status create(const char* ansiPath, const Meta& meta, DWORD lockWaitMs);
		Status open(const char* ansiPath, bool readOnly = false);
		void close();
		const char* error() const;
		Status readMeta(Meta* out);
		Status readAll(Meta* meta, std::vector<PageRow>* pages, std::vector<ItemRow>* items,
			std::vector<OpRow>* pendingOps, Id* maxOpId = NULL);
		Status importStash(const std::vector<PageRow>& pages, const std::vector<ItemRow>& items, DWORD gold,
			DWORD lockWaitMs);
		Status setMode(Mode from, Mode to, DWORD lockWaitMs);
		Status beginConvertBack(std::vector<std::string>* pendingNames, DWORD lockWaitMs);
		Status settleOps(const char* character, const unsigned char dbGuid[16], const Id* ids, DWORD count,
			DWORD lockWaitMs, Id* firstBad);
		Status revertPendingOps(const char* character, DWORD lockWaitMs, DWORD* reverted);
		Status pendingCharacters(std::vector<std::string>* names);
		Status hasPendingOps(const char* character, bool* pending);
		Status opStates(const Id* ids, DWORD count, std::vector<OpRow>* out);
		Status maxOpId(Id* out);
		Status backupTo(const char* ansiPath);
		Status checkpointTruncate(DWORD lockWaitMs);
		Status check(std::string* problem);
#ifdef SHARED_STASH_TEST_API
		void testSetWriteLockName(const char* name);
		void testFailCreate(int step);
		Status testAddPendingOp(const char* character, Id* opId, DWORD lockWaitMs);
		Status testAttachItem(Id opId, Id itemId, int pendingKind, DWORD lockWaitMs);
		Status testInsertDeposit(Id opId, const ItemRow& row, DWORD lockWaitMs);
		// PRAGMA <name> read through this Store's own connection; -1 when the Store is closed or the read fails.
		int testPragmaInt(const char* name);
#endif
	private:
		Store(const Store&);
		Store& operator=(const Store&);
		void resolveLock(const char* ansiPath);
		DWORD beginLockedWork(DWORD lockWaitMs, LockHold* hold);
		void setError(const char* text);
		sqlite3* db_;
		bool readOnly_;
		HANDLE mutex_;
		bool lockUnavailable_;
		bool waitFailedLogged_;
		// A fixed buffer, so that recording a failure can never itself fail.
		char error_[SHARED_STASH_ERROR_SIZE];
		// Test-only state, kept unconditional so the class layout never depends on SHARED_STASH_TEST_API.
		char lockNameOverride_[SHARED_STASH_MUTEX_NAME_SIZE];
		int failCreateStep_;
	};
}
