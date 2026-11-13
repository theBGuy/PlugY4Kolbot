#pragma once

#include <string>
#include <vector>

struct sqlite3;

// Storage only: the game adapter must reserve before invoking a native action,
// then commit its result once. Pending operations never expire automatically.
namespace SharedStash
{
    typedef long long Id;
    typedef std::vector<unsigned char> Bytes;
    enum Status { OK, BUSY, STALE, NO_ROOM, INVALID, NOT_FOUND, COMMITTED, PENDING, ABORTED, STORE_ERROR };
    struct Page {
        Id id;
        int ordinal;
        unsigned flags;
        std::string name;
        Page() : id(0), ordinal(0), flags(0) {}
    };
    struct Item {
        Id id, page, version, parent; // parent=0 on stash grid; otherwise a root cube ID, on its 3x4 grid
        int x, y, width, height;
        Bytes data;
        Item() : id(0), page(0), version(0), parent(0), x(0), y(0), width(1), height(1) {}
    };
    struct ItemVersion {
        Id id, version;
        ItemVersion(Id i = 0, Id v = 0) : id(i), version(v) {}
    };
    struct Rect {
        Id page, parent;
        int x, y, width, height;
        Rect(Id p = 0, int a = 0, int b = 0, int w = 1, int h = 1, Id container = 0) : page(p), parent(container), x(a), y(b), width(w), height(h) {}
    };
    struct Reservation {
        std::string operation, session;
        std::vector<ItemVersion> items;
        std::vector<Rect> targets;
        Id expectedRevision; // -1: item versions only; layout edits require a current snapshot revision
        bool gold, layout;
        Reservation() : expectedRevision(-1), gold(false), layout(false) {}
    };
    struct Mutation {
        std::vector<Item> put; // id<=0 allocates a new ID; negative ID is an alias usable by another put[].parent
        std::vector<Id> erase;
        std::vector<Page> putPages; // requires layout reservation; negative ID is a new-page alias usable by put[].page
        std::vector<Id> erasePages;
        bool setGold;
        Id gold;
        Mutation() : setGold(false), gold(0) {}
    };
    struct Snapshot {
        Id revision, gold;
        bool initialized;
        std::vector<Page> pages;
        std::vector<Item> items;
        Snapshot() : revision(0), gold(0), initialized(false) {}
    };
    class Store {
    public:
        Store();
        Store(Store&& other) noexcept;
        ~Store();
        Status open(const char* path, int width, int height, int busyMs = 20);
        void close();
        const std::string& error() const { return error_; }
        // Registers the calling Windows process and its creation time. Tokens cannot be reused after end/death.
        Status registerSession(const std::string& token);
        // BUSY while native operations are pending. On success prunes this session's operation history,
        // retaining its identity tombstone so the token cannot be registered or used again.
        Status endSession(const std::string& token);
        // Own process only. Caller must have destroyed all native units and stopped all future saves/actions.
        // This explicitly discards unfinished native changes, cancels pending operations, and ends the session.
        Status abandonSession(const std::string& token);
        Status pendingSessions(std::vector<std::string>& out);
        // Confirms process termination/PID reuse with a Windows process handle; access denied fails closed.
        Status recoverDeadSession(const std::string& token);
        Status revision(Id& out); // cheap poll only; mutations still require reservation version checks
        Status snapshot(Snapshot& out);
        // One import only. Supplied item IDs/page IDs must be positive and unique.
        Status importOnce(const Snapshot& data);
        // OK means newly reserved. PENDING means it already exists: do not apply native action again.
        Status reserve(const Reservation& request);
        // On retry supply exactly the same mutation. created receives IDs of id<=0 puts/pages, pages first.
        // OK: committedSnapshot is exactly the result inside this commit, including automatic root versions.
        // COMMITTED retry: it is the current database snapshot and may include newer foreign changes; reconcile it.
        // Both outputs remain unchanged on failure.
        Status commit(const std::string& operation, const std::string& session, const Mutation& change, std::vector<Id>& created, Snapshot* committedSnapshot = 0);
        // Only before native mutation. Destroyed native state is handled by abandonSession().
        Status cancel(const std::string& operation, const std::string& session);
        Status backup(const char* destination);
    private:
        Store(const Store&);
        Store& operator=(const Store&);
        sqlite3* db_;
        int width_, height_;
        std::string error_;
    };
}
