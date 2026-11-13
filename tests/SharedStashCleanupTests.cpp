#include "../src/common/SharedStashCleanup.h"
#include "../third_party/sqlite/sqlite3.h"
#include <windows.h>
#include <stdio.h>
#include <utility>

using namespace SharedStash;

static int checks = 0;
static int failures = 0;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #test); } } while (0)

static bool execute(sqlite3* db, const char* sql)
{
    int result = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (result != SQLITE_OK) printf("SQL: %s\n", sqlite3_errmsg(db));
    return result == SQLITE_OK;
}

static sqlite3* connection(const std::string& path)
{
    sqlite3* db = NULL;
    int result = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
    CHECK(result == SQLITE_OK);
    return db;
}

static bool makePending(Store& owner, Store& peer, const std::string& path, const char* token)
{
    CHECK(owner.open(path.c_str(), 10, 10) == OK);
    Snapshot seed;
    Page page;
    page.id = 1;
    seed.pages.push_back(page);
    CHECK(owner.importOnce(seed) == OK);
    CHECK(owner.registerSession(token) == OK);
    Reservation request;
    request.operation = std::string(token) + "-operation";
    request.session = token;
    request.layout = true;
    request.expectedRevision = 1;
    CHECK(owner.reserve(request) == OK);
    CHECK(peer.open(path.c_str(), 10, 10) == OK);
    CHECK(peer.registerSession("peer") == OK);
    CHECK(peer.recoverDeadSession(token) == BUSY);
    return failures == 0;
}

static void pumpMessages(DWORD milliseconds)
{
    DWORD started = GetTickCount();
    do
    {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(10);
    } while (GetTickCount() - started < milliseconds);
}

static bool waitForCleanup(Store& peer)
{
    // This is a menu message loop: no game-frame or explicit cleanup polling call.
    DWORD started = GetTickCount();
    do
    {
        pumpMessages(40);
        std::vector<std::string> owners;
        if (peer.pendingSessions(owners) == OK && owners.empty()) return true;
    } while (GetTickCount() - started < 6000);
    return false;
}

struct RetireWork
{
    Store* store;
    const char* token;
    bool accepted;
};

static DWORD WINAPI retireOnServerThread(void* argument)
{
    RetireWork& work = *(RetireWork*)argument;
    work.accepted = retireSession(*work.store, work.token);
    return 0;
}

static void checkPeerCanUseStash(Store& peer, const char* closedToken)
{
    CHECK(peer.recoverDeadSession(closedToken) == COMMITTED);
    CHECK(peer.registerSession(closedToken) == INVALID);
    Reservation request;
    request.operation = "peer-after-cleanup";
    request.session = "peer";
    request.layout = true;
    request.expectedRevision = 1;
    CHECK(peer.reserve(request) == OK);
    CHECK(peer.recoverDeadSession("peer") == BUSY);
    CHECK(peer.cancel(request.operation, request.session) == OK);
    CHECK(peer.endSession("peer") == OK);
}

static void writeLockAtClose(const std::string& path)
{
    Store owner, peer;
    if (!makePending(owner, peer, path, "closed-locked")) return;
    sqlite3* blocker = connection(path);
    CHECK(execute(blocker, "BEGIN IMMEDIATE"));

    // All native units are gone. Only the database connection and token move.
    RetireWork work = {&owner, "closed-locked", false};
    HANDLE serverThread = CreateThread(NULL, 0, retireOnServerThread, &work, 0, NULL);
    CHECK(serverThread != NULL);
    if (!serverThread) return;
    CHECK(WaitForSingleObject(serverThread, INFINITE) == WAIT_OBJECT_0);
    CHECK(CloseHandle(serverThread) != FALSE);
    CHECK(work.accepted);
    Id revision = 123;
    CHECK(owner.revision(revision) == INVALID && revision == 123);
    pumpMessages(1200);
    std::vector<std::string> pending;
    CHECK(peer.pendingSessions(pending) == OK && pending.size() == 1 && pending[0] == "closed-locked");

    CHECK(execute(blocker, "ROLLBACK"));
    CHECK(sqlite3_close(blocker) == SQLITE_OK);
    CHECK(waitForCleanup(peer));
    checkPeerCanUseStash(peer, "closed-locked");
}

static void writeErrorAtClose(const std::string& path)
{
    Store owner, peer;
    if (!makePending(owner, peer, path, "closed-error")) return;
    sqlite3* controls = connection(path);
    CHECK(execute(controls,
        "CREATE TRIGGER fail_cleanup BEFORE UPDATE OF ended ON sessions "
        "BEGIN SELECT RAISE(ABORT,'cleanup failure'); END;"));
    CHECK(retireSession(owner, "closed-error"));
    pumpMessages(1200);
    std::vector<std::string> pending;
    CHECK(peer.pendingSessions(pending) == OK && pending.size() == 1 && pending[0] == "closed-error");
    CHECK(execute(controls, "DROP TRIGGER fail_cleanup"));
    CHECK(sqlite3_close(controls) == SQLITE_OK);
    CHECK(waitForCleanup(peer));
    checkPeerCanUseStash(peer, "closed-error");
}

static void moveConnection(const std::string& path)
{
    Store source;
    CHECK(source.open(path.c_str(), 10, 10) == OK);
    Store destination(std::move(source));
    Id revision = 123;
    CHECK(source.revision(revision) == INVALID && revision == 123);
    CHECK(destination.revision(revision) == OK && revision == 0);
}

static void immediateClose(const std::string& path)
{
    Store owner, peer;
    if (!makePending(owner, peer, path, "closed-now")) return;
    CHECK(retireSession(owner, "closed-now"));
    std::vector<std::string> pending;
    CHECK(peer.pendingSessions(pending) == OK && pending.empty());
    checkPeerCanUseStash(peer, "closed-now");
}

int main()
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(MAX_PATH, temporary) != 0);
    char folder[MAX_PATH];
    snprintf(folder, sizeof(folder), "%sPlugYCleanup-%lu-%lu", temporary, GetCurrentProcessId(), GetTickCount());
    CHECK(CreateDirectoryA(folder, NULL) != FALSE);
    CHECK(startCleanupTimer());
    CHECK(startCleanupTimer());
    moveConnection(std::string(folder) + "\\move.sqlite3");
    immediateClose(std::string(folder) + "\\immediate.sqlite3");
    writeLockAtClose(std::string(folder) + "\\locked.sqlite3");
    writeErrorAtClose(std::string(folder) + "\\error.sqlite3");
    stopCleanupTimer();
    printf("SharedStashCleanup: %d checks, %d failures; artifacts %s\n", checks, failures, folder);
    return failures ? 1 : 0;
}
