/*=================================================================
	Tests for src/common/SharedStashStore: schema and open gate,
	import, settle and revert, the mode guard, the write lock across
	processes, backups, the WAL pairing, paths and failed creates.
	Built and run by scripts/build.ps1 -Test; exits 1 on any failure.

	SharedStashStoreTests.exe [--scratch DIR]
	SharedStashStoreTests.exe --child MODE ...   (the suite's own helper processes)

	--scratch defaults to a scratch folder beside the exe. Each run
	works in its own subfolder, removed when every check passes and
	kept (its path printed) when one fails.
=================================================================*/

#include "../src/common/SharedStashStore.h"
#include "../third_party/sqlite/sqlite3.h"
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <string>
#include <vector>

using namespace SharedStash;

static int nbPassed = 0;
static int nbFailed = 0;

static void check(bool ok, const char* text, int line)
{
	if (ok)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d: %s\n", line, text);
}

static void checkInt(__int64 actual, __int64 expected, const char* text, int line)
{
	if (actual == expected)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d: %s is %I64d, expected %I64d\n", line, text, actual, expected);
}

static const char* StatusNames[] = {"OK", "BUSY", "NOT_FOUND", "CONFLICT", "INVALID", "FOREIGN", "NEWER", "STORE_ERROR"};

static const char* statusName(Status status)
{
	return status >= OK && status <= STORE_ERROR ? StatusNames[status] : "?";
}

static void checkStatus(Status actual, Status expected, const char* text, const char* error, int line)
{
	if (actual == expected)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d: %s is %s, expected %s (%s)\n", line, text, statusName(actual), statusName(expected),
		error);
}

#define CHECK(cond) check((cond) ? true : false, #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkInt((__int64)(actual), (__int64)(expected), #actual, __LINE__)
#define CHECK_ST(store, call, expected) \
	do \
	{ \
		Status checkedStatus = (call); \
		checkStatus(checkedStatus, expected, #call, (store).error(), __LINE__); \
	} while (0)

static char runDir[MAX_PATH] = "";
static char exePath[MAX_PATH] = "";
static char nameTag[64] = "";
static int nameSeq = 0;
static HANDLE job = NULL;
static std::vector<std::string> logLines;

static void recordLog(const char* line)
{
	logLines.push_back(line);
}

// The record holds exactly count lines, each holding text (none when count is 0). The record is cleared either
// way, so a stray line fails only the check that met it; on a failure every line is printed.
static void expectLog(const char* text, size_t count, int line)
{
	bool ok = logLines.size() == count;
	for (size_t i = 0; i < logLines.size() && ok; i++)
		ok = text && strstr(logLines[i].c_str(), text) != NULL;
	check(ok, text ? text : "no log line", line);
	if (!ok)
		for (size_t i = 0; i < logLines.size(); i++)
			printf("  log: %s\n", logLines[i].c_str());
	logLines.clear();
}

#define EXPECT_LOG(text, count) expectLog(text, count, __LINE__)
#define EXPECT_NO_LOG() expectLog(NULL, 0, __LINE__)

static ULONGLONG nowMs()
{
	LARGE_INTEGER count;
	LARGE_INTEGER frequency;
	QueryPerformanceCounter(&count);
	QueryPerformanceFrequency(&frequency);
	return (ULONGLONG)(count.QuadPart / frequency.QuadPart) * 1000
		+ (ULONGLONG)(count.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart;
}

static std::string runPath(const char* name)
{
	return std::string(runDir) + "\\" + name;
}

// Kernel object names carry the pid and the tick of this run, so two suites never share one.
static std::string objectName(const char* scope, const char* kind)
{
	char name[128];
	_snprintf(name, sizeof(name) - 1, "%s\\PlugYStashTest-%s-%s%d", scope, nameTag, kind, ++nameSeq);
	name[sizeof(name) - 1] = 0;
	return name;
}

static HANDLE makeEvent(std::string* name)
{
	*name = objectName("Local", "e");
	return CreateEventA(NULL, TRUE, FALSE, name->c_str());
}

static std::set<std::string> listNames(const std::string& dir)
{
	std::set<std::string> names;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return names;
	do
	{
		if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
			names.insert(fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return names;
}

static std::set<std::string> listRun()
{
	return listNames(runDir);
}

static std::string baseName(const std::string& path)
{
	size_t slash = path.rfind('\\');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// after holds before, plus at most the -wal and -shm of db.
static bool onlyWalShmAdded(const std::set<std::string>& before, const std::set<std::string>& after,
	const std::string& db)
{
	std::string name = baseName(db);
	for (std::set<std::string>::const_iterator it = after.begin(); it != after.end(); ++it)
		if (!before.count(*it) && *it != name + "-wal" && *it != name + "-shm")
			return false;
	for (std::set<std::string>::const_iterator it = before.begin(); it != before.end(); ++it)
		if (!after.count(*it))
			return false;
	return true;
}

static bool readBytes(const std::string& path, Bytes* out)
{
	out->clear();
	HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	unsigned char buffer[65536];
	DWORD n = 0;
	bool ok = true;
	while ((ok = ReadFile(h, buffer, sizeof(buffer), &n, NULL) != 0) && n)
		out->insert(out->end(), buffer, buffer + n);
	CloseHandle(h);
	return ok;
}

static bool writeBytes(const std::string& path, const Bytes& data)
{
	HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD n = 0;
	bool ok = data.empty() || (WriteFile(h, &data[0], (DWORD)data.size(), &n, NULL) && n == data.size());
	CloseHandle(h);
	return ok;
}

static bool exists(const std::string& path)
{
	return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static __int64 fileSize(const std::string& path)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
		return -1;
	return ((__int64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

static std::string upper(const std::string& s)
{
	std::string out = s;
	for (size_t i = 0; i < out.size(); i++)
		if (out[i] >= 'a' && out[i] <= 'z')
			out[i] = (char)(out[i] - 'a' + 'A');
	return out;
}

// --- Raw SQLite, for what the Store must never do itself ------------------------------------------------------------

static sqlite3* rawOpen(const std::string& path, bool create = false)
{
	std::string utf8;
	SqlitePath(path.c_str(), &utf8);
	sqlite3* db = NULL;
	int rc = sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0), NULL);
	if (rc != SQLITE_OK)
	{
		printf("  sqlite open %s: %s\n", path.c_str(), db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_busy_timeout(db, 2000);
	return db;
}

static bool rawExec(sqlite3* db, const char* sql)
{
	if (!db)
		return false;
	char* err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK)
		printf("  sqlite: %s: %s\n", sql, err ? err : sqlite3_errstr(rc));
	sqlite3_free(err);
	return rc == SQLITE_OK;
}

static __int64 rawInt(sqlite3* db, const char* sql)
{
	if (!db)
		return -1;
	sqlite3_stmt* stmt = NULL;
	__int64 value = -1;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
		value = sqlite3_column_int64(stmt, 0);
	else
		printf("  sqlite: %s: %s\n", sql, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);
	return value;
}

static std::string rawText(sqlite3* db, const char* sql)
{
	std::string value;
	sqlite3_stmt* stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW
		&& sqlite3_column_text(stmt, 0))
		value = (const char*)sqlite3_column_text(stmt, 0);
	sqlite3_finalize(stmt);
	return value;
}

static bool rawRun(const std::string& path, const char* sql)
{
	sqlite3* db = rawOpen(path);
	bool ok = rawExec(db, sql);
	sqlite3_close(db);
	return ok;
}

static __int64 rawQuery(const std::string& path, const char* sql)
{
	sqlite3* db = rawOpen(path);
	__int64 value = rawInt(db, sql);
	sqlite3_close(db);
	return value;
}

// --- Rows -----------------------------------------------------------------------------------------------------------

static Bytes blobOf(int seed, int size = 6)
{
	Bytes b;
	b.push_back(0x4A);
	b.push_back(0x4D);
	for (int i = 2; i < size; i++)
		b.push_back((unsigned char)(seed * 13 + i));
	return b;
}

static PageRow pageOf(Id id, int ordinal, const Bytes& name = Bytes(), DWORD flags = 0)
{
	PageRow page;
	page.id = id;
	page.ordinal = ordinal;
	page.flags = flags;
	page.name = name;
	page.version = 1;
	return page;
}

static ItemRow itemOf(Id id, Id page, int x, int y, int w = 1, int h = 1)
{
	ItemRow item;
	item.id = id;
	item.page = page;
	item.x = x;
	item.y = y;
	item.w = w;
	item.h = h;
	item.data = blobOf((int)id);
	item.version = 1;
	item.pendingOp = 0;
	item.pendingKind = PENDING_NONE;
	return item;
}

static Meta metaFor(int gridW, int gridH)
{
	static int counter = 0;
	counter++;
	Meta meta;
	memset(&meta, 0, sizeof(meta));
	for (int i = 0; i < 16; i++)
		meta.dbGuid[i] = (unsigned char)(0xA0 + i * 3 + counter);
	meta.gridW = gridW;
	meta.gridH = gridH;
	meta.sssVersion = 0x3130;
	// Ignored by create(): it always starts IMPORTING with gold 0 and revision 0.
	meta.mode = MODE_LIVE;
	meta.gold = 7;
	meta.revision = 99;
	return meta;
}

static std::vector<PageRow> onePage()
{
	return std::vector<PageRow>(1, pageOf(1, 0));
}

// create + import + LIVE; the Store stays open.
static bool makeLive(Store& s, const std::string& path, const std::vector<PageRow>& pages,
	const std::vector<ItemRow>& items, int grid = 10)
{
	Meta meta = metaFor(grid, grid);
	bool ok = s.create(path.c_str(), meta, 2000) == OK && s.importStash(pages, items, 0, 2000) == OK
		&& s.setMode(MODE_IMPORTING, MODE_LIVE, 2000) == OK;
	if (!ok)
		printf("  setup of %s failed: %s\n", path.c_str(), s.error());
	return ok;
}

static Meta metaOf(Store& s)
{
	Meta meta;
	memset(&meta, 0, sizeof(meta));
	if (s.readMeta(&meta) != OK)
		printf("  readMeta failed: %s\n", s.error());
	return meta;
}

static Id revisionOf(Store& s)
{
	return metaOf(s).revision;
}

static Id addOp(Store& s, const char* character)
{
	Id op = 0;
	if (s.testAddPendingOp(character, &op, 2000) != OK)
		printf("  testAddPendingOp(%s) failed: %s\n", character, s.error());
	return op;
}

static Status settleOne(Store& s, const char* character, Id op, DWORD waitMs, Id* bad = NULL)
{
	Meta meta = metaOf(s);
	Id ignored = 0;
	return s.settleOps(character, meta.dbGuid, &op, 1, waitMs, bad ? bad : &ignored);
}

// The op's state, or -1 when it does not exist.
static int opStateOf(Store& s, Id op)
{
	std::vector<OpRow> rows;
	if (s.opStates(&op, 1, &rows) != OK || rows.size() != 1)
		return -1;
	return rows[0].state;
}

static bool readRows(Store& s, std::vector<PageRow>* pages, std::vector<ItemRow>* items)
{
	std::vector<OpRow> ops;
	return s.readAll(NULL, pages, items, &ops) == OK;
}

static const ItemRow* itemAt(const std::vector<ItemRow>& items, int x, int y)
{
	for (size_t i = 0; i < items.size(); i++)
		if (items[i].x == x && items[i].y == y)
			return &items[i];
	return NULL;
}

static bool sameMeta(const Meta& a, const Meta& b)
{
	return !memcmp(a.dbGuid, b.dbGuid, 16) && a.mode == b.mode && a.gridW == b.gridW && a.gridH == b.gridH
		&& a.gold == b.gold && a.revision == b.revision && a.sssVersion == b.sssVersion;
}

static bool samePages(const std::vector<PageRow>& a, const std::vector<PageRow>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (a[i].id != b[i].id || a[i].ordinal != b[i].ordinal || a[i].flags != b[i].flags || a[i].name != b[i].name)
			return false;
	return true;
}

static bool sameItems(const std::vector<ItemRow>& a, const std::vector<ItemRow>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (a[i].id != b[i].id || a[i].page != b[i].page || a[i].x != b[i].x || a[i].y != b[i].y
			|| a[i].w != b[i].w || a[i].h != b[i].h || a[i].data != b[i].data)
			return false;
	return true;
}

// --- Child processes ------------------------------------------------------------------------------------------------

static std::string quoted(const std::string& s)
{
	return "\"" + s + "\"";
}

// The same exe with --child, created suspended and put in the run's kill-on-close job before it runs.
static HANDLE startChild(const std::string& args)
{
	std::string command = quoted(exePath) + " --child " + args;
	std::vector<char> line(command.begin(), command.end());
	line.push_back(0);
	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, &line[0], NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
	{
		printf("  cannot start child (error %lu)\n", GetLastError());
		return NULL;
	}
	if (!AssignProcessToJobObject(job, pi.hProcess))
	{
		printf("  cannot put child in the job (error %lu)\n", GetLastError());
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 15000);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return NULL;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	return pi.hProcess;
}

// True when the child signalled before it exited and within the budget.
static bool waitReady(HANDLE ready, HANDLE child, DWORD ms = 15000)
{
	if (!ready || !child)
		return false;
	HANDLE handles[2] = {ready, child};
	DWORD wait = WaitForMultipleObjects(2, handles, FALSE, ms);
	if (wait != WAIT_OBJECT_0)
		printf("  child not ready (wait %lu)\n", wait);
	return wait == WAIT_OBJECT_0;
}

// The child's exit code, or 0xFFFF0000 when it did not end within the budget.
static DWORD waitExit(HANDLE child, DWORD ms = 15000)
{
	if (!child)
		return 0xFFFF0001;
	if (WaitForSingleObject(child, ms) != WAIT_OBJECT_0)
	{
		TerminateProcess(child, 1);
		WaitForSingleObject(child, 15000);
		CloseHandle(child);
		return 0xFFFF0000;
	}
	DWORD code = 0xFFFF0002;
	GetExitCodeProcess(child, &code);
	CloseHandle(child);
	return code;
}

// TerminateProcess only starts the end: the mutex is abandoned and the file locks go once the process has ended.
static bool killChild(HANDLE child)
{
	if (!child)
		return false;
	TerminateProcess(child, 9);
	bool ended = WaitForSingleObject(child, 15000) == WAIT_OBJECT_0;
	CloseHandle(child);
	return ended;
}

#define CHILD_SETUP_FAILED 10
#define CHILD_STATUS_BASE 100

static HANDLE openEvent(const char* name)
{
	return OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
}

static int childHold(const char* mutexName, const char* readyName, const char* releaseName)
{
	HANDLE mutex = CreateMutexA(NULL, FALSE, mutexName);
	HANDLE ready = openEvent(readyName);
	HANDLE release = openEvent(releaseName);
	if (!mutex || !ready || !release || WaitForSingleObject(mutex, 15000) != WAIT_OBJECT_0)
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	WaitForSingleObject(release, 15000);
	ReleaseMutex(mutex);
	return 0;
}

// A mutex nobody else may open: a DACL with no ACE.
static int childEmptyDacl(const char* mutexName, const char* readyName, const char* releaseName)
{
	SECURITY_DESCRIPTOR descriptor;
	ACL acl;
	if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !InitializeAcl(&acl, sizeof(acl), ACL_REVISION) || !SetSecurityDescriptorDacl(&descriptor, TRUE, &acl, FALSE))
		return CHILD_SETUP_FAILED;
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &descriptor, FALSE};
	HANDLE mutex = CreateMutexA(&attributes, FALSE, mutexName);
	if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
		return CHILD_SETUP_FAILED + 1;
	HANDLE ready = openEvent(readyName);
	HANDLE release = openEvent(releaseName);
	if (!ready || !release)
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	WaitForSingleObject(release, 30000);
	CloseHandle(mutex);
	return 0;
}

static int childRawLock(const char* path, const char* readyName, const char* releaseName)
{
	sqlite3* db = rawOpen(path);
	HANDLE ready = openEvent(readyName);
	HANDLE release = openEvent(releaseName);
	if (!db || !ready || !release || !rawExec(db, "BEGIN IMMEDIATE"))
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	WaitForSingleObject(release, 15000);
	rawExec(db, "ROLLBACK");
	sqlite3_close(db);
	return 0;
}

// Leaves a committed change in -wal only, then waits to be killed.
static int childHotLog(const char* path, const char* readyName)
{
	sqlite3* db = rawOpen(path);
	HANDLE ready = openEvent(readyName);
	if (!db || !ready || !rawExec(db, "PRAGMA user_version = 2"))
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	Sleep(60000);
	return 0;
}

// Rewrites every row of a rollback-mode file through a 10-page cache, so the journal is synced and pages spill into
// the main file before the commit; then waits to be killed, leaving a hot -journal.
static int childHotJournal(const char* path, const char* readyName)
{
	sqlite3* db = rawOpen(path);
	HANDLE ready = openEvent(readyName);
	if (!db || !ready || !rawExec(db, "PRAGMA cache_size = 10") || !rawExec(db, "BEGIN IMMEDIATE")
		|| !rawExec(db, "UPDATE x SET a = randomblob(200)"))
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	Sleep(60000);
	return 0;
}

// Holds the write mutex and an open write transaction with one inserted row, then waits to be killed.
static int childKillWriter(const char* path, const char* mutexName, const char* readyName)
{
	HANDLE mutex = CreateMutexA(NULL, FALSE, mutexName);
	HANDLE ready = openEvent(readyName);
	if (!mutex || !ready || WaitForSingleObject(mutex, 15000) != WAIT_OBJECT_0)
		return CHILD_SETUP_FAILED;
	sqlite3* db = rawOpen(path);
	if (!rawExec(db, "BEGIN IMMEDIATE")
		|| !rawExec(db, "INSERT INTO ops(character, state, created_utc) VALUES('Killed', 0, 0)"))
		return CHILD_SETUP_FAILED + 1;
	SetEvent(ready);
	Sleep(60000);
	return 0;
}

// Creates and imports through a Store it keeps open, so the rows sit in -wal; then waits to be killed.
static int childStoreCreate(const char* path, const char* readyName)
{
	HANDLE ready = openEvent(readyName);
	Store s;
	std::vector<ItemRow> items;
	items.push_back(itemOf(1, 1, 0, 0));
	items.push_back(itemOf(2, 1, 1, 0));
	items.push_back(itemOf(3, 1, 2, 0, 2, 2));
	if (!ready || s.create(path, metaFor(10, 10), 5000) != OK || s.importStash(onePage(), items, 0, 5000) != OK)
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	Sleep(60000);
	return 0;
}

// A create() killed before its COMMIT, made with raw SQLite.
static int childKillCreate(const char* path, const char* readyName)
{
	HANDLE ready = openEvent(readyName);
	sqlite3* db = rawOpen(path, true);
	if (!ready || !db || rawText(db, "PRAGMA journal_mode=WAL") != "wal" || !rawExec(db, "BEGIN IMMEDIATE")
		|| !rawExec(db, "CREATE TABLE t(a INTEGER)"))
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	Sleep(60000);
	return 0;
}

static int childSettle(const char* path, const char* character, const char* opText, const char* waitText)
{
	Store s;
	if (s.open(path) != OK)
		return CHILD_SETUP_FAILED;
	Id op = _atoi64(opText);
	Status status = settleOne(s, character, op, (DWORD)strtoul(waitText, NULL, 10));
	return CHILD_STATUS_BASE + status;
}

static int childWriteLock(const char* path, const char* readyName, const char* releaseName)
{
	HANDLE ready = openEvent(readyName);
	HANDLE release = openEvent(releaseName);
	WriteLock lock;
	if (!ready || !release || lock.acquire(path, 15000) != OK || !lock.owned())
		return CHILD_SETUP_FAILED;
	SetEvent(ready);
	WaitForSingleObject(release, 15000);
	lock.release();
	return 0;
}

// T-D11's racer: 50 cycles of add, deposit, then settle (even cycle) or revert (odd cycle), never retried.
static int childRace(const char* path, int index, const char* startName, const char* readyName, const char* result)
{
	Store s;
	HANDLE start = OpenEventA(SYNCHRONIZE, FALSE, startName);
	HANDLE ready = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE, FALSE, readyName);
	if (!start || !ready || s.open(path) != OK)
		return CHILD_SETUP_FAILED;
	Meta meta = metaOf(s);
	ReleaseSemaphore(ready, 1, NULL);
	if (WaitForSingleObject(start, 30000) != WAIT_OBJECT_0)
		return CHILD_SETUP_FAILED + 1;
	DWORD waitMs = index % 2 ? 2000 : 1000;
	char character[16];
	_snprintf(character, sizeof(character) - 1, "Racer%d", index);
	character[sizeof(character) - 1] = 0;
	int changes = 0;
	int busy = 0;
	int unexpected = 0;
	for (int k = 0; k < 50 && !unexpected; k++)
	{
		Id op = 0;
		Status status = s.testAddPendingOp(character, &op, waitMs);
		if (status == BUSY)
		{
			busy++;
			continue;
		}
		if (status != OK)
		{
			unexpected = CHILD_STATUS_BASE + status;
			break;
		}
		changes++;
		ItemRow row = itemOf(0, index + 1, k % 10, k / 10);
		status = s.testInsertDeposit(op, row, waitMs);
		if (status == OK)
			changes++;
		else if (status == BUSY)
			busy++;
		else
			unexpected = CHILD_STATUS_BASE + status;
		if (k % 2 == 0)
		{
			Id bad = 0;
			status = s.settleOps(character, meta.dbGuid, &op, 1, waitMs, &bad);
			if (status == OK)
				changes++;
		}
		else
		{
			DWORD reverted = 0;
			status = s.revertPendingOps(character, waitMs, &reverted);
			if (status == OK && reverted)
				changes++;
		}
		if (status == BUSY)
			busy++;
		else if (status != OK && !unexpected)
			unexpected = CHILD_STATUS_BASE + status;
	}
	FILE* f = fopen(result, "w");
	if (!f)
		return CHILD_SETUP_FAILED + 2;
	fprintf(f, "%d %d %d\n", changes, busy, unexpected);
	fclose(f);
	return unexpected ? 3 : 0;
}

static int childMain(int argc, char** argv)
{
	std::string mode = argc > 0 ? argv[0] : "";
	if (mode == "hold" && argc == 4)
		return childHold(argv[1], argv[2], argv[3]);
	if (mode == "emptydacl" && argc == 4)
		return childEmptyDacl(argv[1], argv[2], argv[3]);
	if (mode == "rawlock" && argc == 4)
		return childRawLock(argv[1], argv[2], argv[3]);
	if (mode == "hotlog" && argc == 3)
		return childHotLog(argv[1], argv[2]);
	if (mode == "hotjournal" && argc == 3)
		return childHotJournal(argv[1], argv[2]);
	if (mode == "killwriter" && argc == 4)
		return childKillWriter(argv[1], argv[2], argv[3]);
	if (mode == "storecreate" && argc == 3)
		return childStoreCreate(argv[1], argv[2]);
	if (mode == "killcreate" && argc == 3)
		return childKillCreate(argv[1], argv[2]);
	if (mode == "settle" && argc == 5)
		return childSettle(argv[1], argv[2], argv[3], argv[4]);
	if (mode == "writelock" && argc == 4)
		return childWriteLock(argv[1], argv[2], argv[3]);
	if (mode == "race" && argc == 6)
		return childRace(argv[1], atoi(argv[2]), argv[3], argv[4], argv[5]);
	return 2;
}

// A child holding a mutex (hold, emptydacl, writelock): started, waited for, and released by the caller.
struct Holder
{
	HANDLE process;
	HANDLE ready;
	HANDLE release;
};

static bool startHolder(const char* mode, const std::string& target, Holder* h)
{
	std::string readyName;
	std::string releaseName;
	h->ready = makeEvent(&readyName);
	h->release = makeEvent(&releaseName);
	h->process = startChild(std::string(mode) + " " + quoted(target) + " " + quoted(readyName) + " "
		+ quoted(releaseName));
	return waitReady(h->ready, h->process);
}

static DWORD stopHolder(Holder* h)
{
	SetEvent(h->release);
	DWORD code = waitExit(h->process);
	CloseHandle(h->ready);
	CloseHandle(h->release);
	return code;
}

static DWORD runSettleChild(const std::string& path, const char* character, Id op, DWORD waitMs, ULONGLONG* tookMs)
{
	char args[512];
	_snprintf(args, sizeof(args) - 1, "settle %s %s %I64d %lu", quoted(path).c_str(), character, op, waitMs);
	args[sizeof(args) - 1] = 0;
	ULONGLONG start = nowMs();
	DWORD code = waitExit(startChild(args));
	if (tookMs)
		*tookMs = nowMs() - start;
	return code;
}


/*============================= T-D1 ==========================*/

static void testCreateAndGate()
{
	printf("T-D1 create, schema and open gate\n");
	std::string path = runPath("t-d1.sqlite3");
	Meta meta = metaFor(9, 12);
	meta.sssVersion = 0x3230;
	Store s;
	CHECK_ST(s, s.create(path.c_str(), meta, 1000), OK);
	sqlite3* db = rawOpen(path);
	CHECK_EQ(rawInt(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table'"
		" AND name IN ('meta', 'pages', 'ops', 'items', 'cells')"), 5);
	CHECK_EQ(rawInt(db, "SELECT count(*) FROM sqlite_master WHERE type = 'index'"
		" AND name IN ('items_page', 'items_pending', 'cells_item', 'ops_character')"), 4);
	CHECK_EQ(rawInt(db, "PRAGMA application_id"), 1348029526);
	CHECK_EQ(rawInt(db, "PRAGMA user_version"), 1);
	CHECK(rawText(db, "PRAGMA journal_mode") == "wal");
	sqlite3_close(db);
	// Connection settings: only the Store's own connection shows what its open pragmas set.
	CHECK_EQ(s.testPragmaInt("synchronous"), 2);
	CHECK_EQ(s.testPragmaInt("foreign_keys"), 1);
	CHECK_EQ(s.testPragmaInt("trusted_schema"), 0);
	CHECK_EQ(s.testPragmaInt("journal_size_limit"), 4194304);
	Meta read = metaOf(s);
	CHECK(!memcmp(read.dbGuid, meta.dbGuid, 16));
	CHECK(read.gridW == 9 && read.gridH == 12 && read.sssVersion == 0x3230);
	CHECK(read.mode == MODE_IMPORTING && read.gold == 0 && read.revision == 0);
	s.close();

	std::set<std::string> before = listRun();
	Meta bad = metaFor(0, 10);
	Store invalid;
	CHECK_ST(invalid, invalid.create(runPath("t-d1-grid.sqlite3").c_str(), bad, 1000), INVALID);
	CHECK(listRun() == before);

	// A held lock: nothing is created.
	std::string lockName = objectName("Global", "m");
	Holder holder;
	CHECK(startHolder("hold", lockName, &holder));
	Store busy;
	busy.testSetWriteLockName(lockName.c_str());
	before = listRun();
	CHECK_ST(busy, busy.create(runPath("t-d1-busy.sqlite3").c_str(), metaFor(10, 10), 100), BUSY);
	CHECK(listRun() == before);
	CHECK_EQ(stopHolder(&holder), 0);

	// Gate controls: the refused file keeps its bytes; only -wal and -shm may appear.
	std::string foreign = runPath("t-d1-foreign.sqlite3");
	sqlite3* raw = rawOpen(foreign, true);
	CHECK(rawExec(raw, "PRAGMA application_id = 1348031304; PRAGMA user_version = 2; CREATE TABLE x(a INTEGER)"));
	sqlite3_close(raw);
	Bytes bytesBefore;
	Bytes bytesAfter;
	CHECK(readBytes(foreign, &bytesBefore));
	before = listRun();
	Store gate;
	CHECK_ST(gate, gate.open(foreign.c_str()), FOREIGN);
	CHECK(readBytes(foreign, &bytesAfter) && bytesAfter == bytesBefore);
	CHECK(onlyWalShmAdded(before, listRun(), foreign));

	std::string newer = runPath("t-d1-newer.sqlite3");
	Store made;
	CHECK_ST(made, made.create(newer.c_str(), metaFor(10, 10), 1000), OK);
	made.close();
	CHECK(rawRun(newer, "PRAGMA user_version = 2"));
	CHECK(readBytes(newer, &bytesBefore));
	before = listRun();
	CHECK_ST(gate, gate.open(newer.c_str()), NEWER);
	CHECK(readBytes(newer, &bytesAfter) && bytesAfter == bytesBefore);
	CHECK(onlyWalShmAdded(before, listRun(), newer));

	// A hot log: the newer user_version sits only in -wal, which the gate must read and must not checkpoint.
	std::string hot = runPath("t-d1-hot.sqlite3");
	CHECK_ST(made, made.create(hot.c_str(), metaFor(10, 10), 1000), OK);
	made.close();
	std::string readyName;
	HANDLE ready = makeEvent(&readyName);
	HANDLE child = startChild("hotlog " + quoted(hot) + " " + quoted(readyName));
	CHECK(waitReady(ready, child));
	CHECK(killChild(child));
	CloseHandle(ready);
	Bytes walBefore;
	Bytes walAfter;
	CHECK(fileSize(hot + "-wal") > 0);
	CHECK(readBytes(hot, &bytesBefore));
	CHECK(readBytes(hot + "-wal", &walBefore));
	before = listRun();
	CHECK_ST(gate, gate.open(hot.c_str()), NEWER);
	CHECK(readBytes(hot, &bytesAfter) && bytesAfter == bytesBefore);
	CHECK(readBytes(hot + "-wal", &walAfter) && walAfter == walBefore);
	CHECK(onlyWalShmAdded(before, listRun(), hot));

	// A rollback-mode file of another program with a hot -journal (its writer killed after pages spilled into the
	// main file): a read-write first read would play the journal back before the gate refuses the file.
	std::string journaled = runPath("t-d1-journaled.sqlite3");
	raw = rawOpen(journaled, true);
	CHECK(rawExec(raw, "PRAGMA application_id = 1348031304; CREATE TABLE x(a BLOB);"
		" WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000)"
		" INSERT INTO x SELECT randomblob(200) FROM n"));
	sqlite3_close(raw);
	ready = makeEvent(&readyName);
	child = startChild("hotjournal " + quoted(journaled) + " " + quoted(readyName));
	CHECK(waitReady(ready, child));
	CHECK(killChild(child));
	CloseHandle(ready);
	Bytes journalBefore;
	Bytes journalAfter;
	CHECK(readBytes(journaled + "-journal", &journalBefore) && journalBefore.size() > 0 && journalBefore[0] != 0);
	CHECK(readBytes(journaled, &bytesBefore));
	before = listRun();
	CHECK_ST(gate, gate.open(journaled.c_str()), FOREIGN);
	CHECK(readBytes(journaled, &bytesAfter) && bytesAfter == bytesBefore);
	CHECK(readBytes(journaled + "-journal", &journalAfter) && journalAfter == journalBefore);
	CHECK(listRun() == before);
}


/*============================= T-D2 ==========================*/

static void testOpen()
{
	printf("T-D2 open\n");
	std::set<std::string> before = listRun();
	Store s;
	CHECK_ST(s, s.open(runPath("t-d2-missing.sqlite3").c_str()), NOT_FOUND);
	CHECK(listRun() == before);
	std::string zero = runPath("t-d2-zero.sqlite3");
	CHECK(writeBytes(zero, Bytes()));
	before = listRun();
	CHECK_ST(s, s.open(zero.c_str()), INVALID);
	CHECK(listRun() == before);

	std::string ro = runPath("t-d2-ro.sqlite3");
	CHECK_ST(s, s.create(ro.c_str(), metaFor(10, 10), 1000), OK);
	s.close();
	CHECK(SetFileAttributesA(ro.c_str(), FILE_ATTRIBUTE_READONLY));
	before = listRun();
	CHECK_ST(s, s.open(ro.c_str()), STORE_ERROR);
	CHECK(strstr(s.error(), "read-only") != NULL);
	CHECK(listRun() == before);
	CHECK(SetFileAttributesA(ro.c_str(), FILE_ATTRIBUTE_NORMAL));
	CHECK_ST(s, s.open(ro.c_str()), OK);
	s.close();

	// A read-only Store reads, refuses writes and never changes the main file.
	Bytes bytesBefore;
	Bytes bytesAfter;
	CHECK(readBytes(ro, &bytesBefore));
	before = listRun();
	Store r;
	CHECK_ST(r, r.open(ro.c_str(), true), OK);
	Meta meta = metaOf(r);
	CHECK(meta.mode == MODE_IMPORTING);
	Id op = 1;
	Id bad = 5;
	CHECK_ST(r, r.settleOps("Carl", meta.dbGuid, &op, 1, 100, &bad), INVALID);
	r.close();
	CHECK(readBytes(ro, &bytesAfter) && bytesAfter == bytesBefore);
	CHECK(onlyWalShmAdded(before, listRun(), ro));
}


/*============================= T-D3 ==========================*/

static void testImportRoundTrip()
{
	printf("T-D3 import and readAll\n");
	std::string path = runPath("t-d3.sqlite3");
	std::vector<PageRow> pages;
	pages.push_back(pageOf(5, 0));
	const unsigned char* twenty = (const unsigned char*)"ABCDEFGHIJKLMNOPQRST";
	Bytes longName(twenty, twenty + 20);
	pages.push_back(pageOf(6, 1, longName, 0xFFFFFFFF));
	Bytes highName;
	highName.push_back(0x80);
	highName.push_back(0xE9);
	highName.push_back(0xFF);
	pages.push_back(pageOf(9, 2, highName, 7));
	std::vector<ItemRow> items;
	items.push_back(itemOf(11, 5, 0, 0, 2, 3));
	items.push_back(itemOf(12, 6, 9, 9));
	ItemRow high = itemOf(20, 9, 3, 4, 1, 2);
	high.data.push_back(0xFF);
	high.data.push_back(0x00);
	high.data.push_back(0x80);
	items.push_back(high);

	Store s;
	CHECK_ST(s, s.create(path.c_str(), metaFor(10, 10), 1000), OK);
	CHECK_ST(s, s.importStash(pages, items, 0, 1000), OK);
	std::vector<PageRow> readPages;
	std::vector<ItemRow> readItems;
	std::vector<OpRow> ops;
	CHECK_ST(s, s.readAll(NULL, &readPages, &readItems, &ops), OK);
	CHECK(samePages(readPages, pages));
	CHECK(sameItems(readItems, items));
	CHECK(ops.empty());
	bool versionsOne = true;
	for (size_t i = 0; i < readPages.size(); i++)
		versionsOne = versionsOne && readPages[i].version == 1;
	for (size_t i = 0; i < readItems.size(); i++)
		versionsOne = versionsOne && readItems[i].version == 1 && readItems[i].pendingOp == 0
			&& readItems[i].pendingKind == PENDING_NONE;
	CHECK(versionsOne);
	Meta meta = metaOf(s);
	CHECK(meta.revision == 1 && meta.gold == 0 && meta.mode == MODE_IMPORTING);
	std::string problem = "x";
	CHECK_ST(s, s.check(&problem), OK);
	CHECK(problem.empty());

	// meta and maxOpId come from the same snapshot as the rows.
	Meta all;
	Id maxAll = -1;
	Id maxRead = -1;
	CHECK_ST(s, s.readAll(&all, &readPages, &readItems, &ops, &maxAll), OK);
	CHECK_ST(s, s.maxOpId(&maxRead), OK);
	CHECK(sameMeta(all, metaOf(s)));
	CHECK(maxAll == 0 && maxRead == 0);

	// Still IMPORTING but no longer empty: the emptiness rule, not the mode guard, refuses a second import.
	CHECK_ST(s, s.importStash(pages, items, 0, 1000), CONFLICT);
	std::vector<PageRow> otherPages = pages;
	std::vector<ItemRow> otherItems = items;
	for (size_t i = 0; i < otherPages.size(); i++)
		otherPages[i].id += 100;
	for (size_t i = 0; i < otherItems.size(); i++)
	{
		otherItems[i].id += 100;
		otherItems[i].page += 100;
	}
	CHECK_ST(s, s.importStash(otherPages, otherItems, 0, 1000), CONFLICT);
	CHECK(metaOf(s).mode == MODE_IMPORTING && metaOf(s).revision == 1);
	CHECK(readRows(s, &readPages, &readItems));
	CHECK(samePages(readPages, pages) && sameItems(readItems, items));
	CHECK_ST(s, s.setMode(MODE_IMPORTING, MODE_LIVE, 1000), OK);
	addOp(s, "Alice");
	Id second = addOp(s, "Alice");
	CHECK_ST(s, s.readAll(&all, &readPages, &readItems, &ops, &maxAll), OK);
	CHECK_ST(s, s.maxOpId(&maxRead), OK);
	CHECK(sameMeta(all, metaOf(s)));
	CHECK(maxAll == maxRead && maxAll == second);
	CHECK_EQ(ops.size(), 2);
	CHECK_ST(s, s.readAll(NULL, &readPages, &readItems, &ops, NULL), OK);

	CHECK_ST(s, s.readAll(NULL, NULL, &readItems, &ops), INVALID);
	CHECK_ST(s, s.readAll(NULL, &readPages, NULL, &ops), INVALID);
	CHECK_ST(s, s.readAll(NULL, &readPages, &readItems, NULL), INVALID);
	CHECK_ST(s, s.importStash(pages, items, 0, 1000), CONFLICT);
	std::vector<PageRow> afterPages;
	std::vector<ItemRow> afterItems;
	CHECK(readRows(s, &afterPages, &afterItems));
	CHECK(samePages(afterPages, pages) && sameItems(afterItems, items));

	CHECK(rawRun(path, "DELETE FROM cells WHERE item = 11 AND x = 1 AND y = 2"));
	problem.clear();
	CHECK_ST(s, s.check(&problem), INVALID);
	CHECK(!problem.empty());
	s.close();

	std::string goldPath = runPath("t-d3-gold.sqlite3");
	Store g;
	CHECK_ST(g, g.create(goldPath.c_str(), metaFor(10, 10), 1000), OK);
	CHECK_ST(g, g.importStash(pages, items, 4294967295UL, 1000), OK);
	CHECK(metaOf(g).gold == 4294967295UL);
	CHECK_ST(g, g.check(&problem), OK);
}


/*============================= T-D4 ==========================*/

static void checkStillEmpty(Store& s, int line)
{
	std::vector<PageRow> pages;
	std::vector<ItemRow> items;
	bool empty = readRows(s, &pages, &items) && pages.empty() && items.empty() && metaOf(s).mode == MODE_IMPORTING;
	check(empty, "pages and items empty, mode IMPORTING", line);
}

static void importCase(const char* name, const std::vector<PageRow>& pages, const std::vector<ItemRow>& items,
	Status expected, int line)
{
	Store s;
	std::string path = runPath(name);
	if (s.create(path.c_str(), metaFor(10, 10), 1000) != OK)
	{
		check(false, s.error(), line);
		return;
	}
	Status status = s.importStash(pages, items, 0, 1000);
	checkStatus(status, expected, name, s.error(), line);
	if (expected != OK)
		checkStillEmpty(s, line);
}

static void testImportRefusals()
{
	printf("T-D4 import refusals\n");
	std::vector<PageRow> pages = onePage();
	std::vector<ItemRow> overlap;
	overlap.push_back(itemOf(1, 1, 0, 0, 2, 2));
	overlap.push_back(itemOf(2, 1, 1, 1));
	importCase("t-d4-overlap.sqlite3", pages, overlap, STORE_ERROR, __LINE__);
	overlap[1].x = 2;
	importCase("t-d4-overlap-ok.sqlite3", pages, overlap, OK, __LINE__);

	std::vector<ItemRow> zeroId(1, itemOf(0, 1, 0, 0));
	importCase("t-d4-id.sqlite3", pages, zeroId, INVALID, __LINE__);
	zeroId[0].id = 1;
	importCase("t-d4-id-ok.sqlite3", pages, zeroId, OK, __LINE__);

	std::vector<PageRow> gap = pages;
	gap.push_back(pageOf(2, 2));
	importCase("t-d4-ordinals.sqlite3", gap, std::vector<ItemRow>(), INVALID, __LINE__);
	gap[1].ordinal = 1;
	importCase("t-d4-ordinals-ok.sqlite3", gap, std::vector<ItemRow>(), OK, __LINE__);

	// Inside the schema's 15 x 15, outside this database's 10 x 10 grid.
	std::vector<ItemRow> outside(1, itemOf(1, 1, 10, 0));
	importCase("t-d4-grid.sqlite3", pages, outside, INVALID, __LINE__);
	outside[0].x = 9;
	importCase("t-d4-grid-ok.sqlite3", pages, outside, OK, __LINE__);
}


/*============================= T-D5 ==========================*/

static void testSettle()
{
	printf("T-D5 settleOps\n");
	std::string path = runPath("t-d5.sqlite3");
	std::vector<ItemRow> items;
	items.push_back(itemOf(1, 1, 0, 0));
	items.push_back(itemOf(2, 1, 2, 0));
	Store s;
	if (!makeLive(s, path, onePage(), items))
	{
		CHECK(false);
		return;
	}
	Meta meta = metaOf(s);
	Id op1 = addOp(s, "Carl");
	CHECK_ST(s, s.testInsertDeposit(op1, itemOf(0, 1, 5, 5), 1000), OK);
	CHECK_ST(s, s.testAttachItem(op1, 2, PENDING_WITHDRAWN, 1000), OK);
	Id op2 = addOp(s, "Carl");
	Id rev = revisionOf(s);
	Id ids[2] = {op1, op2};
	Id bad = 77;
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, ids, 2, 1000, &bad), OK);
	CHECK_EQ(bad, 0);
	std::vector<PageRow> pages;
	std::vector<ItemRow> rows;
	CHECK(readRows(s, &pages, &rows));
	const ItemRow* deposit = itemAt(rows, 5, 5);
	CHECK(deposit && deposit->pendingOp == 0 && deposit->pendingKind == PENDING_NONE && deposit->version == 2);
	CHECK(itemAt(rows, 2, 0) == NULL);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM cells WHERE item = 2"), 0);
	CHECK(opStateOf(s, op1) == OP_SETTLED && opStateOf(s, op2) == OP_SETTLED);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM ops WHERE state = 1 AND resolved_utc IS NOT NULL"), 2);
	CHECK_EQ(revisionOf(s), rev + 1);

	rev = revisionOf(s);
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, ids, 2, 1000, &bad), OK);
	CHECK_EQ(revisionOf(s), rev);

	// A reverted id fails the whole batch, the pending op before it included.
	Id reverted = addOp(s, "Carl");
	DWORD count = 0;
	CHECK_ST(s, s.revertPendingOps("Carl", 1000, &count), OK);
	CHECK_EQ(count, 1);
	Id op3 = addOp(s, "Carl");
	CHECK_ST(s, s.testInsertDeposit(op3, itemOf(0, 1, 6, 6), 1000), OK);
	rev = revisionOf(s);
	Id batch[2] = {op3, reverted};
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, batch, 2, 1000, &bad), CONFLICT);
	CHECK_EQ(bad, reverted);
	CHECK(opStateOf(s, op3) == OP_PENDING);
	CHECK(readRows(s, &pages, &rows));
	CHECK(itemAt(rows, 6, 6) && itemAt(rows, 6, 6)->pendingOp == op3);
	CHECK_EQ(revisionOf(s), rev);

	Id missing[2] = {op3, 9999};
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, missing, 2, 1000, &bad), NOT_FOUND);
	CHECK_EQ(bad, 9999);
	CHECK(opStateOf(s, op3) == OP_PENDING);

	unsigned char wrongGuid[16];
	memcpy(wrongGuid, meta.dbGuid, 16);
	wrongGuid[0] ^= 0xFF;
	bad = 77;
	CHECK_ST(s, s.settleOps("Carl", wrongGuid, &op3, 1, 1000, &bad), FOREIGN);
	CHECK_EQ(bad, 0);
	CHECK(opStateOf(s, op3) == OP_PENDING);

	bad = 77;
	CHECK_ST(s, s.settleOps("Carl", wrongGuid, &op3, 0, 1000, &bad), OK);
	CHECK_EQ(bad, 0);
	CHECK_EQ(revisionOf(s), rev);

	CHECK_ST(s, s.settleOps("CARL", meta.dbGuid, &op3, 1, 1000, &bad), OK);
	CHECK(opStateOf(s, op3) == OP_SETTLED);

	// Control: another character's op.
	Id other = addOp(s, "Dora");
	rev = revisionOf(s);
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, &other, 1, 1000, &bad), CONFLICT);
	CHECK_EQ(bad, other);
	CHECK(opStateOf(s, other) == OP_PENDING);
	CHECK_EQ(revisionOf(s), rev);
}


/*============================= T-D6 ==========================*/

static void testRevert()
{
	printf("T-D6 revertPendingOps\n");
	std::string path = runPath("t-d6.sqlite3");
	Store s;
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>(1, itemOf(1, 1, 0, 0))))
	{
		CHECK(false);
		return;
	}
	Id opA = addOp(s, "Carl");
	CHECK_ST(s, s.testInsertDeposit(opA, itemOf(0, 1, 3, 3), 1000), OK);
	CHECK_ST(s, s.testAttachItem(opA, 1, PENDING_WITHDRAWN, 1000), OK);
	Id opB = addOp(s, "Carl");
	Id opE = addOp(s, "Eve");
	CHECK_ST(s, s.testInsertDeposit(opE, itemOf(0, 1, 7, 7), 1000), OK);
	Id opS = addOp(s, "Carl");
	CHECK_EQ(settleOne(s, "Carl", opS, 1000), OK);

	Id rev = revisionOf(s);
	DWORD reverted = 0;
	CHECK_ST(s, s.revertPendingOps("Carl", 1000, &reverted), OK);
	CHECK_EQ(reverted, 2);
	std::vector<PageRow> pages;
	std::vector<ItemRow> rows;
	CHECK(readRows(s, &pages, &rows));
	CHECK(itemAt(rows, 3, 3) == NULL);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM cells WHERE x = 3 AND y = 3"), 0);
	const ItemRow* withdrawn = itemAt(rows, 0, 0);
	CHECK(withdrawn && withdrawn->pendingOp == 0 && withdrawn->pendingKind == PENDING_NONE && withdrawn->version == 2);
	CHECK(opStateOf(s, opA) == OP_REVERTED && opStateOf(s, opB) == OP_REVERTED);
	CHECK_EQ(revisionOf(s), rev + 1);
	CHECK(opStateOf(s, opE) == OP_PENDING);
	CHECK(itemAt(rows, 7, 7) && itemAt(rows, 7, 7)->pendingOp == opE);
	CHECK(opStateOf(s, opS) == OP_SETTLED);

	rev = revisionOf(s);
	CHECK_ST(s, s.revertPendingOps("Carl", 1000, &reverted), OK);
	CHECK_EQ(reverted, 0);
	CHECK_EQ(revisionOf(s), rev);
}


/*============================= T-D7 ==========================*/

static void testRevertFits()
{
	printf("T-D7 a pending withdrawal keeps its cells\n");
	std::string path = runPath("t-d7.sqlite3");
	Store s;
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>(1, itemOf(1, 1, 0, 0, 2, 2))))
	{
		CHECK(false);
		return;
	}
	Id withdrawal = addOp(s, "Carl");
	CHECK_ST(s, s.testAttachItem(withdrawal, 1, PENDING_WITHDRAWN, 1000), OK);
	Id deposit = addOp(s, "Dan");
	CHECK_ST(s, s.testInsertDeposit(deposit, itemOf(0, 1, 1, 1), 1000), STORE_ERROR);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM items"), 1);
	CHECK_EQ(settleOne(s, "Carl", withdrawal, 1000), OK);
	CHECK_ST(s, s.testInsertDeposit(deposit, itemOf(0, 1, 1, 1), 1000), OK);
}


/*============================= T-D8 ==========================*/

static void testModeGuard()
{
	printf("T-D8 mode guard\n");
	std::string path = runPath("t-d8.sqlite3");
	std::vector<ItemRow> items;
	items.push_back(itemOf(1, 1, 0, 0));
	items.push_back(itemOf(2, 1, 1, 0));
	Store s;
	if (!makeLive(s, path, onePage(), items))
	{
		CHECK(false);
		return;
	}
	// Control: the three test calls in mode LIVE.
	Id opA = 0;
	CHECK_ST(s, s.testAddPendingOp("Carl", &opA, 1000), OK);
	CHECK_ST(s, s.testAttachItem(opA, 1, PENDING_WITHDRAWN, 1000), OK);
	CHECK_ST(s, s.testInsertDeposit(opA, itemOf(0, 1, 5, 5), 1000), OK);

	Id opS = addOp(s, "Sam");
	CHECK_EQ(settleOne(s, "Sam", opS, 1000), OK);
	__int64 itemCount = rawQuery(path, "SELECT count(*) FROM items");
	CHECK_ST(s, s.testInsertDeposit(9999, itemOf(0, 1, 6, 6), 1000), NOT_FOUND);
	CHECK_ST(s, s.testInsertDeposit(opS, itemOf(0, 1, 6, 6), 1000), CONFLICT);
	CHECK_ST(s, s.testInsertDeposit(opA, itemOf(0, 1, 10, 0), 1000), INVALID);
	ItemRow empty = itemOf(0, 1, 6, 6);
	empty.data.clear();
	CHECK_ST(s, s.testInsertDeposit(opA, empty, 1000), INVALID);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM items"), itemCount);

	// An op 31 days old stays through a settle, a revert and a close/open: ops are never pruned.
	Id old = addOp(s, "Old");
	char sql[128];
	_snprintf(sql, sizeof(sql) - 1, "UPDATE ops SET created_utc = created_utc - 2678400 WHERE id = %I64d", old);
	sql[sizeof(sql) - 1] = 0;
	CHECK(rawRun(path, sql));

	// The Store keeps no copy of the mode: another connection changes it between calls.
	CHECK(rawRun(path, "UPDATE meta SET mode = 2"));
	Id unused = 0;
	CHECK_ST(s, s.testAddPendingOp("Carl", &unused, 1000), CONFLICT);
	CHECK_ST(s, s.testAttachItem(opA, 2, PENDING_WITHDRAWN, 1000), CONFLICT);
	CHECK_ST(s, s.testInsertDeposit(opA, itemOf(0, 1, 6, 6), 1000), CONFLICT);
	DWORD reverted = 9;
	CHECK_ST(s, s.revertPendingOps("Nobody", 1000, &reverted), OK);
	CHECK_EQ(reverted, 0);
	CHECK_ST(s, s.revertPendingOps("Carl", 1000, &reverted), CONFLICT);

	Meta meta = metaOf(s);
	Id bad = 0;
	CHECK_ST(s, s.settleOps("Sam", meta.dbGuid, &opS, 1, 1000, &bad), OK);
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, &opA, 1, 1000, &bad), CONFLICT);
	CHECK_EQ(bad, opA);
	Id missing = 9999;
	CHECK_ST(s, s.settleOps("Carl", meta.dbGuid, &missing, 1, 1000, &bad), NOT_FOUND);
	CHECK_EQ(bad, 9999);
	Id settledThenMissing[2] = {opS, 9999};
	CHECK_ST(s, s.settleOps("Sam", meta.dbGuid, settledThenMissing, 2, 1000, &bad), NOT_FOUND);
	CHECK_EQ(bad, 9999);

	CHECK_ST(s, s.setMode(MODE_CONVERTING_BACK, MODE_LIVE, 1000), OK);
	CHECK(metaOf(s).mode == MODE_LIVE);
	CHECK_EQ(settleOne(s, "Old", old, 1000), OK);
	std::vector<std::string> names;
	CHECK_ST(s, s.beginConvertBack(&names, 1000), CONFLICT);
	CHECK(names.size() == 1 && names[0] == "Carl");
	CHECK(metaOf(s).mode == MODE_LIVE);
	CHECK_ST(s, s.revertPendingOps("Carl", 1000, &reverted), OK);
	CHECK_EQ(reverted, 1);
	CHECK_ST(s, s.beginConvertBack(&names, 1000), OK);
	CHECK(names.empty());
	CHECK(metaOf(s).mode == MODE_CONVERTING_BACK);

	CHECK(rawRun(path, "UPDATE meta SET mode = 0"));
	CHECK_ST(s, s.beginConvertBack(&names, 1000), CONFLICT);
	CHECK(metaOf(s).mode == MODE_IMPORTING);
	CHECK_ST(s, s.setMode(MODE_IMPORTING, MODE_LIVE, 1000), OK);
	CHECK(metaOf(s).mode == MODE_LIVE);
	CHECK_ST(s, s.setMode(MODE_IMPORTING, MODE_LIVE, 1000), CONFLICT);
	CHECK_ST(s, s.setMode(MODE_CONVERTING_BACK, MODE_LIVE, 1000), CONFLICT);
	CHECK_ST(s, s.setMode(MODE_LIVE, MODE_IMPORTING, 1000), INVALID);
	CHECK_ST(s, s.setMode(MODE_LIVE, MODE_CONVERTING_BACK, 1000), INVALID);
	CHECK_ST(s, s.setMode(MODE_LIVE, MODE_BACKUP_COPY, 1000), INVALID);
	CHECK_ST(s, s.setMode(MODE_LIVE, MODE_LIVE, 1000), INVALID);
	CHECK_ST(s, s.setMode(MODE_BACKUP_COPY, MODE_LIVE, 1000), INVALID);
	CHECK(metaOf(s).mode == MODE_LIVE);

	s.close();
	CHECK_ST(s, s.open(path.c_str()), OK);
	CHECK(opStateOf(s, old) == OP_SETTLED);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM ops"), 3);
}


/*============================= T-D9 ==========================*/

static void testWriteLock()
{
	printf("T-D9 write lock\n");
	std::string path = runPath("t-d9.sqlite3");
	std::string lockName = objectName("Global", "m");
	Store s;
	s.testSetWriteLockName(lockName.c_str());
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	Id op1 = addOp(s, "Carl");
	Id rev = revisionOf(s);

	Holder holder;
	CHECK(startHolder("hold", lockName, &holder));
	ULONGLONG start = nowMs();
	CHECK_EQ(settleOne(s, "Carl", op1, 100), BUSY);
	ULONGLONG took = nowMs() - start;
	printf("  T-D9 mutex held: BUSY after %I64u ms (bounds 60..999)\n", took);
	CHECK(took >= 60 && took < 1000);
	CHECK_EQ(revisionOf(s), rev);
	CHECK(opStateOf(s, op1) == OP_PENDING);
	sqlite3* raw = rawOpen(path);
	sqlite3_busy_timeout(raw, 0);
	CHECK(rawExec(raw, "BEGIN IMMEDIATE"));
	CHECK(rawExec(raw, "ROLLBACK"));
	sqlite3_close(raw);
	CHECK_EQ(stopHolder(&holder), 0);
	CHECK_EQ(settleOne(s, "Carl", op1, 100), OK);

	// A writer outside the mutex: the per-call busy timeout bounds the wait.
	Id op2 = addOp(s, "Carl");
	CHECK(startHolder("rawlock", path, &holder));
	start = nowMs();
	CHECK_EQ(settleOne(s, "Carl", op2, 1000), BUSY);
	took = nowMs() - start;
	printf("  T-D9 SQLite lock held: BUSY after %I64u ms (bounds 900..2499)\n", took);
	CHECK(took >= 900 && took < 2500);
	CHECK_EQ(stopHolder(&holder), 0);
	CHECK(opStateOf(s, op2) == OP_PENDING);

	CHECK(startHolder("hold", lockName, &holder));
	CHECK_ST(s, s.checkpointTruncate(100), BUSY);
	CHECK_EQ(stopHolder(&holder), 0);
}


/*============================= T-D10 ==========================*/

static void testAbandoned()
{
	printf("T-D10 a writer killed inside its transaction\n");
	std::string path = runPath("t-d10.sqlite3");
	std::string lockName = objectName("Global", "m");
	Store s;
	s.testSetWriteLockName(lockName.c_str());
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	std::string readyName;
	HANDLE ready = makeEvent(&readyName);
	HANDLE child = startChild("killwriter " + quoted(path) + " " + quoted(lockName) + " " + quoted(readyName));
	CHECK(waitReady(ready, child));
	CHECK(killChild(child));
	CloseHandle(ready);
	EXPECT_NO_LOG();
	Id op = 0;
	CHECK_ST(s, s.testAddPendingOp("Carl", &op, 2000), OK);
	EXPECT_LOG("write lock abandoned", 1);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM ops WHERE character = 'Killed'"), 0);
}


/*============================= T-D11 ==========================*/

static void testRace()
{
	printf("T-D11 eight processes\n");
	std::string path = runPath("t-d11.sqlite3");
	std::vector<PageRow> pages;
	for (int i = 0; i < 8; i++)
		pages.push_back(pageOf(i + 1, i));
	Store s;
	if (!makeLive(s, path, pages, std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	Id revBefore = revisionOf(s);
	s.close();

	std::string startName = objectName("Local", "start");
	std::string readyName = objectName("Local", "ready");
	HANDLE start = CreateEventA(NULL, TRUE, FALSE, startName.c_str());
	HANDLE ready = CreateSemaphoreA(NULL, 0, 8, readyName.c_str());
	HANDLE children[8] = {NULL};
	std::string results[8];
	bool started = start && ready;
	for (int i = 0; i < 8 && started; i++)
	{
		char result[32];
		_snprintf(result, sizeof(result) - 1, "t-d11-racer%d.txt", i);
		result[sizeof(result) - 1] = 0;
		results[i] = runPath(result);
		char index[8];
		_snprintf(index, sizeof(index) - 1, "%d", i);
		index[sizeof(index) - 1] = 0;
		children[i] = startChild("race " + quoted(path) + " " + index + " " + quoted(startName) + " "
			+ quoted(readyName) + " " + quoted(results[i]));
		started = children[i] != NULL;
	}
	CHECK(started);
	for (int i = 0; i < 8 && started; i++)
		started = WaitForSingleObject(ready, 15000) == WAIT_OBJECT_0;
	CHECK(started);
	if (!started)
	{
		for (int i = 0; i < 8; i++)
			if (children[i])
				killChild(children[i]);
		CloseHandle(start);
		CloseHandle(ready);
		return;
	}
	ULONGLONG began = nowMs();
	SetEvent(start);
	DWORD wait = WaitForMultipleObjects(8, children, TRUE, 60000);
	ULONGLONG took = nowMs() - began;
	CHECK(wait <= WAIT_OBJECT_0 + 7);

	int sumChanges = 0;
	std::string counts;
	for (int i = 0; i < 8; i++)
	{
		DWORD code = waitExit(children[i], wait <= WAIT_OBJECT_0 + 7 ? 15000 : 0);
		int changes = -1;
		int busy = -1;
		int unexpected = -1;
		FILE* f = fopen(results[i].c_str(), "r");
		if (f)
		{
			if (fscanf(f, "%d %d %d", &changes, &busy, &unexpected) != 3)
				changes = -1;
			fclose(f);
		}
		CHECK_EQ(code, 0);
		CHECK(changes >= 0 && unexpected == 0);
		if (unexpected > 0)
			printf("  racer %d: unexpected status %s\n", i, statusName((Status)(unexpected - CHILD_STATUS_BASE)));
		sumChanges += changes > 0 ? changes : 0;
		char one[32];
		_snprintf(one, sizeof(one) - 1, " %d/%d", changes, busy);
		one[sizeof(one) - 1] = 0;
		counts += one;
	}
	CloseHandle(start);
	CloseHandle(ready);
	printf("race wall time %I64u ms (budget 60000); changes/BUSY per racer:%s\n", took, counts.c_str());

	CHECK_ST(s, s.open(path.c_str()), OK);
	std::string problem;
	CHECK_ST(s, s.check(&problem), OK);
	CHECK_EQ(revisionOf(s), revBefore + sumChanges);
}


/*============================= T-D12 ==========================*/

// A mount-point reparse point at link (an empty directory) to target; no privilege needed.
struct MountPointBuffer
{
	DWORD ReparseTag;
	WORD ReparseDataLength;
	WORD Reserved;
	WORD SubstituteNameOffset;
	WORD SubstituteNameLength;
	WORD PrintNameOffset;
	WORD PrintNameLength;
	WCHAR PathBuffer[2 * MAX_PATH + 8];
};

static bool makeJunction(const std::string& link, const std::string& target)
{
	wchar_t wideTarget[MAX_PATH];
	if (!MultiByteToWideChar(CP_ACP, 0, target.c_str(), -1, wideTarget, MAX_PATH))
		return false;
	std::wstring substitute = std::wstring(L"\\??\\") + wideTarget;
	std::wstring print = wideTarget;
	MountPointBuffer buffer;
	memset(&buffer, 0, sizeof(buffer));
	WORD substituteBytes = (WORD)(substitute.size() * sizeof(WCHAR));
	WORD printBytes = (WORD)(print.size() * sizeof(WCHAR));
	buffer.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	buffer.SubstituteNameOffset = 0;
	buffer.SubstituteNameLength = substituteBytes;
	buffer.PrintNameOffset = substituteBytes + sizeof(WCHAR);
	buffer.PrintNameLength = printBytes;
	memcpy(buffer.PathBuffer, substitute.c_str(), substituteBytes + sizeof(WCHAR));
	memcpy((BYTE*)buffer.PathBuffer + substituteBytes + sizeof(WCHAR), print.c_str(), printBytes + sizeof(WCHAR));
	buffer.ReparseDataLength = (WORD)(8 + substituteBytes + sizeof(WCHAR) + printBytes + sizeof(WCHAR));
	HANDLE h = CreateFileA(link.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD bytes = 0;
	BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &buffer, 8 + buffer.ReparseDataLength, NULL, 0, &bytes, NULL);
	CloseHandle(h);
	return ok != 0;
}

static void testPaths()
{
	printf("T-D12 paths\n");
	const wchar_t wideFolder[] = L"t-d12-\x00E9t\x00E9";
	char ansiFolder[MAX_PATH];
	BOOL usedDefault = FALSE;
	int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wideFolder, -1, ansiFolder, MAX_PATH, NULL, &usedDefault);
	if (!n || usedDefault)
		printf("SKIP T-D12 non-ASCII folder: code page %u cannot represent it\n", GetACP());
	else
	{
		std::string folder = runPath(ansiFolder);
		CHECK(CreateDirectoryA(folder.c_str(), NULL));
		std::string db = folder + "\\x.sqlite3";
		Store s;
		CHECK_ST(s, s.create(db.c_str(), metaFor(10, 10), 1000), OK);
		s.close();
		CHECK_ST(s, s.open(db.c_str()), OK);
		CHECK(metaOf(s).gridW == 10);
	}

	char canonical[MAX_PATH];
	ULONGLONG start = nowMs();
	CHECK(LocalDatabasePath("\\\\127.0.0.1\\NoSuchShare\\x.sqlite3", canonical) == LOCAL_UNC);
	printf("  T-D12 UNC answer after %I64u ms\n", nowMs() - start);

	std::string file = runPath("t-d12-file.sqlite3");
	CHECK(writeBytes(file, Bytes(16, 0x41)));
	CHECK(LocalDatabasePath(file.c_str(), canonical) == LOCAL_OK);
	if (!CreateHardLinkA(runPath("t-d12-link.sqlite3").c_str(), file.c_str(), NULL))
		printf("SKIP T-D12 hard link: the volume refused it (error %lu)\n", GetLastError());
	else
		CHECK(LocalDatabasePath(file.c_str(), canonical) == LOCAL_HARD_LINK);

	CHECK(LocalDatabasePath(runPath("t-d12-missing.sqlite3").c_str(), canonical) == LOCAL_OK);
	CHECK(_stricmp(canonical, runPath("t-d12-missing.sqlite3").c_str()) == 0);
	std::string dir = runPath("t-d12-dir.sqlite3");
	CHECK(CreateDirectoryA(dir.c_str(), NULL));
	CHECK(LocalDatabasePath(dir.c_str(), canonical) == LOCAL_REPARSE);

	std::string target = runPath("t-d12-target");
	std::string junction = runPath("t-d12-junction.sqlite3");
	CHECK(CreateDirectoryA(target.c_str(), NULL));
	CHECK(CreateDirectoryA(junction.c_str(), NULL));
	if (!makeJunction(junction, target))
		printf("SKIP T-D12 junction: the volume refused it (error %lu)\n", GetLastError());
	else
	{
		CHECK((GetFileAttributesA(junction.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
		CHECK(LocalDatabasePath(junction.c_str(), canonical) == LOCAL_REPARSE);
	}
	CHECK(LocalDatabasePath(runPath("no-such-folder\\x.sqlite3").c_str(), canonical) == LOCAL_ERROR);
}


/*============================= T-D13 ==========================*/

static void testBackup()
{
	printf("T-D13 backupTo\n");
	std::string path = runPath("t-d13.sqlite3");
	std::string lockName = objectName("Global", "m");
	std::vector<ItemRow> items;
	items.push_back(itemOf(1, 1, 0, 0));
	items.push_back(itemOf(2, 1, 3, 3, 2, 2));
	Store s;
	s.testSetWriteLockName(lockName.c_str());
	if (!makeLive(s, path, onePage(), items))
	{
		CHECK(false);
		return;
	}
	std::string target = runPath("t-d13-backup.sqlite3");
	std::set<std::string> before = listRun();
	Holder holder;
	// The holder keeps the mutex until released (up to 15 s), so a backupTo that waited on it would run long.
	CHECK(startHolder("hold", lockName, &holder));
	ULONGLONG start = nowMs();
	CHECK_ST(s, s.backupTo(target.c_str()), OK);
	ULONGLONG took = nowMs() - start;
	printf("  T-D13 backupTo with the write mutex held: %I64u ms (bound 1999)\n", took);
	CHECK(took < 2000);
	CHECK_EQ(stopHolder(&holder), 0);
	std::set<std::string> expected = before;
	expected.insert(baseName(target));
	CHECK(listRun() == expected);

	Bytes copyBytes;
	CHECK(readBytes(target, &copyBytes) && copyBytes.size() > 19 && copyBytes[18] == 1 && copyBytes[19] == 1);
	Store copy;
	CHECK_ST(copy, copy.open(target.c_str(), true), OK);
	std::string problem;
	CHECK_ST(copy, copy.check(&problem), OK);
	CHECK(metaOf(copy).mode == MODE_BACKUP_COPY);
	std::vector<PageRow> sourcePages;
	std::vector<ItemRow> sourceItems;
	std::vector<PageRow> copyPages;
	std::vector<ItemRow> copyItems;
	CHECK(readRows(s, &sourcePages, &sourceItems) && readRows(copy, &copyPages, &copyItems));
	CHECK(samePages(copyPages, sourcePages) && sameItems(copyItems, sourceItems));
	copy.close();
	CHECK(!exists(target + "-wal") && !exists(target + "-shm") && !exists(target + "-journal"));
	CHECK(metaOf(s).mode == MODE_LIVE);

	// An existing target is replaced, and no temp file stays.
	addOp(s, "Carl");
	CHECK_ST(s, s.backupTo(target.c_str()), OK);
	CHECK(listRun() == expected);
	CHECK_ST(copy, copy.open(target.c_str(), true), OK);
	CHECK_EQ(rawQuery(target, "SELECT count(*) FROM ops"), 1);
	copy.close();

	before = listRun();
	CHECK(s.backupTo(runPath("no-such-folder\\b.sqlite3").c_str()) != OK);
	CHECK(listRun() == before);

	Bytes targetBefore;
	Bytes targetAfter;
	Bytes planted;
	CHECK(readBytes(target, &targetBefore));
	CHECK(writeBytes(target + "-journal", Bytes(1, 0x5A)));
	before = listRun();
	CHECK_ST(s, s.backupTo(target.c_str()), INVALID);
	CHECK(readBytes(target, &targetAfter) && targetAfter == targetBefore);
	CHECK(readBytes(target + "-journal", &planted) && planted.size() == 1 && planted[0] == 0x5A);
	CHECK(listRun() == before);
}


/*============================= T-D14 ==========================*/

static void testWalPairing(bool* blocked)
{
	printf("T-D14 WAL pairing, store half\n");
	std::string path = runPath("t-d14.sqlite3");
	std::string readyName;
	HANDLE ready = makeEvent(&readyName);
	HANDLE child = startChild("storecreate " + quoted(path) + " " + quoted(readyName));
	CHECK(waitReady(ready, child));
	CHECK(fileSize(path + "-wal") > 0);
	CHECK(killChild(child));
	CloseHandle(ready);

	Store s;
	CHECK_ST(s, s.open(path.c_str()), OK);
	CHECK_ST(s, s.checkpointTruncate(1000), OK);
	s.close();
	CHECK(!exists(path + "-wal") && !exists(path + "-shm"));
	std::string failed = path + ".failed-test";
	CHECK(MoveFileA(path.c_str(), failed.c_str()));
	CHECK_ST(s, s.open(failed.c_str()), OK);
	std::vector<PageRow> pages;
	std::vector<ItemRow> items;
	CHECK(readRows(s, &pages, &items) && pages.size() == 1 && items.size() == 3);
	s.close();
	CHECK_ST(s, s.create(path.c_str(), metaFor(10, 10), 1000), OK);
	CHECK(readRows(s, &pages, &items) && pages.empty() && items.empty());

	// Another connection keeps the main file from being renamed.
	Store second;
	CHECK_ST(second, second.open(path.c_str()), OK);
	BOOL moved = MoveFileA(path.c_str(), (path + ".moved").c_str());
	DWORD error = moved ? 0 : GetLastError();
	printf("  T-D14 rename of an open database: %s, error %lu\n", moved ? "moved" : "refused", error);
	CHECK(!moved && error == ERROR_SHARING_VIOLATION);
	if (moved || error != ERROR_SHARING_VIOLATION)
		*blocked = true;
	if (moved)
		MoveFileA((path + ".moved").c_str(), path.c_str());
	second.close();
	s.close();

	std::string orphan = runPath("t-d14-orphan.sqlite3");
	Bytes walBytes(8, 0x33);
	CHECK(writeBytes(orphan + "-wal", walBytes));
	Store o;
	CHECK_ST(o, o.create(orphan.c_str(), metaFor(10, 10), 1000), INVALID);
	Bytes walAfter;
	CHECK(readBytes(orphan + "-wal", &walAfter) && walAfter == walBytes);
	CHECK(!exists(orphan));
}


/*============================= T-D15 ==========================*/

static void testLockUnavailable()
{
	printf("T-D15 write lock unavailable\n");
	std::string path = runPath("t-d15.sqlite3");
	Store setup;
	if (!makeLive(setup, path, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	setup.close();

	std::string lockName = objectName("Global", "m");
	Holder holder;
	CHECK(startHolder("emptydacl", lockName, &holder));
	EXPECT_NO_LOG();
	Store s;
	s.testSetWriteLockName(lockName.c_str());
	CHECK_ST(s, s.open(path.c_str()), OK);
	Id op = 0;
	CHECK_ST(s, s.testAddPendingOp("Carl", &op, 1000), OK);
	CHECK_ST(s, s.testAddPendingOp("Carl", &op, 1000), OK);
	EXPECT_LOG("write lock unavailable", 1);
	s.close();
	CHECK_EQ(stopHolder(&holder), 0);

	Store control;
	control.testSetWriteLockName(objectName("Global", "m").c_str());
	CHECK_ST(control, control.open(path.c_str()), OK);
	CHECK_ST(control, control.testAddPendingOp("Carl", &op, 1000), OK);
	CHECK_ST(control, control.testAddPendingOp("Carl", &op, 1000), OK);
	EXPECT_NO_LOG();
}


/*============================= T-D16 ==========================*/

static void testOpCap()
{
	printf("T-D16 pending op cap\n");
	std::string path = runPath("t-d16.sqlite3");
	Store s;
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	CHECK(rawRun(path, "BEGIN; WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1023)"
		" INSERT INTO ops(character, state, created_utc) SELECT 'Capped', 0, 0 FROM n; COMMIT"));
	Id op = 0;
	CHECK_ST(s, s.testAddPendingOp("Capped", &op, 1000), OK);
	CHECK_ST(s, s.testAddPendingOp("Capped", &op, 1000), BUSY);
	CHECK_ST(s, s.testAddPendingOp("Other", &op, 1000), OK);
	CHECK_EQ(rawQuery(path, "SELECT count(*) FROM ops WHERE character = 'Capped' AND state = 0"), LIVE_OPS_CAP);
}


/*============================= T-D17 ==========================*/

static void testLockName()
{
	printf("T-D17 lock name\n");
	std::string path = runPath("t-d17.sqlite3");
	Store s;
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	Id op = addOp(s, "Carl");
	char plain[MAX_PATH];
	char other[MAX_PATH];
	CHECK(CanonicalDatabasePath(path.c_str(), plain));
	CHECK(CanonicalDatabasePath(upper(path).c_str(), other) && !strcmp(plain, other));
	CHECK(CanonicalDatabasePath((std::string(runDir) + "\\.\\t-d17.sqlite3").c_str(), other) && !strcmp(plain, other));
	char shortDir[MAX_PATH];
	DWORD n = GetShortPathNameA(runDir, shortDir, MAX_PATH);
	if (!n || n >= MAX_PATH || !strcmp(shortDir, runDir))
		printf("SKIP T-D17 short folder name: the run folder has no 8.3 form on this volume\n");
	else
		CHECK(CanonicalDatabasePath((std::string(shortDir) + "\\t-d17.sqlite3").c_str(), other)
			&& !strcmp(plain, other));
	CHECK(CanonicalDatabasePath(runPath("t-d17-other.sqlite3").c_str(), other) && strcmp(plain, other) != 0);
	bool lower = true;
	for (const char* p = plain; *p; p++)
		lower = lower && !(*p >= 'A' && *p <= 'Z');
	CHECK(lower);
	CHECK(plain[0] >= 'a' && plain[0] <= 'z' && plain[1] == ':' && plain[2] == '\\');

	char root[16];
	char expectedRoot[16];
	_snprintf(root, sizeof(root) - 1, "%c:\\x.sqlite3", runDir[0]);
	root[sizeof(root) - 1] = 0;
	strcpy(expectedRoot, root);
	_strlwr(expectedRoot);
	CHECK(CanonicalDatabasePath(root, other) && !strcmp(other, expectedRoot));

	// Two spellings, one lock: a child holds it through the upper-case spelling.
	Holder holder;
	CHECK(startHolder("writelock", upper(path), &holder));
	CHECK_EQ(settleOne(s, "Carl", op, 100), BUSY);
	CHECK_EQ(stopHolder(&holder), 0);
	CHECK_EQ(settleOne(s, "Carl", op, 100), OK);

	std::string sss = runPath("t-d17-shared-stash-file.sss");
	CHECK(writeBytes(sss, Bytes(1, 0x53)));
	char shortSss[MAX_PATH];
	n = GetShortPathNameA(sss.c_str(), shortSss, MAX_PATH);
	if (!n || n >= MAX_PATH || !strcmp(shortSss, sss.c_str()))
		printf("SKIP T-D17 SharedStashMutexName short spelling: no 8.3 form on this volume\n");
	else
	{
		char longName[SHARED_STASH_MUTEX_NAME_SIZE];
		char shortName[SHARED_STASH_MUTEX_NAME_SIZE];
		SharedStashMutexName(sss.c_str(), longName);
		SharedStashMutexName(shortSss, shortName);
		CHECK(!strcmp(longName, shortName));
	}
}


/*============================= T-D18 ==========================*/

static void testCallerLock()
{
	printf("T-D18 caller-held WriteLock\n");
	std::string path = runPath("t-d18.sqlite3");
	Store s;
	if (!makeLive(s, path, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	Id mine = addOp(s, "Carl");
	Id theirs = addOp(s, "Cid");
	Id another = addOp(s, "Cid");
	Id control = addOp(s, "Cid");

	WriteLock lock;
	CHECK_ST(s, lock.acquire(path.c_str(), 1000), OK);
	CHECK(lock.owned());
	CHECK_EQ(settleOne(s, "Carl", mine, 100), OK);
	CHECK_EQ(runSettleChild(path, "Cid", theirs, 100, NULL), CHILD_STATUS_BASE + BUSY);
	lock.release();
	CHECK(!lock.owned());
	CHECK_EQ(runSettleChild(path, "Cid", theirs, 100, NULL), CHILD_STATUS_BASE + OK);
	EXPECT_NO_LOG();

	WriteLock upperLock;
	CHECK_ST(s, upperLock.acquire(upper(path).c_str(), 1000), OK);
	CHECK(upperLock.owned());
	CHECK_EQ(runSettleChild(path, "Cid", another, 100, NULL), CHILD_STATUS_BASE + BUSY);
	upperLock.release();

	WriteLock twice;
	CHECK_ST(s, twice.acquire(path.c_str(), 1000), OK);
	CHECK_ST(s, twice.acquire(path.c_str(), 1000), INVALID);
	CHECK(twice.owned());
	twice.release();
	twice.release();
	CHECK(!twice.owned());
	WriteLock none;
	CHECK_ST(s, none.acquire(NULL, 100), INVALID);
	WriteLock notYet;
	CHECK_ST(s, notYet.acquire(runPath("t-d18-not-yet.sqlite3").c_str(), 1000), OK);
	CHECK(notYet.owned());
	notYet.release();

	Holder holder;
	CHECK(startHolder("writelock", path, &holder));
	WriteLock waiting;
	ULONGLONG start = nowMs();
	CHECK_ST(s, waiting.acquire(path.c_str(), 100), BUSY);
	ULONGLONG took = nowMs() - start;
	printf("  T-D18 WriteLock held by a child: BUSY after %I64u ms (bounds 60..999)\n", took);
	CHECK(took >= 60 && took < 1000);
	CHECK(!waiting.owned());
	CHECK_EQ(stopHolder(&holder), 0);

	// The parent's open Store keeps the mutex object alive, so the killed holder abandons it.
	std::string readyName;
	std::string releaseName;
	HANDLE ready = makeEvent(&readyName);
	HANDLE release = makeEvent(&releaseName);
	HANDLE child = startChild("writelock " + quoted(path) + " " + quoted(readyName) + " " + quoted(releaseName));
	CHECK(waitReady(ready, child));
	CHECK(killChild(child));
	CloseHandle(ready);
	CloseHandle(release);
	WriteLock taken;
	CHECK_ST(s, taken.acquire(path.c_str(), 1000), OK);
	CHECK(taken.owned());
	EXPECT_LOG("write lock abandoned", 1);
	taken.release();

	start = nowMs();
	CHECK_EQ(runSettleChild(path, "Cid", control, 100, &took), CHILD_STATUS_BASE + OK);
	printf("  T-D18 no lock held: the child's settle took %I64u ms\n", took);
	s.close();

	// The computed name made first by someone we cannot open it for.
	std::string other = runPath("t-d18-b.sqlite3");
	Store b;
	if (!makeLive(b, other, onePage(), std::vector<ItemRow>()))
	{
		CHECK(false);
		return;
	}
	Id op = addOp(b, "Carl");
	b.close();
	char canonical[MAX_PATH];
	char lockName[SHARED_STASH_MUTEX_NAME_SIZE];
	CHECK(CanonicalDatabasePath(other.c_str(), canonical));
	LiveStashLockName(canonical, lockName);
	CHECK(startHolder("emptydacl", lockName, &holder));
	EXPECT_NO_LOG();
	WriteLock unavailable;
	CHECK_ST(s, unavailable.acquire(other.c_str(), 1000), OK);
	CHECK(!unavailable.owned());
	EXPECT_LOG("write lock unavailable", 1);
	CHECK_ST(b, b.open(other.c_str()), OK);
	EXPECT_LOG("write lock unavailable", 1);
	CHECK_EQ(settleOne(b, "Carl", op, 1000), OK);
	unavailable.release();
	b.close();
	CHECK_EQ(stopHolder(&holder), 0);
}


/*============================= T-D19 ==========================*/

static void testFailedCreate()
{
	printf("T-D19 failed and killed create\n");
	for (int step = 3; step <= 5; step++)
	{
		char name[64];
		_snprintf(name, sizeof(name) - 1, "t-d19-step%d.sqlite3", step);
		name[sizeof(name) - 1] = 0;
		std::string path = runPath(name);
		std::set<std::string> before = listRun();
		Store s;
		s.testFailCreate(step);
		Status status = s.create(path.c_str(), metaFor(10, 10), 1000);
		CHECK_EQ(status, STORE_ERROR);
		CHECK(strstr(s.error(), "test failure") != NULL);
		CHECK(listRun() == before);
		CHECK_ST(s, s.create(path.c_str(), metaFor(10, 10), 1000), OK);
		Meta meta;
		CHECK_ST(s, s.readMeta(&meta), OK);
	}
	// A create() refused before step 3 leaves the armed failure for the next one.
	Store kept;
	kept.testFailCreate(4);
	CHECK_ST(kept, kept.create(runPath("t-d19-kept.sqlite3").c_str(), metaFor(0, 10), 1000), INVALID);
	CHECK_ST(kept, kept.create(runPath("t-d19-kept.sqlite3").c_str(), metaFor(10, 10), 1000), STORE_ERROR);
	CHECK(strstr(kept.error(), "test failure") != NULL);
	CHECK_ST(kept, kept.create(runPath("t-d19-kept.sqlite3").c_str(), metaFor(10, 10), 1000), OK);
	kept.close();
	Store disarmed;
	disarmed.testFailCreate(1);
	CHECK_ST(disarmed, disarmed.create(runPath("t-d19-step1.sqlite3").c_str(), metaFor(10, 10), 1000), OK);
	disarmed.close();

	// Without the lock, another create() may be in the same window: nothing is cleaned.
	std::string lockName = objectName("Global", "m");
	Holder holder;
	CHECK(startHolder("emptydacl", lockName, &holder));
	std::string unlocked = runPath("t-d19-unlocked.sqlite3");
	EXPECT_NO_LOG();
	Store u;
	u.testSetWriteLockName(lockName.c_str());
	u.testFailCreate(4);
	CHECK_ST(u, u.create(unlocked.c_str(), metaFor(10, 10), 1000), STORE_ERROR);
	EXPECT_LOG("write lock unavailable", 1);
	CHECK(exists(unlocked));
	CHECK_EQ(stopHolder(&holder), 0);
	Store reopen;
	CHECK_ST(reopen, reopen.open(unlocked.c_str()), INVALID);
	Store recreate;
	CHECK_ST(recreate, recreate.create(unlocked.c_str(), metaFor(10, 10), 1000), INVALID);
	EXPECT_NO_LOG();

	std::string existing = runPath("t-d19-existing.sqlite3");
	Store e;
	CHECK_ST(e, e.create(existing.c_str(), metaFor(10, 10), 1000), OK);
	e.close();
	Bytes bytesBefore;
	Bytes bytesAfter;
	CHECK(readBytes(existing, &bytesBefore));
	std::set<std::string> before = listRun();
	CHECK_ST(e, e.create(existing.c_str(), metaFor(10, 10), 1000), INVALID);
	CHECK(readBytes(existing, &bytesAfter) && bytesAfter == bytesBefore);
	CHECK(listRun() == before);

	std::string killed = runPath("t-d19-killed.sqlite3");
	std::string readyName;
	HANDLE ready = makeEvent(&readyName);
	HANDLE child = startChild("killcreate " + quoted(killed) + " " + quoted(readyName));
	CHECK(waitReady(ready, child));
	CHECK(killChild(child));
	CloseHandle(ready);
	CHECK(exists(killed));
	Store k;
	CHECK_ST(k, k.open(killed.c_str()), INVALID);
	CHECK(readBytes(killed, &bytesBefore));
	CHECK_ST(k, k.create(killed.c_str(), metaFor(10, 10), 1000), INVALID);
	CHECK(readBytes(killed, &bytesAfter) && bytesAfter == bytesBefore);

	std::string headerOnly = runPath("t-d19-header-only.sqlite3");
	sqlite3* raw = rawOpen(headerOnly, true);
	CHECK(rawText(raw, "PRAGMA journal_mode=WAL") == "wal");
	sqlite3_close(raw);
	CHECK(exists(headerOnly));
	CHECK_ST(k, k.open(headerOnly.c_str()), INVALID);
}


/*============================= Run folder ==========================*/

// Removes dir and everything under it. A reparse point is removed itself, never entered.
static bool removeTree(const std::string& dir)
{
	std::vector<std::pair<std::string, DWORD> > entries;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
				entries.push_back(std::make_pair(dir + "\\" + fd.cFileName, fd.dwFileAttributes));
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	bool ok = true;
	for (size_t i = 0; i < entries.size(); i++)
	{
		const std::string& path = entries[i].first;
		DWORD attributes = entries[i].second;
		bool removed;
		if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
			removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(path.c_str()) != 0
				: DeleteFileA(path.c_str()) != 0;
		else if (attributes & FILE_ATTRIBUTE_DIRECTORY)
			removed = removeTree(path);
		else
		{
			if (attributes & FILE_ATTRIBUTE_READONLY)
				SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
			removed = DeleteFileA(path.c_str()) != 0;
		}
		if (!removed)
			printf("  cannot remove %s (error %lu)\n", path.c_str(), GetLastError());
		ok = ok && removed;
	}
	return RemoveDirectoryA(dir.c_str()) != 0 && ok;
}

int main(int argc, char** argv)
{
	if (argc >= 2 && !strcmp(argv[1], "--child"))
		return childMain(argc - 2, argv + 2);

	setvbuf(stdout, NULL, _IONBF, 0);
	ULONGLONG suiteStart = nowMs();
	char scratchArg[MAX_PATH] = "";
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--scratch") && i + 1 < argc)
			strncpy(scratchArg, argv[++i], MAX_PATH - 1);
		else
		{
			printf("usage: SharedStashStoreTests [--scratch DIR]\n");
			return 2;
		}
	}
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	char scratchDir[MAX_PATH];
	if (!scratchArg[0])
	{
		strcpy(scratchArg, exePath);
		char* slash = strrchr(scratchArg, '\\');
		strcpy(slash ? slash + 1 : scratchArg, "scratch");
	}
	DWORD n = GetFullPathNameA(scratchArg, MAX_PATH, scratchDir, NULL);
	if (!n || n >= MAX_PATH)
	{
		printf("bad --scratch %s\n", scratchArg);
		return 1;
	}
	CreateDirectoryA(scratchDir, NULL);
	_snprintf(nameTag, sizeof(nameTag) - 1, "%lu-%lu", GetCurrentProcessId(), GetTickCount());
	_snprintf(runDir, MAX_PATH - 1, "%s\\stash-%s", scratchDir, nameTag);
	runDir[MAX_PATH - 1] = 0;
	if (!CreateDirectoryA(runDir, NULL))
	{
		printf("cannot create %s (error %lu)\n", runDir, GetLastError());
		return 1;
	}
	job = CreateJobObjectA(NULL, NULL);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
	{
		printf("cannot create the child job (error %lu)\n", GetLastError());
		return 1;
	}
	SetLogSink(recordLog);
	printf("SharedStashStoreTests (SQLite %s): scratch %s\n", sqlite3_libversion(), runDir);

	bool blocked = false;
	testCreateAndGate();
	EXPECT_NO_LOG();
	testOpen();
	EXPECT_NO_LOG();
	testImportRoundTrip();
	EXPECT_NO_LOG();
	testImportRefusals();
	EXPECT_NO_LOG();
	testSettle();
	EXPECT_NO_LOG();
	testRevert();
	EXPECT_NO_LOG();
	testRevertFits();
	EXPECT_NO_LOG();
	testModeGuard();
	EXPECT_NO_LOG();
	testWriteLock();
	EXPECT_NO_LOG();
	testAbandoned();
	EXPECT_NO_LOG();
	testRace();
	EXPECT_NO_LOG();
	testPaths();
	EXPECT_NO_LOG();
	testBackup();
	EXPECT_NO_LOG();
	testWalPairing(&blocked);
	EXPECT_NO_LOG();
	testLockUnavailable();
	EXPECT_NO_LOG();
	testOpCap();
	EXPECT_NO_LOG();
	testLockName();
	EXPECT_NO_LOG();
	testCallerLock();
	EXPECT_NO_LOG();
	testFailedCreate();
	EXPECT_NO_LOG();

	if (blocked)
		printf("BLOCKED: renaming an open database did not fail with a sharing violation (T-D14)\n");
	if (nbFailed)
		printf("kept %s\n", runDir);
	else if (!removeTree(runDir))
	{
		check(false, "removeTree(runDir)", __LINE__);
		printf("run folder only partly removed: %s\n", runDir);
	}
	CloseHandle(job);
	printf("store suite wall time %I64u ms\n", nowMs() - suiteStart);
	printf("%d passed, %d failed\n", nbPassed, nbFailed);
	return nbFailed ? 1 : 0;
}
