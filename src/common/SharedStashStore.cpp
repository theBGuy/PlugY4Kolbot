/*=================================================================
	Live shared stash, the SQLite store. See SharedStashStore.h.
	The statement and transaction wrappers, the status mapping and
	the cells table come from xrip's shared stash store (GPL-3.0).
=================================================================*/

#include "SharedStashStore.h"
#include "../../third_party/sqlite/sqlite3.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <exception>

#define STRINGIZE_VALUE(x) #x
#define STRINGIZE(x) STRINGIZE_VALUE(x)

// Reads wait this long for another connection's WAL recovery or checkpoint; writes get the caller's budget instead.
#define READ_BUSY_MS 1000
// The shortest busy timeout a write runs with, whatever is left of the caller's budget.
#define MIN_BUSY_MS 50
#define MAX_BLOB_SIZE 8192

namespace SharedStash
{

// Every SQL string literal uses single quotes: SQLite is built with SQLITE_DQS=0.
static const char* SchemaSql =
	"PRAGMA application_id = " STRINGIZE(LIVE_APPLICATION_ID) ";"
	"PRAGMA user_version = " STRINGIZE(LIVE_USER_VERSION) ";"
	"CREATE TABLE meta("
	" singleton INTEGER PRIMARY KEY CHECK(singleton = 1),"
	" db_guid BLOB NOT NULL CHECK(length(db_guid) = 16),"
	" mode INTEGER NOT NULL CHECK(mode IN (0, 1, 2, 3)),"
	" grid_w INTEGER NOT NULL CHECK(grid_w BETWEEN 1 AND 15),"
	" grid_h INTEGER NOT NULL CHECK(grid_h BETWEEN 1 AND 15),"
	" gold INTEGER NOT NULL CHECK(gold BETWEEN 0 AND 4294967295),"
	" revision INTEGER NOT NULL CHECK(revision >= 0),"
	" sss_version INTEGER NOT NULL CHECK(sss_version IN (0, 12592, 12848)),"
	" created_utc INTEGER NOT NULL);"
	"CREATE TABLE pages("
	" id INTEGER PRIMARY KEY AUTOINCREMENT,"
	" ordinal INTEGER NOT NULL UNIQUE CHECK(ordinal >= 0),"
	" flags INTEGER NOT NULL CHECK(flags BETWEEN 0 AND 4294967295),"
	" name BLOB NOT NULL CHECK(length(name) <= 20),"
	" version INTEGER NOT NULL CHECK(version > 0));"
	"CREATE TABLE ops("
	" id INTEGER PRIMARY KEY AUTOINCREMENT,"
	" character TEXT NOT NULL COLLATE NOCASE CHECK(length(character) BETWEEN 1 AND 15),"
	" state INTEGER NOT NULL CHECK(state IN (0, 1, 2)),"
	" gold_delta INTEGER NOT NULL DEFAULT 0 CHECK(gold_delta = 0),"
	" created_utc INTEGER NOT NULL,"
	" resolved_utc INTEGER);"
	"CREATE TABLE items("
	" id INTEGER PRIMARY KEY AUTOINCREMENT,"
	" page INTEGER NOT NULL REFERENCES pages(id),"
	" x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL,"
	" data BLOB NOT NULL CHECK(length(data) BETWEEN 1 AND 8192),"
	" version INTEGER NOT NULL CHECK(version > 0),"
	" pending_op INTEGER REFERENCES ops(id),"
	" pending_kind INTEGER CHECK(pending_kind IN (1, 2)),"
	" CHECK(x >= 0 AND y >= 0 AND w >= 1 AND h >= 1 AND x + w <= 15 AND y + h <= 15),"
	" CHECK((pending_op IS NULL) = (pending_kind IS NULL)));"
	"CREATE TABLE cells("
	" page INTEGER NOT NULL REFERENCES pages(id),"
	" x INTEGER NOT NULL, y INTEGER NOT NULL,"
	" item INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,"
	" PRIMARY KEY(page, x, y)) WITHOUT ROWID;"
	"CREATE INDEX items_page ON items(page);"
	"CREATE INDEX items_pending ON items(pending_op) WHERE pending_op IS NOT NULL;"
	"CREATE INDEX cells_item ON cells(item);"
	"CREATE INDEX ops_character ON ops(character, state);";

// --- Log ------------------------------------------------------------------------------------------------------------

static LogSink logSink = NULL;

void SetLogSink(LogSink sink)
{
	logSink = sink;
}

static void logLine(const char* format, ...)
{
	if (!logSink)
		return;
	char line[256];
	va_list args;
	va_start(args, format);
	_vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	line[sizeof(line) - 1] = 0;
	logSink(line);
}

// --- Status mapping, statements, transactions -----------------------------------------------------------------------

// The text is copied at the throw (SQLite's message changes as the unwinding rolls back), into a fixed buffer so
// that neither the throw nor the catch can allocate. code: SQLite's extended result code, 0 for our own refusals.
struct Failure
{
	Status status;
	int code;
	char text[SHARED_STASH_ERROR_SIZE];

	Failure(Status s, const char* t, int c = 0) : status(s), code(c)
	{
		strncpy(text, t ? t : "", sizeof(text) - 1);
		text[sizeof(text) - 1] = 0;
	}
};

static void require(bool ok, Status status, const char* text)
{
	if (!ok)
		throw Failure(status, text);
}

// OK, ROW and DONE pass; BUSY and LOCKED (any extended code) are BUSY; anything else is STORE_ERROR.
static void sqlCheck(sqlite3* db, int rc)
{
	if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE)
		return;
	int primary = rc & 255;
	throw Failure(primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? BUSY : STORE_ERROR,
		db ? sqlite3_errmsg(db) : sqlite3_errstr(rc), db ? sqlite3_extended_errcode(db) : rc);
}

static void exec(sqlite3* db, const char* sql)
{
	sqlCheck(db, sqlite3_exec(db, sql, NULL, NULL, NULL));
}

class Query
{
public:
	Query(sqlite3* db, const char* sql) : db_(db), stmt_(NULL)
	{
		sqlCheck(db_, sqlite3_prepare_v2(db_, sql, -1, &stmt_, NULL));
	}

	~Query()
	{
		sqlite3_finalize(stmt_);
	}

	Query& number(int n, Id value)
	{
		sqlCheck(db_, sqlite3_bind_int64(stmt_, n, value));
		return *this;
	}

	Query& null(int n)
	{
		sqlCheck(db_, sqlite3_bind_null(stmt_, n));
		return *this;
	}

	Query& text(int n, const char* value)
	{
		sqlCheck(db_, sqlite3_bind_text(stmt_, n, value, -1, SQLITE_TRANSIENT));
		return *this;
	}

	// An empty blob binds as a zero-length blob: a NULL pointer would bind SQL NULL and fail NOT NULL.
	Query& blob(int n, const Bytes& value)
	{
		require(value.size() <= INT_MAX, INVALID, "blob too large");
		sqlCheck(db_, sqlite3_bind_blob(stmt_, n, value.empty() ? "" : (const void*)&value[0], (int)value.size(),
			SQLITE_TRANSIENT));
		return *this;
	}

	Query& blob(int n, const unsigned char* data, int size)
	{
		sqlCheck(db_, sqlite3_bind_blob(stmt_, n, size ? (const void*)data : "", size, SQLITE_TRANSIENT));
		return *this;
	}

	bool row()
	{
		int rc = sqlite3_step(stmt_);
		sqlCheck(db_, rc);
		return rc == SQLITE_ROW;
	}

	void reset()
	{
		sqlite3_reset(stmt_);
		sqlite3_clear_bindings(stmt_);
	}

	Id integer(int n)
	{
		return sqlite3_column_int64(stmt_, n);
	}

	std::string string(int n)
	{
		const char* p = (const char*)sqlite3_column_text(stmt_, n);
		return p ? std::string(p, sqlite3_column_bytes(stmt_, n)) : std::string();
	}

	// A NULL pointer with 0 bytes is an empty blob.
	Bytes bytes(int n)
	{
		const unsigned char* p = (const unsigned char*)sqlite3_column_blob(stmt_, n);
		int count = sqlite3_column_bytes(stmt_, n);
		return p && count ? Bytes(p, p + count) : Bytes();
	}

private:
	Query(const Query&);
	Query& operator=(const Query&);
	sqlite3* db_;
	sqlite3_stmt* stmt_;
};

// Rolls back on every exit path that did not commit.
class Transaction
{
public:
	Transaction(sqlite3* db, bool write) : db_(db), active_(false)
	{
		exec(db_, write ? "BEGIN IMMEDIATE" : "BEGIN");
		active_ = true;
	}

	~Transaction()
	{
		if (active_)
			sqlite3_exec(db_, "ROLLBACK", NULL, NULL, NULL);
	}

	void commit()
	{
		exec(db_, "COMMIT");
		active_ = false;
	}

private:
	Transaction(const Transaction&);
	Transaction& operator=(const Transaction&);
	sqlite3* db_;
	bool active_;
};

static Id scalar(sqlite3* db, const char* sql)
{
	Query q(db, sql);
	require(q.row(), STORE_ERROR, "query returned no row");
	return q.integer(0);
}

#define CATCH_STATUS \
	catch (const Failure& failure) \
	{ \
		setError(failure.text); \
		return failure.status; \
	} \
	catch (const std::exception& failure) \
	{ \
		setError(failure.what()); \
		return STORE_ERROR; \
	} \
	catch (...) \
	{ \
		setError("unexpected failure"); \
		return STORE_ERROR; \
	}

// --- Paths ----------------------------------------------------------------------------------------------------------

bool SqlitePath(const char* ansiPath, std::string* utf8)
{
	try
	{
		utf8->clear();
		if (!ansiPath)
			return false;
		// A byte sequence the code page cannot decode is refused, never replaced: SQLite and the ...A file calls must
		// name the same file.
		int wideLen = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, ansiPath, -1, NULL, 0);
		if (!wideLen)
			return false;
		std::vector<wchar_t> wide(wideLen);
		if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, ansiPath, -1, &wide[0], wideLen))
			return false;
		int len = WideCharToMultiByte(CP_UTF8, 0, &wide[0], -1, NULL, 0, NULL, NULL);
		if (!len)
			return false;
		std::vector<char> out(len);
		if (!WideCharToMultiByte(CP_UTF8, 0, &wide[0], -1, &out[0], len, NULL, NULL))
			return false;
		utf8->assign(&out[0]);
		return true;
	}
	catch (...)
	{
		utf8->clear();
		return false;
	}
}

static bool hasDrivePrefix(const char* p)
{
	return p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\' && p[4] && p[5] == ':' && p[6] == '\\';
}

enum Resolve { RESOLVE_OK, RESOLVE_BAD_PATH, RESOLVE_NO_FOLDER };

// The one folder rule of CanonicalDatabasePath and LocalDatabasePath: the full path split at its last separator, the
// folder opened and its final path read back without "\\?\" (a network one as "\\server\share..."), then joined to
// the file name as given. A drive root keeps its separator ("C:" alone names drive C's current directory) and no
// separator is doubled.
static Resolve resolveDatabaseFolder(const char* ansiPath, char* folder, char* joined, bool* unc)
{
	// Every failure leaves its own code in GetLastError: the lock-unavailable log line reports it.
	char full[MAX_PATH];
	DWORD n = GetFullPathNameA(ansiPath, MAX_PATH, full, NULL);
	if (!n)
		return RESOLVE_BAD_PATH;
	if (n >= MAX_PATH)
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return RESOLVE_BAD_PATH;
	}
	char* slash = strrchr(full, '\\');
	if (!slash || !slash[1])
	{
		SetLastError(ERROR_BAD_PATHNAME);
		return RESOLVE_BAD_PATH;
	}
	char fileName[MAX_PATH];
	strcpy(fileName, slash + 1);
	bool root = slash > full && slash[-1] == ':' && (slash == full + 2 || (slash == full + 6 && hasDrivePrefix(full)));
	if (root)
		slash[1] = 0;
	else
		*slash = 0;

	HANDLE dir = CreateFileA(full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (dir == INVALID_HANDLE_VALUE)
		return RESOLVE_NO_FOLDER;
	char finalPath[MAX_PATH];
	n = GetFinalPathNameByHandleA(dir, finalPath, MAX_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	DWORD error = GetLastError();
	CloseHandle(dir);
	if (!n || n >= MAX_PATH)
	{
		SetLastError(n ? ERROR_FILENAME_EXCED_RANGE : error);
		return RESOLVE_NO_FOLDER;
	}
	*unc = !_strnicmp(finalPath, "\\\\?\\UNC\\", 8);
	if (*unc)
	{
		strcpy(folder, "\\\\");
		strcat(folder, finalPath + 8);
	}
	else if (!strncmp(finalPath, "\\\\?\\", 4))
		strcpy(folder, finalPath + 4);
	else
		strcpy(folder, finalPath);

	size_t len = strlen(folder);
	bool separated = len && folder[len - 1] == '\\';
	if (len + (separated ? 0 : 1) + strlen(fileName) >= MAX_PATH)
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return RESOLVE_BAD_PATH;
	}
	strcpy(joined, folder);
	if (!separated)
		strcat(joined, "\\");
	strcat(joined, fileName);
	return RESOLVE_OK;
}

bool CanonicalDatabasePath(const char* ansiPath, char* out)
{
	if (!ansiPath || !*ansiPath || !out)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	char folder[MAX_PATH];
	char joined[MAX_PATH];
	bool unc = false;
	if (resolveDatabaseFolder(ansiPath, folder, joined, &unc) != RESOLVE_OK)
		return false;
	_strlwr(joined);
	strcpy(out, joined);
	return true;
}

LocalPath LocalDatabasePath(const char* ansiPath, char* canonical)
{
	if (canonical)
		canonical[0] = 0;
	if (!ansiPath || !*ansiPath)
		return LOCAL_ERROR;
	char full[MAX_PATH];
	DWORD n = GetFullPathNameA(ansiPath, MAX_PATH, full, NULL);
	if (!n || n >= MAX_PATH)
		return LOCAL_ERROR;
	if (full[0] == '\\' && full[1] == '\\' && !hasDrivePrefix(full))
		return LOCAL_UNC;

	char folder[MAX_PATH];
	char joined[MAX_PATH];
	bool unc = false;
	if (resolveDatabaseFolder(ansiPath, folder, joined, &unc) != RESOLVE_OK)
		return LOCAL_ERROR;
	if (canonical)
		strcpy(canonical, joined);
	if (unc)
		return LOCAL_UNC;
	char volume[MAX_PATH];
	if (!GetVolumePathNameA(folder, volume, MAX_PATH))
		return LOCAL_ERROR;
	UINT type = GetDriveTypeA(volume);
	if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK)
		return LOCAL_NOT_LOCAL_DRIVE;

	DWORD attributes = GetFileAttributesA(joined);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? LOCAL_OK : LOCAL_ERROR;
	if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		return LOCAL_REPARSE;
	HANDLE file = CreateFileA(joined, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return LOCAL_ERROR;
	LocalPath result = LOCAL_OK;
	BY_HANDLE_FILE_INFORMATION info;
	if (GetFileType(file) != FILE_TYPE_DISK)
		result = LOCAL_REPARSE;
	else if (!GetFileInformationByHandle(file, &info))
		result = LOCAL_ERROR;
	else if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		result = LOCAL_REPARSE;
	else if (info.nNumberOfLinks != 1)
		result = LOCAL_HARD_LINK;
	CloseHandle(file);
	return result;
}

// --- Write lock -----------------------------------------------------------------------------------------------------

struct LockHold
{
	HANDLE mutex;
	bool owned;

	LockHold() : mutex(NULL), owned(false)
	{
	}

	~LockHold()
	{
		release();
	}

	void release()
	{
		if (owned)
			ReleaseMutex(mutex);
		owned = false;
	}
};

enum LockWait { LOCK_OWNED, LOCK_TIMEOUT, LOCK_FAILED };

static bool lockNameOf(const char* ansiPath, char* name, DWORD* error)
{
	char canonical[MAX_PATH];
	if (!CanonicalDatabasePath(ansiPath, canonical))
	{
		*error = GetLastError();
		return false;
	}
	LiveStashLockName(canonical, name);
	return true;
}

// Creates or opens the named mutex with a DACL that grants only the current user. NULL (with *error) when the lock
// is unavailable: an existing object we may not open (another user's, or an elevated process's) included.
static HANDLE openWriteMutex(const char* name, DWORD* error)
{
	HANDLE token = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		*error = GetLastError();
		return NULL;
	}
	DWORD userBuffer[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD) + 1];
	DWORD size = sizeof(userBuffer);
	BOOL gotUser = GetTokenInformation(token, TokenUser, userBuffer, size, &size);
	*error = gotUser ? 0 : GetLastError();
	CloseHandle(token);
	if (!gotUser)
		return NULL;
	PSID sid = ((TOKEN_USER*)userBuffer)->User.Sid;

	DWORD aclBuffer[(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD) + 1];
	ACL* acl = (ACL*)aclBuffer;
	SECURITY_DESCRIPTOR descriptor;
	if (!InitializeAcl(acl, sizeof(aclBuffer), ACL_REVISION)
		|| !AddAccessAllowedAce(acl, ACL_REVISION, MUTEX_ALL_ACCESS, sid)
		|| !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE))
	{
		*error = GetLastError();
		return NULL;
	}
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &descriptor, FALSE};
	// Only the rights a wait and a release need: an existing object's integrity label can refuse WRITE_DAC,
	// WRITE_OWNER or DELETE (all in MUTEX_ALL_ACCESS) where our own DACL would grant them.
	HANDLE mutex = CreateMutexExA(&attributes, name, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
	if (!mutex)
		*error = GetLastError();
	return mutex;
}

// WAIT_ABANDONED counts as acquired: SQLite rolls the dead writer's open transaction back itself.
static LockWait waitWriteMutex(HANDLE mutex, DWORD waitMs, DWORD* error)
{
	DWORD wait = WaitForSingleObject(mutex, waitMs);
	if (wait == WAIT_OBJECT_0)
		return LOCK_OWNED;
	if (wait == WAIT_ABANDONED)
	{
		logLine("live stash: write lock abandoned by a process that ended; taken over");
		return LOCK_OWNED;
	}
	if (wait == WAIT_TIMEOUT)
		return LOCK_TIMEOUT;
	*error = GetLastError();
	return LOCK_FAILED;
}

static void logLockUnavailable(DWORD error)
{
	logLine("live stash: write lock unavailable (error %lu); writes wait on the database file lock only", error);
}

static void logWaitFailed(DWORD error)
{
	logLine("live stash: write lock wait failed (error %lu); this write waits on the database file lock only", error);
}

static ULONGLONG nowMs()
{
	LARGE_INTEGER count;
	LARGE_INTEGER frequency;
	QueryPerformanceCounter(&count);
	QueryPerformanceFrequency(&frequency);
	return (ULONGLONG)(count.QuadPart / frequency.QuadPart) * 1000
		+ (ULONGLONG)(count.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart;
}

// What is left of waitMs after spentMs, at least MIN_BUSY_MS, as sqlite3_busy_timeout takes it.
static int busyMsOf(DWORD waitMs, ULONGLONG spentMs)
{
	ULONGLONG left = waitMs > spentMs ? waitMs - spentMs : 0;
	if (left < MIN_BUSY_MS)
		left = MIN_BUSY_MS;
	return left > INT_MAX ? INT_MAX : (int)left;
}

WriteLock::WriteLock() : mutex_(NULL), owned_(false), acquired_(false)
{
}

WriteLock::~WriteLock()
{
	release();
}

Status WriteLock::acquire(const char* ansiDbPath, DWORD waitMs)
{
	if (!ansiDbPath || !*ansiDbPath || acquired_)
		return INVALID;
	char name[SHARED_STASH_MUTEX_NAME_SIZE];
	DWORD error = 0;
	if (lockNameOf(ansiDbPath, name, &error))
		mutex_ = openWriteMutex(name, &error);
	if (!mutex_)
	{
		logLockUnavailable(error);
		acquired_ = true;
		return OK;
	}
	LockWait wait = waitWriteMutex(mutex_, waitMs, &error);
	if (wait == LOCK_TIMEOUT)
	{
		CloseHandle(mutex_);
		mutex_ = NULL;
		return BUSY;
	}
	acquired_ = true;
	if (wait == LOCK_FAILED)
	{
		logWaitFailed(error);
		CloseHandle(mutex_);
		mutex_ = NULL;
		return OK;
	}
	owned_ = true;
	return OK;
}

void WriteLock::release()
{
	if (owned_)
		ReleaseMutex(mutex_);
	if (mutex_)
		CloseHandle(mutex_);
	mutex_ = NULL;
	owned_ = false;
	acquired_ = false;
}

bool WriteLock::owned() const
{
	return owned_;
}

// --- Store helpers --------------------------------------------------------------------------------------------------

static Id nowUtc()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULONGLONG ticks = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return (Id)((ticks - 116444736000000000ULL) / 10000000ULL);
}

static void requireCharacter(const char* character)
{
	require(character && *character && strlen(character) <= 15, INVALID, "character name must be 1 to 15 bytes");
}

static void readMetaRow(sqlite3* db, Meta* out)
{
	Query q(db, "SELECT db_guid, mode, grid_w, grid_h, gold, revision, sss_version FROM meta WHERE singleton = 1");
	require(q.row(), STORE_ERROR, "the meta row is missing");
	Bytes guid = q.bytes(0);
	require(guid.size() == 16, STORE_ERROR, "the database guid is not 16 bytes");
	memcpy(out->dbGuid, &guid[0], 16);
	out->mode = (Mode)q.integer(1);
	out->gridW = (int)q.integer(2);
	out->gridH = (int)q.integer(3);
	out->gold = (DWORD)q.integer(4);
	out->revision = q.integer(5);
	out->sssVersion = (WORD)q.integer(6);
}

static Mode currentMode(sqlite3* db)
{
	return (Mode)scalar(db, "SELECT mode FROM meta WHERE singleton = 1");
}

static void bumpRevision(sqlite3* db)
{
	exec(db, "UPDATE meta SET revision = revision + 1 WHERE singleton = 1");
}

// The w*h cells of an item's rectangle, in the same transaction as its row; a taken cell fails the primary key.
static void insertCells(Query& cell, Id page, int x, int y, int w, int h, Id item)
{
	for (int cy = y; cy < y + h; cy++)
		for (int cx = x; cx < x + w; cx++)
		{
			cell.reset();
			cell.number(1, page).number(2, cx).number(3, cy).number(4, item).row();
		}
}

static bool insideGrid(const ItemRow& row, int gridW, int gridH)
{
	return row.x >= 0 && row.y >= 0 && row.w >= 1 && row.h >= 1 && row.x <= gridW - row.w && row.y <= gridH - row.h;
}

// Open pragmas, in this order; journal_mode=WAL cannot run inside a transaction and is skipped on a read-only open.
static void applyOpenPragmas(sqlite3* db, bool readOnly)
{
	exec(db, "PRAGMA foreign_keys=ON");
	exec(db, "PRAGMA trusted_schema=OFF");
	if (!readOnly)
	{
		Query wal(db, "PRAGMA journal_mode=WAL");
		require(wal.row() && wal.string(0) == "wal", STORE_ERROR, "the database cannot use WAL journaling");
	}
	exec(db, "PRAGMA synchronous=FULL");
	exec(db, "PRAGMA journal_size_limit=4194304");
}

// The open gate, in a read transaction and before any pragma that could write. A file that is not an SQLite
// database at all counts as made by another program, and so does one whose hot rollback journal a read-only
// connection may not play back: our databases are WAL files and never leave one.
static void openGate(sqlite3* db)
{
	Transaction tx(db, false);
	Id application = 0;
	Id version = 0;
	Id tables = 0;
	try
	{
		application = scalar(db, "PRAGMA application_id");
		version = scalar(db, "PRAGMA user_version");
		tables = scalar(db, "SELECT count(*) FROM sqlite_master");
	}
	catch (const Failure& failure)
	{
		if ((failure.code & 255) == SQLITE_NOTADB)
			throw Failure(FOREIGN, "the file is not a live shared stash database");
		if (failure.code == SQLITE_READONLY_ROLLBACK)
			throw Failure(FOREIGN, "the file holds an unfinished transaction of another program");
		throw;
	}
	require(application != 0 || version != 0 || tables != 0, INVALID,
		"the database file is empty: its creation never finished");
	require(application == LIVE_APPLICATION_ID, FOREIGN, "the file is not a live shared stash database");
	require(version <= LIVE_USER_VERSION, NEWER, "the live shared stash was made by a newer PlugY");
	require(version == LIVE_USER_VERSION, FOREIGN, "the file is not a live shared stash database");
	tx.commit();
}

static bool fileExists(const std::string& path)
{
	return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// --- Store ----------------------------------------------------------------------------------------------------------

Store::Store() : db_(NULL), readOnly_(false), mutex_(NULL), lockUnavailable_(false), waitFailedLogged_(false),
	failCreateStep_(0)
{
	error_[0] = 0;
	lockNameOverride_[0] = 0;
}

void Store::setError(const char* text)
{
	strncpy(error_, text ? text : "", sizeof(error_) - 1);
	error_[sizeof(error_) - 1] = 0;
}

Store::~Store()
{
	close();
}

const char* Store::error() const
{
	return error_;
}

void Store::close()
{
	if (db_)
	{
		if (readOnly_)
			sqlite3_db_config(db_, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, (int*)NULL);
		else
			sqlite3_wal_checkpoint_v2(db_, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
		sqlite3_close_v2(db_);
		db_ = NULL;
	}
	if (mutex_)
		CloseHandle(mutex_);
	mutex_ = NULL;
	lockUnavailable_ = false;
	waitFailedLogged_ = false;
	readOnly_ = false;
}

// Once per open: the computed (or test) name, then the mutex. Unavailable: every write of this open runs under
// SQLite's busy handler alone; correctness is SQLite's file lock, only fairness is lost.
void Store::resolveLock(const char* ansiPath)
{
	char name[SHARED_STASH_MUTEX_NAME_SIZE];
	DWORD error = 0;
	bool named = true;
	if (lockNameOverride_[0])
		strcpy(name, lockNameOverride_);
	else
		named = lockNameOf(ansiPath, name, &error);
	if (named)
		mutex_ = openWriteMutex(name, &error);
	if (!mutex_)
	{
		lockUnavailable_ = true;
		logLockUnavailable(error);
	}
}

// Takes the write lock for one call and returns the busy timeout the call runs with. A timeout throws BUSY with
// nothing begun; a failed wait runs this call alone without the mutex.
DWORD Store::beginLockedWork(DWORD lockWaitMs, LockHold* hold)
{
	if (lockUnavailable_ || !mutex_)
		return busyMsOf(lockWaitMs, 0);
	ULONGLONG start = nowMs();
	DWORD error = 0;
	LockWait wait = waitWriteMutex(mutex_, lockWaitMs, &error);
	if (wait == LOCK_TIMEOUT)
		throw Failure(BUSY, "the database write lock is held by another game");
	if (wait == LOCK_FAILED)
	{
		if (!waitFailedLogged_)
			logWaitFailed(error);
		waitFailedLogged_ = true;
		return busyMsOf(lockWaitMs, 0);
	}
	hold->mutex = mutex_;
	hold->owned = true;
	return busyMsOf(lockWaitMs, nowMs() - start);
}

#define REQUIRE_OPEN() require(db_ != NULL, INVALID, "the database is not open")
#define REQUIRE_WRITABLE() \
	REQUIRE_OPEN(); \
	require(!readOnly_, INVALID, "the database is open read-only")

Status Store::create(const char* ansiPath, const Meta& meta, DWORD lockWaitMs)
{
	close();
	error_[0] = 0;
	// The armed test failure is consumed only by a create() that reaches step 3.
	int failStep = failCreateStep_;
	LockHold hold;
	bool ownedWhole = false;
	bool provedAbsent = false;
	bool notEmpty = false;
	int step = 0;
	Status status = OK;
	try
	{
		require(ansiPath && *ansiPath, INVALID, "no database path");
		require(meta.gridW >= 1 && meta.gridW <= 15 && meta.gridH >= 1 && meta.gridH <= 15, INVALID,
			"the stash grid must be 1 to 15 cells each way");
		require(meta.sssVersion == 0 || meta.sssVersion == 0x3130 || meta.sssVersion == 0x3230, INVALID,
			"unknown classic stash version");
		std::string utf8;
		require(SqlitePath(ansiPath, &utf8), INVALID, "the database path cannot be converted to UTF-8");

		step = 1;
		ULONGLONG start = nowMs();
		resolveLock(ansiPath);
		beginLockedWork(lockWaitMs, &hold);
		ownedWhole = hold.owned;
		int busyMs = ownedWhole ? busyMsOf(lockWaitMs, nowMs() - start) : busyMsOf(lockWaitMs, 0);

		step = 2;
		static const char* suffixes[] = {"", "-wal", "-shm", "-journal"};
		for (int i = 0; i < 4; i++)
		{
			std::string name = std::string(ansiPath) + suffixes[i];
			if (GetFileAttributesA(name.c_str()) != INVALID_FILE_ATTRIBUTES)
				throw Failure(INVALID, "the database file, or a -wal, -shm or -journal file of it, already exists");
			require(GetLastError() == ERROR_FILE_NOT_FOUND, STORE_ERROR, "cannot look up the database file");
		}
		provedAbsent = true;

		step = 3;
		failCreateStep_ = 0;
		int rc = sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
			NULL);
		sqlCheck(db_, rc);
		sqlite3_busy_timeout(db_, busyMs);
		require(failStep != 3, STORE_ERROR, "test failure");

		step = 4;
		applyOpenPragmas(db_, false);
		require(failStep != 4, STORE_ERROR, "test failure");

		step = 5;
		Transaction tx(db_, true);
		if (scalar(db_, "PRAGMA application_id") != 0 || scalar(db_, "PRAGMA user_version") != 0
			|| scalar(db_, "SELECT count(*) FROM sqlite_master") != 0)
		{
			notEmpty = true;
			throw Failure(INVALID, "database not empty");
		}
		exec(db_, SchemaSql);
		Query insert(db_, "INSERT INTO meta(singleton, db_guid, mode, grid_w, grid_h, gold, revision, sss_version,"
			" created_utc) VALUES(1, ?1, 0, ?2, ?3, 0, 0, ?4, ?5)");
		insert.blob(1, meta.dbGuid, 16).number(2, meta.gridW).number(3, meta.gridH).number(4, meta.sssVersion)
			.number(5, nowUtc()).row();
		require(failStep != 5, STORE_ERROR, "test failure");
		tx.commit();
		readOnly_ = false;
		return OK;
	}
	catch (const Failure& failure)
	{
		status = failure.status;
		setError(failure.text);
	}
	catch (const std::exception& failure)
	{
		status = STORE_ERROR;
		setError(failure.what());
	}
	catch (...)
	{
		status = STORE_ERROR;
		setError("unexpected failure");
	}
	// Only files this call created, and only while the mutex keeps every other create() of this path out: a file
	// another connection still holds fails DeleteFileA with a sharing violation, which keeps best effort safe.
	bool cleanup = step >= 3 && ownedWhole && provedAbsent && !notEmpty;
	if (db_)
	{
		// A file this call may not have created keeps its bytes: no close-time checkpoint into it.
		if (!cleanup)
			sqlite3_db_config(db_, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, (int*)NULL);
		sqlite3_close_v2(db_);
		db_ = NULL;
	}
	size_t pathLen = cleanup ? strlen(ansiPath) : 0;
	if (cleanup && pathLen < MAX_PATH)
	{
		static const char* suffixes[] = {"-wal", "-shm", "-journal", ""};
		char name[MAX_PATH + 16];
		for (int i = 0; i < 4; i++)
		{
			strcpy(name, ansiPath);
			strcat(name, suffixes[i]);
			DeleteFileA(name);
		}
	}
	hold.release();
	close();
	return status;
}

Status Store::open(const char* ansiPath, bool readOnly)
{
	close();
	error_[0] = 0;
	Status status = OK;
	try
	{
		require(ansiPath && *ansiPath, INVALID, "no database path");
		std::string utf8;
		require(SqlitePath(ansiPath, &utf8), INVALID, "the database path cannot be converted to UTF-8");
		if (GetFileAttributesA(ansiPath) == INVALID_FILE_ATTRIBUTES)
		{
			DWORD error = GetLastError();
			// A missing folder is a missing file here (create() step 2 answers STORE_ERROR for it instead).
			require(error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND, NOT_FOUND,
				"the database file does not exist");
			throw Failure(STORE_ERROR, "cannot look up the database file");
		}
		// A read-write connection plays a hot rollback journal back into the main file at its first read, before the
		// gate could refuse the file: with a -journal beside it, the gate runs first on a read-only connection.
		if (!readOnly && fileExists(std::string(ansiPath) + "-journal"))
		{
			sqlite3* probe = NULL;
			int probeRc = sqlite3_open_v2(utf8.c_str(), &probe, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
			try
			{
				sqlCheck(probe, probeRc);
				sqlite3_busy_timeout(probe, READ_BUSY_MS);
				openGate(probe);
			}
			catch (...)
			{
				sqlite3_db_config(probe, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, (int*)NULL);
				sqlite3_close_v2(probe);
				throw;
			}
			sqlite3_db_config(probe, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, (int*)NULL);
			sqlite3_close_v2(probe);
		}
		int flags = (readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_FULLMUTEX;
		int rc = sqlite3_open_v2(utf8.c_str(), &db_, flags, NULL);
		sqlCheck(db_, rc);
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		// SQLite reopens a file it may not write read-only without saying so.
		require(readOnly || sqlite3_db_readonly(db_, "main") != 1, STORE_ERROR, "the database file is read-only");
		openGate(db_);
		applyOpenPragmas(db_, readOnly);
		readOnly_ = readOnly;
		if (!readOnly)
			resolveLock(ansiPath);
		return OK;
	}
	catch (const Failure& failure)
	{
		status = failure.status;
		setError(failure.text);
	}
	catch (const std::exception& failure)
	{
		status = STORE_ERROR;
		setError(failure.what());
	}
	catch (...)
	{
		status = STORE_ERROR;
		setError("unexpected failure");
	}
	// A refused file's bytes never change: no close-time checkpoint of a -wal that was already there.
	if (db_)
	{
		sqlite3_db_config(db_, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, (int*)NULL);
		sqlite3_close_v2(db_);
		db_ = NULL;
	}
	close();
	return status;
}

Status Store::readMeta(Meta* out)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		require(out != NULL, INVALID, "no output");
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		Meta meta;
		readMetaRow(db_, &meta);
		tx.commit();
		*out = meta;
		return OK;
	}
	CATCH_STATUS
}

Status Store::readAll(Meta* meta, std::vector<PageRow>* pages, std::vector<ItemRow>* items,
	std::vector<OpRow>* pendingOps, Id* maxOpId)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		require(pages && items && pendingOps, INVALID, "no output");
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		Meta metaRow;
		readMetaRow(db_, &metaRow);
		std::vector<PageRow> pageRows;
		Query pageQuery(db_, "SELECT id, ordinal, flags, name, version FROM pages ORDER BY ordinal");
		while (pageQuery.row())
		{
			PageRow page;
			page.id = pageQuery.integer(0);
			page.ordinal = (int)pageQuery.integer(1);
			page.flags = (DWORD)pageQuery.integer(2);
			page.name = pageQuery.bytes(3);
			page.version = pageQuery.integer(4);
			pageRows.push_back(page);
		}
		std::vector<ItemRow> itemRows;
		Query itemQuery(db_, "SELECT id, page, x, y, w, h, data, version, pending_op, pending_kind FROM items"
			" ORDER BY id");
		while (itemQuery.row())
		{
			ItemRow item;
			item.id = itemQuery.integer(0);
			item.page = itemQuery.integer(1);
			item.x = (int)itemQuery.integer(2);
			item.y = (int)itemQuery.integer(3);
			item.w = (int)itemQuery.integer(4);
			item.h = (int)itemQuery.integer(5);
			item.data = itemQuery.bytes(6);
			item.version = itemQuery.integer(7);
			item.pendingOp = itemQuery.integer(8);
			item.pendingKind = (int)itemQuery.integer(9);
			itemRows.push_back(item);
		}
		std::vector<OpRow> opRows;
		Query opQuery(db_, "SELECT id, character, state FROM ops WHERE state = 0 ORDER BY id");
		while (opQuery.row())
		{
			OpRow op;
			op.id = opQuery.integer(0);
			op.character = opQuery.string(1);
			op.state = (OpState)opQuery.integer(2);
			opRows.push_back(op);
		}
		Id maxId = scalar(db_, "SELECT COALESCE(MAX(id), 0) FROM ops");
		tx.commit();
		if (meta)
			*meta = metaRow;
		pages->swap(pageRows);
		items->swap(itemRows);
		pendingOps->swap(opRows);
		if (maxOpId)
			*maxOpId = maxId;
		return OK;
	}
	CATCH_STATUS
}

Status Store::importStash(const std::vector<PageRow>& pages, const std::vector<ItemRow>& items, DWORD gold,
	DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		std::vector<bool> ordinals(pages.size(), false);
		for (size_t i = 0; i < pages.size(); i++)
		{
			require(pages[i].id > 0, INVALID, "page ids must be positive");
			int ordinal = pages[i].ordinal;
			require(ordinal >= 0 && (size_t)ordinal < pages.size() && !ordinals[ordinal], INVALID,
				"page ordinals must be exactly 0 to n-1");
			ordinals[ordinal] = true;
		}
		for (size_t i = 0; i < items.size(); i++)
		{
			require(items[i].id > 0, INVALID, "item ids must be positive");
			require(items[i].pendingOp == 0 && items[i].pendingKind == PENDING_NONE, INVALID,
				"an imported item cannot carry a pending marker");
		}

		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		Meta meta;
		readMetaRow(db_, &meta);
		require(meta.mode == MODE_IMPORTING && scalar(db_, "SELECT count(*) FROM pages") == 0
			&& scalar(db_, "SELECT count(*) FROM items") == 0 && scalar(db_, "SELECT count(*) FROM ops") == 0,
			CONFLICT, "the stash was already imported");
		for (size_t i = 0; i < items.size(); i++)
			require(insideGrid(items[i], meta.gridW, meta.gridH), INVALID, "an item lies outside the stash grid");

		Query page(db_, "INSERT INTO pages(id, ordinal, flags, name, version) VALUES(?1, ?2, ?3, ?4, 1)");
		for (size_t i = 0; i < pages.size(); i++)
		{
			page.reset();
			page.number(1, pages[i].id).number(2, pages[i].ordinal).number(3, pages[i].flags).blob(4, pages[i].name)
				.row();
		}
		Query item(db_, "INSERT INTO items(id, page, x, y, w, h, data, version) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, 1)");
		Query cell(db_, "INSERT INTO cells(page, x, y, item) VALUES(?1, ?2, ?3, ?4)");
		for (size_t i = 0; i < items.size(); i++)
		{
			const ItemRow& row = items[i];
			item.reset();
			item.number(1, row.id).number(2, row.page).number(3, row.x).number(4, row.y).number(5, row.w)
				.number(6, row.h).blob(7, row.data).row();
			insertCells(cell, row.page, row.x, row.y, row.w, row.h, row.id);
		}
		Query update(db_, "UPDATE meta SET gold = ?1, revision = 1 WHERE singleton = 1");
		update.number(1, gold).row();
		tx.commit();
		return OK;
	}
	CATCH_STATUS
}

Status Store::setMode(Mode from, Mode to, DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		require(to == MODE_LIVE && (from == MODE_IMPORTING || from == MODE_CONVERTING_BACK), INVALID,
			"only an import or a conversion back can switch the stash to live mode");
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		require(currentMode(db_) == from, CONFLICT, "the stash is not in the expected mode");
		Query update(db_, "UPDATE meta SET mode = ?1 WHERE singleton = 1");
		update.number(1, to).row();
		tx.commit();
		return OK;
	}
	CATCH_STATUS
}

Status Store::beginConvertBack(std::vector<std::string>* pendingNames, DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		require(pendingNames != NULL, INVALID, "no output");
		pendingNames->clear();
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		Mode mode = currentMode(db_);
		require(mode == MODE_LIVE || mode == MODE_CONVERTING_BACK, CONFLICT, "the stash is not in live mode");
		std::vector<std::string> names;
		Query pending(db_, "SELECT DISTINCT character FROM ops WHERE state = 0 ORDER BY character");
		while (pending.row())
			names.push_back(pending.string(0));
		if (!names.empty())
		{
			pendingNames->swap(names);
			setError("characters have unfinished stash operations");
			return CONFLICT;
		}
		exec(db_, "UPDATE meta SET mode = 2 WHERE singleton = 1");
		tx.commit();
		return OK;
	}
	CATCH_STATUS
}

Status Store::settleOps(const char* character, const unsigned char dbGuid[16], const Id* ids, DWORD count,
	DWORD lockWaitMs, Id* firstBad)
{
	error_[0] = 0;
	if (firstBad)
		*firstBad = 0;
	try
	{
		REQUIRE_WRITABLE();
		requireCharacter(character);
		require(dbGuid && firstBad && (ids || !count), INVALID, "missing argument");
		if (!count)
			return OK;

		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		Meta meta;
		readMetaRow(db_, &meta);
		require(!memcmp(meta.dbGuid, dbGuid, 16), FOREIGN, "the ops belong to another live stash database");

		// One pass in the order given; the first bad id decides and the whole call rolls back.
		Query state(db_, "SELECT state, character = ?2 FROM ops WHERE id = ?1");
		for (DWORD i = 0; i < count; i++)
		{
			state.reset();
			state.number(1, ids[i]).text(2, character);
			if (!state.row())
			{
				*firstBad = ids[i];
				throw Failure(NOT_FOUND, "an op does not exist");
			}
			Id opState = state.integer(0);
			bool sameCharacter = state.integer(1) != 0;
			if (!sameCharacter || opState == OP_REVERTED || (meta.mode != MODE_LIVE && opState == OP_PENDING))
			{
				*firstBad = ids[i];
				throw Failure(CONFLICT, !sameCharacter ? "an op belongs to another character"
					: opState == OP_REVERTED ? "an op was reverted" : "an op is still pending outside live mode");
			}
		}
		state.reset();

		bool changed = false;
		if (meta.mode == MODE_LIVE)
		{
			Query deposited(db_, "UPDATE items SET pending_op = NULL, pending_kind = NULL, version = version + 1"
				" WHERE pending_op = ?1 AND pending_kind = 1");
			Query withdrawn(db_, "DELETE FROM items WHERE pending_op = ?1 AND pending_kind = 2");
			Query settle(db_, "UPDATE ops SET state = 1, resolved_utc = ?2 WHERE id = ?1 AND state = 0");
			Id now = nowUtc();
			for (DWORD i = 0; i < count; i++)
			{
				settle.reset();
				settle.number(1, ids[i]).number(2, now).row();
				if (sqlite3_changes(db_) == 0)
					continue;
				deposited.reset();
				deposited.number(1, ids[i]).row();
				withdrawn.reset();
				withdrawn.number(1, ids[i]).row();
				changed = true;
			}
		}
		if (changed)
			bumpRevision(db_);
		tx.commit();
		return OK;
	}
	CATCH_STATUS
}

Status Store::revertPendingOps(const char* character, DWORD lockWaitMs, DWORD* reverted)
{
	error_[0] = 0;
	if (reverted)
		*reverted = 0;
	try
	{
		REQUIRE_WRITABLE();
		requireCharacter(character);
		require(reverted != NULL, INVALID, "no output");
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		std::vector<Id> ops;
		Query pending(db_, "SELECT id FROM ops WHERE character = ?1 AND state = 0 ORDER BY id");
		pending.text(1, character);
		while (pending.row())
			ops.push_back(pending.integer(0));
		if (currentMode(db_) != MODE_LIVE)
		{
			require(ops.empty(), CONFLICT, "the character has pending ops outside live mode");
			tx.commit();
			return OK;
		}
		Query deposited(db_, "DELETE FROM items WHERE pending_op = ?1 AND pending_kind = 1");
		Query withdrawn(db_, "UPDATE items SET pending_op = NULL, pending_kind = NULL, version = version + 1"
			" WHERE pending_op = ?1 AND pending_kind = 2");
		Query revert(db_, "UPDATE ops SET state = 2, resolved_utc = ?2 WHERE id = ?1");
		Id now = nowUtc();
		for (size_t i = 0; i < ops.size(); i++)
		{
			deposited.reset();
			deposited.number(1, ops[i]).row();
			withdrawn.reset();
			withdrawn.number(1, ops[i]).row();
			revert.reset();
			revert.number(1, ops[i]).number(2, now).row();
		}
		if (!ops.empty())
			bumpRevision(db_);
		tx.commit();
		*reverted = (DWORD)ops.size();
		return OK;
	}
	CATCH_STATUS
}

Status Store::pendingCharacters(std::vector<std::string>* names)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		require(names != NULL, INVALID, "no output");
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		std::vector<std::string> result;
		Query pending(db_, "SELECT DISTINCT character FROM ops WHERE state = 0 ORDER BY character");
		while (pending.row())
			result.push_back(pending.string(0));
		tx.commit();
		names->swap(result);
		return OK;
	}
	CATCH_STATUS
}

Status Store::hasPendingOps(const char* character, bool* pending)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		requireCharacter(character);
		require(pending != NULL, INVALID, "no output");
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		Query q(db_, "SELECT EXISTS(SELECT 1 FROM ops WHERE character = ?1 AND state = 0)");
		q.text(1, character);
		require(q.row(), STORE_ERROR, "query returned no row");
		bool any = q.integer(0) != 0;
		q.reset();
		tx.commit();
		*pending = any;
		return OK;
	}
	CATCH_STATUS
}

Status Store::opStates(const Id* ids, DWORD count, std::vector<OpRow>* out)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		require(out != NULL && (ids || !count), INVALID, "missing argument");
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		std::vector<OpRow> rows;
		Query q(db_, "SELECT id, character, state FROM ops WHERE id = ?1");
		for (DWORD i = 0; i < count; i++)
		{
			q.reset();
			q.number(1, ids[i]);
			if (!q.row())
				continue;
			OpRow op;
			op.id = q.integer(0);
			op.character = q.string(1);
			op.state = (OpState)q.integer(2);
			rows.push_back(op);
		}
		q.reset();
		tx.commit();
		out->swap(rows);
		return OK;
	}
	CATCH_STATUS
}

Status Store::maxOpId(Id* out)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		require(out != NULL, INVALID, "no output");
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		Id maxId = scalar(db_, "SELECT COALESCE(MAX(id), 0) FROM ops");
		tx.commit();
		*out = maxId;
		return OK;
	}
	CATCH_STATUS
}

// VACUUM INTO a fresh temp file beside the target (a rollback-mode single file), stamp it BACKUP_COPY through a
// private connection that runs no journal_mode pragma, then move it over the target. No write lock: VACUUM INTO only
// reads the source.
Status Store::backupTo(const char* ansiPath)
{
	error_[0] = 0;
	char temp[MAX_PATH] = "";
	Status status = OK;
	try
	{
		REQUIRE_OPEN();
		require(ansiPath && *ansiPath, INVALID, "no backup path");
		std::string target(ansiPath);
		// A stale side file under the target's name would be read as the new copy's.
		require(!fileExists(target + "-journal") && !fileExists(target + "-wal") && !fileExists(target + "-shm"),
			INVALID, "a -journal, -wal or -shm file of the backup target exists");
		char full[MAX_PATH];
		char* filePart = NULL;
		DWORD n = GetFullPathNameA(ansiPath, MAX_PATH, full, &filePart);
		require(n && n < MAX_PATH && filePart, INVALID, "the backup path is not a file path");
		std::string folder(full, filePart - full);
		require(GetTempFileNameA(folder.c_str(), "pyb", 0, temp) != 0, STORE_ERROR,
			"cannot create a temporary file beside the backup target");
		std::string tempUtf8;
		require(SqlitePath(temp, &tempUtf8), STORE_ERROR, "the temporary path cannot be converted to UTF-8");

		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		{
			Query vacuum(db_, "VACUUM INTO ?1");
			vacuum.text(1, tempUtf8.c_str()).row();
		}

		sqlite3* copy = NULL;
		int rc = sqlite3_open_v2(tempUtf8.c_str(), &copy, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
		try
		{
			sqlCheck(copy, rc);
			sqlite3_busy_timeout(copy, READ_BUSY_MS);
			exec(copy, "UPDATE meta SET mode = 3 WHERE singleton = 1");
			require(sqlite3_changes(copy) == 1, STORE_ERROR, "the copy has no meta row");
		}
		catch (...)
		{
			sqlite3_close_v2(copy);
			throw;
		}
		sqlCheck(NULL, sqlite3_close_v2(copy));
		require(MoveFileExA(temp, ansiPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, STORE_ERROR,
			"cannot move the backup copy into place");
		return OK;
	}
	catch (const Failure& failure)
	{
		status = failure.status;
		setError(failure.text);
	}
	catch (const std::exception& failure)
	{
		status = STORE_ERROR;
		setError(failure.what());
	}
	catch (...)
	{
		status = STORE_ERROR;
		setError("unexpected failure");
	}
	if (temp[0])
	{
		char journal[MAX_PATH + 16];
		strcpy(journal, temp);
		strcat(journal, "-journal");
		DeleteFileA(temp);
		DeleteFileA(journal);
	}
	return status;
}

// No transaction: a checkpoint inside one answers SQLITE_LOCKED.
Status Store::checkpointTruncate(DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		int log = -1;
		int checkpointed = -1;
		sqlCheck(db_, sqlite3_wal_checkpoint_v2(db_, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log, &checkpointed));
		require(log >= 0, STORE_ERROR, "the database is not in WAL mode");
		require(log == 0, BUSY, "frames are left in the log");
		return OK;
	}
	CATCH_STATUS
}

Status Store::check(std::string* problem)
{
	error_[0] = 0;
	try
	{
		REQUIRE_OPEN();
		require(problem != NULL, INVALID, "no output");
		problem->clear();
		sqlite3_busy_timeout(db_, READ_BUSY_MS);
		Transaction tx(db_, false);
		std::string found;
		{
			Query quick(db_, "PRAGMA quick_check");
			std::string answer = quick.row() ? quick.string(0) : std::string();
			if (answer != "ok")
				found = "quick_check: " + answer;
		}
		if (found.empty())
		{
			Query foreign(db_, "PRAGMA foreign_key_check");
			if (foreign.row())
				found = "foreign key violation in table " + foreign.string(0);
		}
		if (found.empty() && scalar(db_, "SELECT count(*) FROM meta") != 1)
			found = "the meta table does not hold exactly one row";
		if (found.empty())
		{
			Query ordinals(db_, "SELECT count(*), COALESCE(MIN(ordinal), 0), COALESCE(MAX(ordinal), -1) FROM pages");
			ordinals.row();
			Id pages = ordinals.integer(0);
			if (pages && (ordinals.integer(1) != 0 || ordinals.integer(2) != pages - 1))
				found = "page ordinals are not 0 to n-1";
		}
		if (found.empty() && scalar(db_, "SELECT count(*) FROM items, meta WHERE items.x + items.w > meta.grid_w"
			" OR items.y + items.h > meta.grid_h") != 0)
			found = "an item lies outside the stash grid";
		if (found.empty() && scalar(db_, "SELECT count(*) FROM cells JOIN items ON items.id = cells.item"
			" WHERE cells.page <> items.page OR cells.x < items.x OR cells.x >= items.x + items.w"
			" OR cells.y < items.y OR cells.y >= items.y + items.h") != 0)
			found = "a cell lies outside its item's rectangle";
		if (found.empty() && scalar(db_, "SELECT count(*) FROM items"
			" WHERE (SELECT count(*) FROM cells WHERE cells.item = items.id) <> items.w * items.h") != 0)
			found = "an item does not own exactly the cells of its rectangle";
		if (found.empty() && scalar(db_, "SELECT count(*) FROM items LEFT JOIN ops ON ops.id = items.pending_op"
			" WHERE items.pending_op IS NOT NULL AND (ops.id IS NULL OR ops.state <> 0)") != 0)
			found = "a pending marker points at an op that is not pending";
		tx.commit();
		if (!found.empty())
		{
			*problem = found;
			setError(found.c_str());
			return INVALID;
		}
		return OK;
	}
	CATCH_STATUS
}

#ifdef SHARED_STASH_TEST_API

void Store::testSetWriteLockName(const char* name)
{
	lockNameOverride_[0] = 0;
	if (name)
	{
		strncpy(lockNameOverride_, name, SHARED_STASH_MUTEX_NAME_SIZE - 1);
		lockNameOverride_[SHARED_STASH_MUTEX_NAME_SIZE - 1] = 0;
	}
}

void Store::testFailCreate(int step)
{
	failCreateStep_ = step >= 3 && step <= 5 ? step : 0;
}

int Store::testPragmaInt(const char* name)
{
	if (!db_ || !name || !*name || strlen(name) > 64)
		return -1;
	for (const char* p = name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
			return -1;
	char sql[96];
	_snprintf(sql, sizeof(sql) - 1, "PRAGMA %s", name);
	sql[sizeof(sql) - 1] = 0;
	sqlite3_stmt* stmt = NULL;
	int value = -1;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
		value = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return value;
}

Status Store::testAddPendingOp(const char* character, Id* opId, DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		requireCharacter(character);
		require(opId != NULL, INVALID, "no output");
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		require(currentMode(db_) == MODE_LIVE, CONFLICT, "the stash is not in live mode");
		Query count(db_, "SELECT count(*) FROM ops WHERE character = ?1 AND state = 0");
		count.text(1, character);
		require(count.row(), STORE_ERROR, "query returned no row");
		require(count.integer(0) < LIVE_OPS_CAP, BUSY, "the character has too many pending ops");
		count.reset();
		Query insert(db_, "INSERT INTO ops(character, state, created_utc) VALUES(?1, 0, ?2)");
		insert.text(1, character).number(2, nowUtc()).row();
		Id id = sqlite3_last_insert_rowid(db_);
		bumpRevision(db_);
		tx.commit();
		*opId = id;
		return OK;
	}
	CATCH_STATUS
}

Status Store::testAttachItem(Id opId, Id itemId, int pendingKind, DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		require(pendingKind == PENDING_DEPOSITED || pendingKind == PENDING_WITHDRAWN, INVALID, "bad pending kind");
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		require(currentMode(db_) == MODE_LIVE, CONFLICT, "the stash is not in live mode");
		Query op(db_, "SELECT state FROM ops WHERE id = ?1");
		op.number(1, opId);
		require(op.row(), NOT_FOUND, "the op does not exist");
		Id opState = op.integer(0);
		Query item(db_, "SELECT pending_op FROM items WHERE id = ?1");
		item.number(1, itemId);
		require(item.row(), NOT_FOUND, "the item does not exist");
		require(opState == OP_PENDING, CONFLICT, "the op is not pending");
		require(item.integer(0) == 0, CONFLICT, "the item already carries a pending marker");
		op.reset();
		item.reset();
		Query update(db_, "UPDATE items SET pending_op = ?1, pending_kind = ?2 WHERE id = ?3");
		update.number(1, opId).number(2, pendingKind).number(3, itemId).row();
		bumpRevision(db_);
		tx.commit();
		return OK;
	}
	CATCH_STATUS
}

Status Store::testInsertDeposit(Id opId, const ItemRow& row, DWORD lockWaitMs)
{
	error_[0] = 0;
	try
	{
		REQUIRE_WRITABLE();
		LockHold hold;
		sqlite3_busy_timeout(db_, beginLockedWork(lockWaitMs, &hold));
		Transaction tx(db_, true);
		Meta meta;
		readMetaRow(db_, &meta);
		require(meta.mode == MODE_LIVE, CONFLICT, "the stash is not in live mode");
		Query op(db_, "SELECT state FROM ops WHERE id = ?1");
		op.number(1, opId);
		require(op.row(), NOT_FOUND, "the op does not exist");
		require(op.integer(0) == OP_PENDING, CONFLICT, "the op is not pending");
		op.reset();
		require(insideGrid(row, meta.gridW, meta.gridH), INVALID, "the item lies outside the stash grid");
		require(!row.data.empty() && row.data.size() <= MAX_BLOB_SIZE, INVALID, "the item data is empty or too large");
		Query item(db_, "INSERT INTO items(page, x, y, w, h, data, version, pending_op, pending_kind)"
			" VALUES(?1, ?2, ?3, ?4, ?5, ?6, 1, ?7, 1)");
		item.number(1, row.page).number(2, row.x).number(3, row.y).number(4, row.w).number(5, row.h)
			.blob(6, row.data).number(7, opId).row();
		Id id = sqlite3_last_insert_rowid(db_);
		Query cell(db_, "INSERT INTO cells(page, x, y, item) VALUES(?1, ?2, ?3, ?4)");
		insertCells(cell, row.page, row.x, row.y, row.w, row.h, id);
		bumpRevision(db_);
		tx.commit();
		return OK;
	}
	CATCH_STATUS
}

#endif

}
