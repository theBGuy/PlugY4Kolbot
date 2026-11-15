/*=================================================================
	Build tests for the vendored SQLite (third_party/sqlite): the
	version, the compile options scripts/build.ps1 passes, and how a
	database built with them behaves on disk.
	Built and run by scripts/build.ps1 -Test; exits 1 on any failure.

	SqliteBuildTests.exe [--scratch DIR]

	--scratch defaults to a scratch folder beside the exe. Each run
	works in its own subfolder, deleted when every check passes and
	kept (its path printed) when one fails.
=================================================================*/

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <set>
#include <string>
#include "../third_party/sqlite/sqlite3.h"

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

static void checkInt(int actual, int expected, const char* text, int line)
{
	if (actual == expected)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d: %s is %d, expected %d\n", line, text, actual, expected);
}

#define CHECK(cond) check((cond) ? true : false, #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkInt((int)(actual), (int)(expected), #actual, __LINE__)

// A loop's checks name the item, since one source line runs for every item.
static void checkItem(bool ok, const char* item, const char* text, int line)
{
	if (ok)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d [%s]: %s\n", line, item, text);
}

#define CHECK_ITEM(cond) checkItem((cond) ? true : false, item, #cond, __LINE__)

static const char* ExpectedVersion = "3.51.3";
static const char* ExpectedSourceId =
	"2026-03-13 10:38:09 737ae4a34738ffa0c3ff7f9bb18df914dd1cad163f28fd6b6e114a344fe6d618";

static char scratchDir[MAX_PATH] = "";
static char runDir[MAX_PATH] = "";

// sqlite3_open_v2 takes UTF-8; the command line arrives in the ANSI code page.
static std::string utf8Of(const char* ansi)
{
	wchar_t wide[MAX_PATH];
	char utf8[MAX_PATH * 3];
	if (!MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, MAX_PATH)) return std::string();
	if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), NULL, NULL)) return std::string();
	return std::string(utf8);
}

static bool exec(sqlite3* db, const char* sql)
{
	char* err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) printf("  sqlite: %s: %s\n", sql, err ? err : sqlite3_errstr(rc));
	sqlite3_free(err);
	return rc == SQLITE_OK;
}

// The first column of the first row as text, or "" (with the error printed) when there is none.
static std::string queryText(sqlite3* db, const char* sql)
{
	std::string result;
	sqlite3_stmt* stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
	{
		printf("  sqlite: %s: %s\n", sql, sqlite3_errmsg(db));
		return result;
	}
	if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
		result = (const char*)sqlite3_column_text(stmt, 0);
	sqlite3_finalize(stmt);
	return result;
}

static int queryInt(sqlite3* db, const char* sql)
{
	std::string text = queryText(db, sql);
	return text.empty() ? -1 : atoi(text.c_str());
}

// True when preparing sql fails with an error message that contains expected.
static bool prepareFails(sqlite3* db, const char* sql, const char* expected)
{
	sqlite3_stmt* stmt = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	sqlite3_finalize(stmt);
	if (rc == SQLITE_OK) return false;
	if (!strstr(sqlite3_errmsg(db), expected))
	{
		printf("  sqlite: %s: unexpected error \"%s\"\n", sql, sqlite3_errmsg(db));
		return false;
	}
	return true;
}

// The names in dir that match pattern (a FindFirstFile wildcard), without "." and "..".
static std::set<std::string> listNames(const char* dir, const char* pattern)
{
	std::set<std::string> names;
	char path[MAX_PATH];
	_snprintf(path, MAX_PATH, "%s\\%s", dir, pattern);
	path[MAX_PATH - 1] = 0;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(path, &fd);
	if (h == INVALID_HANDLE_VALUE) return names;
	do
	{
		if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
			names.insert(fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return names;
}

static std::string dbPath()
{
	return std::string(runDir) + "\\test.db";
}


/*============================= T-S1 ==========================*/

static void testVersion()
{
	printf("T-S1 version\n");
	CHECK(!strcmp(sqlite3_libversion(), ExpectedVersion));
	CHECK(!strcmp(SQLITE_VERSION, ExpectedVersion));
	CHECK(!strncmp(sqlite3_sourceid(), ExpectedSourceId, strlen(ExpectedSourceId)));
	CHECK(!strcmp(SQLITE_SOURCE_ID, sqlite3_sourceid()));
}


/*============================= T-S2 ==========================*/

// A function that sets a subtype without declaring SQLITE_RESULT_SUBTYPE: STRICT_SUBTYPE=1 makes that an error.
static void undeclaredSubtype(sqlite3_context* ctx, int, sqlite3_value**)
{
	sqlite3_result_int(ctx, 1);
	sqlite3_result_subtype(ctx, 77);
}

static void testCompileOptions()
{
	printf("T-S2 compile options\n");
	// Mirrors $SqliteDefines in scripts/build.ps1, minus the SQLITE_ prefix. TRUSTED_SCHEMA and STRICT_SUBTYPE are
	// not in SQLite's compile-option list, so their behavior is checked below instead.
	static const char* options[] = {
		"DQS=0",
		"THREADSAFE=1",
		"DEFAULT_SYNCHRONOUS=2",
		"DEFAULT_WAL_SYNCHRONOUS=2",
		"TEMP_STORE=3",
		"OMIT_LOAD_EXTENSION",
		"DEFAULT_MEMSTATUS=0",
		"LIKE_DOESNT_MATCH_BLOBS",
		"MAX_EXPR_DEPTH=0",
		"OMIT_DEPRECATED",
		"OMIT_PROGRESS_CALLBACK",
		"OMIT_SHARED_CACHE",
		"DEFAULT_FOREIGN_KEYS",
	};
	for (int i = 0; i < (int)(sizeof(options) / sizeof(options[0])); i++)
	{
		const char* item = options[i];
		CHECK_ITEM(sqlite3_compileoption_used(item));
	}
	CHECK_EQ(sqlite3_threadsafe(), 1);
	// Options this build leaves out: a stray one in the list fails here.
	static const char* absent[] = { "OMIT_AUTOINIT", "OMIT_WAL", "OMIT_JSON", "OMIT_DECLTYPE" };
	for (int i = 0; i < (int)(sizeof(absent) / sizeof(absent[0])); i++)
	{
		const char* item = absent[i];
		CHECK_ITEM(!sqlite3_compileoption_used(item));
	}
	for (int i = 0; sqlite3_compileoption_get(i); i++)
	{
		const char* item = sqlite3_compileoption_get(i);
		CHECK_ITEM(strncmp(item, "ENABLE_", 7) != 0);
	}

	sqlite3* db = NULL;
	CHECK_EQ(sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
	CHECK_EQ(queryInt(db, "PRAGMA trusted_schema"), 0);
	CHECK_EQ(queryInt(db, "PRAGMA foreign_keys"), 1);
	CHECK_EQ(sqlite3_create_function(db, "undeclared_subtype", 0, SQLITE_UTF8, NULL, undeclaredSubtype, NULL, NULL),
		SQLITE_OK);
	sqlite3_stmt* stmt = NULL;
	CHECK_EQ(sqlite3_prepare_v2(db, "SELECT undeclared_subtype()", -1, &stmt, NULL), SQLITE_OK);
	CHECK_EQ(sqlite3_step(stmt), SQLITE_ERROR);
	CHECK(strstr(sqlite3_errmsg(db), "misuse of sqlite3_result_subtype()") != NULL);
	sqlite3_finalize(stmt);
	sqlite3_close(db);
}


/*============================= T-S3 ==========================*/

static void testWal()
{
	printf("T-S3 WAL database in %s\n", runDir);
	std::string path = utf8Of(dbPath().c_str());
	CHECK(!path.empty());
	sqlite3* db = NULL;
	CHECK_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
	CHECK(queryText(db, "PRAGMA journal_mode=WAL") == "wal");
	CHECK_EQ(queryInt(db, "PRAGMA synchronous"), 2);
	CHECK(exec(db, "BEGIN IMMEDIATE"));
	CHECK(exec(db, "CREATE TABLE t(a TEXT)"));
	CHECK(exec(db, "INSERT INTO t VALUES('x')"));
	CHECK(exec(db, "COMMIT"));
	CHECK_EQ(sqlite3_close(db), SQLITE_OK);
}


/*============================= T-S4 ==========================*/

// TEMP_STORE=3 keeps temp b-trees in memory. SQLite names its temp files etilqs_* and deletes them on close, so
// both folders are listed while the sorting statement is still open.
static void testTempStore()
{
	printf("T-S4 temp b-trees stay in memory\n");
	char tempDir[MAX_PATH];
	DWORD len = GetTempPathA(MAX_PATH, tempDir);
	CHECK(len > 0 && len < MAX_PATH);
	if (len > 0 && len < MAX_PATH && tempDir[len - 1] == '\\') tempDir[len - 1] = 0;
	std::set<std::string> tempBefore = listNames(tempDir, "etilqs_*");

	std::string path = utf8Of(dbPath().c_str());
	sqlite3* db = NULL;
	CHECK_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
	CHECK(queryText(db, "PRAGMA journal_mode") == "wal");
	// About 4 MB of unindexed text: past the default 2 MB cache, so a file-backed sorter would spill.
	CHECK(exec(db,
		"BEGIN;"
		"CREATE TABLE big(k TEXT);"
		"WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 10000)"
		" INSERT INTO big SELECT hex(randomblob(200)) FROM n;"
		"COMMIT"));
	sqlite3_stmt* stmt = NULL;
	CHECK_EQ(sqlite3_prepare_v2(db, "SELECT k FROM big ORDER BY k", -1, &stmt, NULL), SQLITE_OK);
	CHECK_EQ(sqlite3_step(stmt), SQLITE_ROW);

	std::set<std::string> tempDuring = listNames(tempDir, "etilqs_*");
	for (std::set<std::string>::const_iterator it = tempDuring.begin(); it != tempDuring.end(); ++it)
	{
		const char* item = it->c_str();
		CHECK_ITEM(tempBefore.count(*it) != 0);
	}
	std::set<std::string> runNames = listNames(runDir, "*");
	for (std::set<std::string>::const_iterator it = runNames.begin(); it != runNames.end(); ++it)
	{
		const char* item = it->c_str();
		CHECK_ITEM(*it == "test.db" || *it == "test.db-wal" || *it == "test.db-shm");
	}
	CHECK(runNames.count("test.db-wal") != 0);

	sqlite3_finalize(stmt);
	CHECK_EQ(sqlite3_close(db), SQLITE_OK);
}


/*============================= T-S5, T-S6 ==========================*/

static void testRefusedSql()
{
	printf("T-S5 no load_extension, T-S6 no double-quoted strings\n");
	sqlite3* db = NULL;
	CHECK_EQ(sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
	CHECK(prepareFails(db, "SELECT load_extension('x')", "no such function"));
	CHECK(prepareFails(db, "SELECT \"nope\"", "no such column"));
	CHECK(queryText(db, "SELECT 'yes'") == "yes");
	sqlite3_close(db);
}


static bool removeRunDir()
{
	std::set<std::string> names = listNames(runDir, "*");
	for (std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it)
	{
		std::string path = std::string(runDir) + "\\" + *it;
		if (!DeleteFileA(path.c_str())) return false;
	}
	return RemoveDirectoryA(runDir) != 0;
}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--scratch") && i + 1 < argc)
			strncpy(scratchDir, argv[++i], MAX_PATH - 1);
		else
		{
			printf("usage: SqliteBuildTests [--scratch DIR]\n");
			return 2;
		}
	}
	if (!scratchDir[0])
	{
		GetModuleFileNameA(NULL, scratchDir, MAX_PATH);
		char* slash = strrchr(scratchDir, '\\');
		strcpy(slash ? slash + 1 : scratchDir, "scratch");
	}
	CreateDirectoryA(scratchDir, NULL);
	_snprintf(runDir, MAX_PATH, "%s\\sqlite-%lu-%lu", scratchDir, GetCurrentProcessId(), GetTickCount());
	runDir[MAX_PATH - 1] = 0;
	if (!CreateDirectoryA(runDir, NULL))
	{
		printf("cannot create %s (error %lu)\n", runDir, GetLastError());
		return 1;
	}
	printf("SqliteBuildTests (SQLite %s): scratch %s\n", sqlite3_libversion(), runDir);

	testVersion();
	testCompileOptions();
	testWal();
	testTempStore();
	testRefusedSql();

	if (!nbFailed) CHECK(removeRunDir());
	if (nbFailed) printf("kept %s\n", runDir);
	printf("%d passed, %d failed\n", nbPassed, nbFailed);
	return nbFailed ? 1 : 0;
}
