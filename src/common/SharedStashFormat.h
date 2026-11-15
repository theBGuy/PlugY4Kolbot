/*=================================================================
	Live shared stash, the pure half: the row types the store and
	the builder share, the classic .sss builder and header parse,
	the .sss marker, the folder state table and the lock names.
	No file I/O, no SQLite, no game types, so the tests cover every
	rule offline (tests/SharedStashFormatTests.cpp).
=================================================================*/
#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Both names hold at most this many characters, the terminating NUL included.
#define SHARED_STASH_MUTEX_NAME_SIZE 64

// Everything is used qualified: PendingKind and PENDING_NONE already exist as global names in src/plugy, so no file
// may add a using-directive for this namespace.
namespace SharedStash
{
	typedef __int64 Id;
	typedef std::vector<unsigned char> Bytes;
	enum Status { OK, BUSY, NOT_FOUND, CONFLICT, INVALID, FOREIGN, NEWER, STORE_ERROR };
	enum Mode { MODE_IMPORTING = 0, MODE_LIVE = 1, MODE_CONVERTING_BACK = 2, MODE_BACKUP_COPY = 3 };
	enum OpState { OP_PENDING = 0, OP_SETTLED = 1, OP_REVERTED = 2 };
	enum PendingKind { PENDING_NONE = 0, PENDING_DEPOSITED = 1, PENDING_WITHDRAWN = 2 };
	struct Meta { unsigned char dbGuid[16]; Mode mode; int gridW, gridH; DWORD gold; Id revision; WORD sssVersion; };
	// page is always a PageRow.id. pendingOp 0 with pendingKind PENDING_NONE means "no marker".
	struct PageRow { Id id; int ordinal; DWORD flags; Bytes name; Id version; };
	struct ItemRow { Id id; Id page; int x, y, w, h; Bytes data; Id version; Id pendingOp; int pendingKind; };
	struct OpRow { Id id; std::string character; OpState state; };
	// BUILD_ prefix: EXPORT is an object-like macro in the Windows SDK.
	enum BuildMode { BUILD_LIVE_LOAD, BUILD_CONVERT, BUILD_EXPORT };
	enum PendingView { VIEW_SETTLED = 1, VIEW_REVERT = 2 };
	struct PendingResolutionEntry { Id opId; PendingView view; };
	typedef std::vector<PendingResolutionEntry> PendingResolution;

	// Same algorithm and seed as fnv1a in src/plugy/Common.cpp, which the tests cannot link.
	DWORD Fnv1a(const BYTE* data, DWORD size, DWORD hash = 0x811C9DC5);

	// "Local\PlugY_SharedStash_%08X", byte for byte the name lockSharedSaveFile builds from a .sss path.
	void SharedStashMutexName(const char* sssPath, char* out);
	// "Global\PlugY_LiveStash_%08X" over the given string as is: the caller passes CanonicalDatabasePath's result.
	void LiveStashLockName(const char* canonicalLowerDbPath, char* out);

	// The classic .sss for these rows, as saveSharedSaveFile writes it: pages by ordinal, each page's items by
	// ascending id. LIVE_LOAD leaves out every pending row; CONVERT refuses any pending row; EXPORT resolves each by
	// its op's view. False (out and emittedIds left empty) on a refused input; emittedIds may be NULL.
	bool BuildClassicStash(const Meta& meta, const std::vector<PageRow>& pages, const std::vector<ItemRow>& items,
		BuildMode mode, const PendingResolution* resolution, Bytes* out, std::vector<Id>* emittedIds);

	// Tag, version, gold and page count only, as loadSharedSaveFile reads them; the page count is not checked
	// against the size.
	struct ClassicHeader { WORD version; DWORD gold; DWORD pageCount; DWORD pagesOffset; };
	bool ParseClassicHeader(const BYTE* data, DWORD size, ClassicHeader* out);

	// The marker that replaces the .sss while live mode is on: "SSS\0" "LV", format DWORD 1, the database guid,
	// then two CRLF lines of ASCII naming the database file.
	bool BuildMarker(const unsigned char dbGuid[16], const char* dbFileName, Bytes* out);
	enum MarkerKind { MARKER_NONE, MARKER_VALID, MARKER_DAMAGED };
	MarkerKind ParseMarker(const BYTE* data, DWORD size, const char* dbFileName, unsigned char guidOut[16]);

	// The whole file is the input: six bytes tell a classic file from a marker, never a valid marker from a
	// damaged one. A 0-byte file is ABSENT (master's reader handles it as missing); a classic header shorter than
	// master's 10-byte minimum is MARKER_DAMAGED, which every folder state refuses. UNREADABLE, and ABSENT for a
	// missing file, are the caller's.
	enum SssKind { SSS_ABSENT, SSS_CLASSIC, SSS_MARKER, SSS_MARKER_DAMAGED, SSS_FOREIGN, SSS_UNREADABLE };
	SssKind ClassifySssBytes(const BYTE* data, DWORD size, const char* dbFileName, unsigned char guidOut[16]);

	// DB_EMPTY: the database file exists and Store::open answers INVALID (a create that never committed).
	enum DbState { DB_ABSENT, DB_EMPTY, DB_IMPORTING, DB_LIVE, DB_CONVERTING_BACK, DB_BACKUP_COPY, DB_FOREIGN,
		DB_NEWER, DB_UNREADABLE };
	enum FolderVerdict { FOLDER_CLASSIC, FOLDER_LIVE, FOLDER_REFUSE, FOLDER_IMPORT };
	enum FolderAction { ACTION_NONE, ACTION_RETIRE_FAILED, ACTION_SET_MODE_LIVE, ACTION_RETIRE_RETIRED,
		ACTION_RENAME_ORPHANS };
	struct FolderInputs { SssKind sss; DbState db; bool guidMatches; bool orphanWalShm; bool requestExists;
		bool folderUsable; };
	// row: the folder state table's row number; 0 is the closed table's catch-all. Every REFUSE has ACTION_NONE.
	struct FolderDecision { FolderVerdict verdict; int row; FolderAction action; };
	// Pure: no I/O, no lock. Taking the locks and deciding again before acting is the caller's.
	FolderDecision LiveFolderDecide(const FolderInputs& in);
}
