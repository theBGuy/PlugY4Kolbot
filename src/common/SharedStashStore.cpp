#include "SharedStashStore.h"
#include "../../third_party/sqlite/sqlite3.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <map>
#include <set>
#include <exception>
#include <utility>

namespace SharedStash {
namespace {
const int ApplicationId = 0x50595348;
enum { ITEM = 1, CELL = 2, LEGACY_PAGE = 3, GOLD = 4, LAYOUT = 5 };
struct Session {
    unsigned long pid;
    Id creationTime;
    bool ended;
};
struct Failure { Status status; std::string text; Failure(Status s, const std::string& t) : status(s), text(t) {} };
void require(bool ok, Status status, const char* text) { if (!ok) throw Failure(status, text); }
void sqlCheck(sqlite3* db, int rc) {
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) return;
    int primary = rc & 255;
    throw Failure(primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? BUSY : STORE_ERROR, sqlite3_errmsg(db));
}
void exec(sqlite3* db, const char* sql) { sqlCheck(db, sqlite3_exec(db, sql, 0, 0, 0)); }
struct Query {
    sqlite3* db; sqlite3_stmt* s;
    Query(sqlite3* d, const char* sql) : db(d), s(0) { sqlCheck(db, sqlite3_prepare_v2(db, sql, -1, &s, 0)); }
    ~Query() { sqlite3_finalize(s); }
    Query& number(int n, Id value) { sqlCheck(db, sqlite3_bind_int64(s, n, value)); return *this; }
    Query& text(int n, const std::string& value) { require(value.size() <= INT_MAX, INVALID, "text too large"); sqlCheck(db, sqlite3_bind_text(s, n, value.c_str(), (int)value.size(), SQLITE_TRANSIENT)); return *this; }
    Query& blob(int n, const Bytes& value) { require(value.size() <= INT_MAX, INVALID, "blob too large"); sqlCheck(db, sqlite3_bind_blob(s, n, value.empty() ? "" : (const void*)&value[0], (int)value.size(), SQLITE_TRANSIENT)); return *this; }
    bool row() { int rc = sqlite3_step(s); sqlCheck(db, rc); return rc == SQLITE_ROW; }
    Id integer(int n) { return sqlite3_column_int64(s, n); }
    std::string string(int n) { const char* p = (const char*)sqlite3_column_text(s,n); return p ? std::string(p, sqlite3_column_bytes(s,n)) : std::string(); }
    Bytes bytes(int n) { const unsigned char* p = (const unsigned char*)sqlite3_column_blob(s,n); int count=sqlite3_column_bytes(s,n); return count ? Bytes(p,p+count) : Bytes(); }
};
struct Transaction {
    sqlite3* db; bool active;
    Transaction(sqlite3* d, bool write=true) : db(d), active(false) { exec(db, write ? "BEGIN IMMEDIATE" : "BEGIN"); active=true; }
    ~Transaction() { if(active) sqlite3_exec(db,"ROLLBACK",0,0,0); }
    void finish() { exec(db,"COMMIT"); active=false; }
};
Id scalar(sqlite3* db, const char* sql) { Query q(db,sql); require(q.row(),STORE_ERROR,"missing metadata"); return q.integer(0); }
void encode(Bytes& b, Id n) { unsigned long long v=(unsigned long long)n; for(int k=0;k<8;++k) { b.push_back((unsigned char)v); v >>= 8; } }
void encode(Bytes& b, const Bytes& value) { encode(b,(Id)value.size()); b.insert(b.end(),value.begin(),value.end()); }
void encode(Bytes& b, const std::string& value) { encode(b,Bytes(value.begin(),value.end())); }
Bytes encodeReservation(const Reservation& r) {
    Bytes b; encode(b,r.session); encode(b,r.expectedRevision); encode(b,r.gold); encode(b,r.layout); encode(b,(Id)r.items.size());
    for(size_t i=0;i<r.items.size();++i) { encode(b,r.items[i].id); encode(b,r.items[i].version); }
    encode(b,(Id)r.targets.size()); for(size_t i=0;i<r.targets.size();++i) { const Rect& a=r.targets[i]; encode(b,a.page); encode(b,a.parent); encode(b,a.x); encode(b,a.y); encode(b,a.width); encode(b,a.height); }
    return b;
}
Bytes encodeMutation(const Mutation& m) {
    Bytes b; encode(b,m.setGold); encode(b,m.gold); encode(b,(Id)m.put.size());
    for(size_t i=0;i<m.put.size();++i) { const Item& a=m.put[i]; encode(b,a.id); encode(b,a.page); encode(b,a.version); encode(b,a.parent); encode(b,a.x); encode(b,a.y); encode(b,a.width); encode(b,a.height); encode(b,a.data); }
    encode(b,(Id)m.erase.size()); for(size_t i=0;i<m.erase.size();++i) encode(b,m.erase[i]);
    encode(b,(Id)m.putPages.size()); for(size_t i=0;i<m.putPages.size();++i) { const Page& p=m.putPages[i]; encode(b,p.id); encode(b,p.ordinal); encode(b,p.flags); encode(b,p.name); }
    encode(b,(Id)m.erasePages.size()); for(size_t i=0;i<m.erasePages.size();++i) encode(b,m.erasePages[i]); return b;
}
bool pageExists(sqlite3* db, Id page) { Query q(db,"SELECT 1 FROM pages WHERE id=?"); return q.number(1,page).row(); }
Item readItem(Query& row) {
    Item item;
    item.id = row.integer(0);
    item.page = row.integer(1);
    item.version = row.integer(2);
    item.x = (int)row.integer(3);
    item.y = (int)row.integer(4);
    item.width = (int)row.integer(5);
    item.height = (int)row.integer(6);
    item.data = row.bytes(7);
    item.parent = row.integer(8);
    return item;
}
Item getItem(sqlite3* db, Id id) {
    Query row(db, "SELECT id,page,version,x,y,w,h,data,parent FROM items WHERE id=?");
    row.number(1, id);
    require(row.row(), STALE, "item no longer in stash");
    return readItem(row);
}
void readSnapshot(sqlite3* db, Snapshot& result) {
    Query meta(db, "SELECT revision,gold,initialized FROM meta WHERE singleton=1");
    require(meta.row(), STORE_ERROR, "missing metadata");
    result.revision = meta.integer(0);
    result.gold = meta.integer(1);
    result.initialized = meta.integer(2) != 0;

    Query pages(db, "SELECT id,ordinal,flags,name FROM pages ORDER BY ordinal,id");
    while (pages.row()) {
        Page page;
        page.id = pages.integer(0);
        page.ordinal = (int)pages.integer(1);
        page.flags = (unsigned)pages.integer(2);
        page.name = pages.string(3);
        result.pages.push_back(page);
    }
    Query items(db, "SELECT id,page,version,x,y,w,h,data,parent FROM items ORDER BY id");
    while (items.row()) result.items.push_back(readItem(items));
}
void publishSnapshot(Snapshot& target,Snapshot& source) {
    target.revision=source.revision; target.gold=source.gold; target.initialized=source.initialized;
    target.pages.swap(source.pages); target.items.swap(source.items);
}
void checkRect(sqlite3* db,const Rect& r,int width,int height) {
    require(r.parent>=0,INVALID,"invalid container ID");
    if(r.parent) { width=3; height=4; }
    require(r.page>0 && r.x>=0 && r.y>=0 && r.width>0 && r.height>0 && r.width<=width && r.height<=height && r.x<=width-r.width && r.y<=height-r.height,INVALID,"invalid item rectangle");
    require(pageExists(db,r.page),NOT_FOUND,"page not found");
}
Rect rect(const Item& a) { return Rect(a.page,a.x,a.y,a.width,a.height,a.parent); }
Id scope(const Rect& r) { return r.parent ? -r.parent : r.page; }
Id processCreation(HANDLE handle) {
    FILETIME created,ended,kernel,user;
    require(GetProcessTimes(handle,&created,&ended,&kernel,&user)!=FALSE,STORE_ERROR,"cannot read process creation time");
    return ((Id)created.dwHighDateTime<<32)|created.dwLowDateTime;
}
Session getSession(sqlite3* db, const std::string& token, Status missing = NOT_FOUND) {
    Query row(db, "SELECT pid,creation,ended FROM sessions WHERE token=?");
    row.text(1, token);
    require(row.row(), missing, "session not registered");
    Session session;
    session.pid = (unsigned long)row.integer(0);
    session.creationTime = row.integer(1);
    session.ended = row.integer(2) != 0;
    return session;
}
void currentSession(sqlite3* db, const std::string& token) {
    Session session = getSession(db, token, INVALID);
    require(!session.ended && session.pid == GetCurrentProcessId()
        && session.creationTime == processCreation(GetCurrentProcess()),
        INVALID, "session does not belong to this live process");
}
bool owns(sqlite3* db,const std::string& op,int kind,Id a=0,Id b=0,Id c=0) {
    Query q(db,"SELECT 1 FROM locks WHERE kind=? AND a=? AND b=? AND c=? AND operation=?"); return q.number(1,kind).number(2,a).number(3,b).number(4,c).text(5,op).row();
}
void lock(sqlite3* db,const std::string& op,int kind,Id a=0,Id b=0,Id c=0) {
    Query q(db,"SELECT operation FROM locks WHERE kind=? AND a=? AND b=? AND c=?"); q.number(1,kind).number(2,a).number(3,b).number(4,c);
    if(q.row()) { require(q.string(0)==op,BUSY,"resource reserved by another operation"); return; }
    Query put(db,"INSERT INTO locks VALUES(?,?,?,?,?)"); put.number(1,kind).number(2,a).number(3,b).number(4,c).text(5,op).row();
}
void checkPageLock(sqlite3* db,Id page,const std::string& op) {
    Query q(db,"SELECT operation FROM locks WHERE kind=3 AND a=? AND operation<>?"); require(!q.number(1,page).text(2,op).row(),BUSY,"page reserved");
}
void lockRect(sqlite3* db,const std::string& op,const Rect& r) {
    checkPageLock(db,r.page,op); for(int y=r.y;y<r.y+r.height;++y) for(int x=r.x;x<r.x+r.width;++x) lock(db,op,CELL,scope(r),x,y);
}
bool canChangeItem(sqlite3* db,const std::string& op,const Item& a) { return owns(db,op,ITEM,a.id) || owns(db,op,LEGACY_PAGE,a.page) || owns(db,op,LAYOUT); }
void checkTargetLock(sqlite3* db,const std::string& op,const Item& a) {
    if(owns(db,op,LEGACY_PAGE,a.page) || owns(db,op,LAYOUT)) return;
    for(int y=a.y;y<a.y+a.height;++y) for(int x=a.x;x<a.x+a.width;++x) require(owns(db,op,CELL,scope(rect(a)),x,y),INVALID,"target cell was not reserved");
}
void putItem(sqlite3* db,Item a) {
    Query q(db,"INSERT INTO items(id,page,version,x,y,w,h,data,parent) VALUES(?,?,?,?,?,?,?,?,NULLIF(?,0))");
    q.number(1,a.id).number(2,a.page).number(3,a.version).number(4,a.x).number(5,a.y).number(6,a.width).number(7,a.height).blob(8,a.data).number(9,a.parent).row();
    for(int y=a.y;y<a.y+a.height;++y) for(int x=a.x;x<a.x+a.width;++x) {
        Query cell(db,"SELECT 1 FROM cells WHERE page=? AND parent=? AND x=? AND y=?"); require(!cell.number(1,a.page).number(2,a.parent).number(3,x).number(4,y).row(),NO_ROOM,"occupied target cell");
        Query p(db,"INSERT INTO cells VALUES(?,?,?,?,?)"); p.number(1,a.page).number(2,a.parent).number(3,x).number(4,y).number(5,a.id).row();
    }
}
void eraseItem(sqlite3* db,Id id) { Query q(db,"DELETE FROM items WHERE id=?"); q.number(1,id).row(); }
void validPage(const Page& p) { require(p.ordinal>=0 && p.name.size()<=20 && p.name.find('\0')==std::string::npos,INVALID,"invalid page"); }
void checkPageOrder(sqlite3* db) {
    require(scalar(db,"SELECT count(*) FROM pages")>0,INVALID,"stash must have a page");
    Query q(db,"SELECT ordinal FROM pages GROUP BY ordinal HAVING count(*)>1"); require(!q.row(),INVALID,"duplicate page ordinal");
}
void putPage(sqlite3* db,const Page& p) {
    validPage(p); Query q(db,"INSERT INTO pages(id,ordinal,flags,name) VALUES(?,?,?,?)"); q.number(1,p.id).number(2,p.ordinal).number(3,p.flags).text(4,p.name).row();
}
void unlock(sqlite3* db,const std::string& op) { Query q(db,"DELETE FROM locks WHERE operation=?"); q.text(1,op).row(); }
void pruneSessionHistory(sqlite3* db,const std::string& token) {
    Query locks(db,"DELETE FROM locks WHERE operation IN(SELECT id FROM operations WHERE session=? AND state IN(1,2))"); locks.text(1,token).row();
    Query operations(db,"DELETE FROM operations WHERE session=? AND state IN(1,2)"); operations.text(1,token).row();
}
void cancelSessionOperations(sqlite3* db,const std::string& token) {
    Query release(db,"DELETE FROM locks WHERE operation IN(SELECT id FROM operations WHERE session=? AND state=0)"); release.text(1,token).row();
    Query cancel(db,"UPDATE operations SET state=2 WHERE session=? AND state=0"); cancel.text(1,token).row();
    Query end(db,"UPDATE sessions SET ended=1 WHERE token=?"); end.text(1,token).row();
    pruneSessionHistory(db,token);
}
void checkParents(sqlite3* db) {
    Query q(db,"SELECT 1 FROM items child LEFT JOIN items root ON root.id=child.parent WHERE child.parent IS NOT NULL AND (root.id IS NULL OR root.parent IS NOT NULL OR root.page<>child.page OR root.id=child.id)");
    require(!q.row(),INVALID,"cube children need a root on the same page; nested inventories are unsupported");
}
void lockFamily(sqlite3* db, const std::string& operation, Id id) {
    Item item = getItem(db, id);
    Item root = item.parent ? getItem(db, item.parent) : item;
    require(root.parent == 0, INVALID, "nested item inventory");
    checkPageLock(db, root.page, operation);
    lock(db, operation, ITEM, root.id);
    lockRect(db, operation, rect(root));

    Query children(db, "SELECT id,page,version,x,y,w,h,data,parent FROM items WHERE parent=?");
    children.number(1, root.id);
    while (children.row()) {
        Item child = readItem(children);
        lock(db, operation, ITEM, child.id);
        lockRect(db, operation, rect(child));
    }
}
#define HANDLE_FAILURE catch(const Failure& f) { error_=f.text; return f.status; } catch(const std::exception& f) { error_=f.what(); return STORE_ERROR; }
}
Store::Store() : db_(0), width_(0), height_(0) {}
Store::Store(Store&& other) noexcept
    : db_(other.db_), width_(other.width_), height_(other.height_), error_(std::move(other.error_))
{
    other.db_ = 0;
    other.width_ = other.height_ = 0;
}
Store::~Store() { close(); }
void Store::close() { if(db_) sqlite3_close_v2(db_); db_=0; }
Status Store::registerSession(const std::string& token) {
    try {
        require(db_!=0 && !token.empty() && token.size()<=128,INVALID,"invalid session token"); Transaction tx(db_);
        Query existing(db_,"SELECT 1 FROM sessions WHERE token=?"); existing.text(1,token);
        if(existing.row()) { currentSession(db_,token); return COMMITTED; }
        Query add(db_,"INSERT INTO sessions VALUES(?,?,?,0)"); add.text(1,token).number(2,GetCurrentProcessId()).number(3,processCreation(GetCurrentProcess())).row(); tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::endSession(const std::string& token) {
    try {
        require(db_!=0,INVALID,"database not open"); Transaction tx(db_); Session session=getSession(db_,token);
        require(session.pid==GetCurrentProcessId() && session.creationTime==processCreation(GetCurrentProcess()),INVALID,"session belongs to another process");
        if(session.ended) return COMMITTED;
        Query pending(db_,"SELECT 1 FROM operations WHERE session=? AND state=0"); require(!pending.text(1,token).row(),BUSY,"session has pending operations");
        Query end(db_,"UPDATE sessions SET ended=1 WHERE token=?"); end.text(1,token).row(); pruneSessionHistory(db_,token); tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::abandonSession(const std::string& token) {
    try {
        require(db_!=0,INVALID,"database not open"); Transaction tx(db_); Session session=getSession(db_,token);
        require(session.pid==GetCurrentProcessId() && session.creationTime==processCreation(GetCurrentProcess()),INVALID,"session belongs to another process");
        if(session.ended) return COMMITTED;
        cancelSessionOperations(db_,token); tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::pendingSessions(std::vector<std::string>& out) {
    try {
        require(db_ != 0, INVALID, "database not open");
        std::vector<std::string> sessions;
        Query pending(db_, "SELECT DISTINCT session FROM operations WHERE state=0 ORDER BY session");
        while (pending.row()) sessions.push_back(pending.string(0));
        out.swap(sessions);
        return OK;
    } HANDLE_FAILURE
}
Status Store::recoverDeadSession(const std::string& token) {
    try {
        require(db_!=0,INVALID,"database not open"); Transaction tx(db_); Session session=getSession(db_,token);
        if(session.ended) return COMMITTED;
        HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,session.pid);
        bool dead=false;
        if(!process) { DWORD error=GetLastError(); require(error==ERROR_INVALID_PARAMETER,BUSY,"cannot prove session owner terminated"); dead=true; }
        else {
            FILETIME created,ended,kernel,user; BOOL times=GetProcessTimes(process,&created,&ended,&kernel,&user); DWORD wait=WaitForSingleObject(process,0); CloseHandle(process);
            require(times!=FALSE,BUSY,"cannot verify session process creation time");
            Id creation=((Id)created.dwHighDateTime<<32)|created.dwLowDateTime;
            dead=creation!=session.creationTime || wait==WAIT_OBJECT_0;
        }
        require(dead,BUSY,"session owner still running");
        cancelSessionOperations(db_,token); tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::open(const char* path, int width, int height, int busyMs) {
    close();
    error_.clear();
    try {
        require(path && *path && width > 0 && width <= 15 && height > 0 && height <= 15 && busyMs >= 0,
            INVALID, "invalid database settings");
        int result = sqlite3_open_v2(path, &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 0);
        sqlCheck(db_, result);
        sqlite3_busy_timeout(db_, busyMs);
        exec(db_, "PRAGMA foreign_keys=ON");

        {
            Transaction transaction(db_);
            Id application = scalar(db_, "PRAGMA application_id");
            Id version = scalar(db_, "PRAGMA user_version");
            bool empty = application == 0 && version == 0
                && scalar(db_, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'") == 0;
            if (empty) {
                exec(db_,"CREATE TABLE meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),width INTEGER,height INTEGER,revision INTEGER NOT NULL, gold INTEGER NOT NULL CHECK(gold>=0 AND gold<=4294967295),initialized INTEGER NOT NULL);"
                    "CREATE TABLE pages(id INTEGER PRIMARY KEY AUTOINCREMENT,ordinal INTEGER NOT NULL CHECK(ordinal>=0),flags INTEGER NOT NULL,name TEXT NOT NULL);"
                    "CREATE TABLE items(id INTEGER PRIMARY KEY AUTOINCREMENT,page INTEGER NOT NULL REFERENCES pages(id),version INTEGER NOT NULL CHECK(version>0),x INTEGER NOT NULL,y INTEGER NOT NULL,w INTEGER NOT NULL,h INTEGER NOT NULL,data BLOB NOT NULL,parent INTEGER REFERENCES items(id) DEFERRABLE INITIALLY DEFERRED);"
                    "CREATE TABLE cells(page INTEGER NOT NULL REFERENCES pages(id),parent INTEGER NOT NULL,x INTEGER NOT NULL,y INTEGER NOT NULL,item INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,PRIMARY KEY(page,parent,x,y));"
                    "CREATE TABLE sessions(token TEXT PRIMARY KEY,pid INTEGER NOT NULL,creation INTEGER NOT NULL,ended INTEGER NOT NULL CHECK(ended IN(0,1)));"
                    "CREATE TABLE operations(id TEXT PRIMARY KEY,session TEXT NOT NULL,state INTEGER NOT NULL,request BLOB NOT NULL,mutation BLOB,result BLOB);"
                    "CREATE TABLE locks(kind INTEGER NOT NULL,a INTEGER NOT NULL,b INTEGER NOT NULL,c INTEGER NOT NULL,operation TEXT NOT NULL REFERENCES operations(id),PRIMARY KEY(kind,a,b,c));"
                    "PRAGMA application_id=1348031304; PRAGMA user_version=2;");
                Query meta(db_, "INSERT INTO meta VALUES(1,?,?,0,0,0)");
                meta.number(1, width).number(2, height).row();
            } else {
                require(application == ApplicationId && version == 2,
                    INVALID, "not a supported shared stash database");
            }
            Query grid(db_, "SELECT width,height FROM meta WHERE singleton=1");
            require(grid.row() && grid.integer(0) == width && grid.integer(1) == height,
                INVALID, "stash grid mismatch");
            transaction.finish();
        }
        {
            Query wal(db_, "PRAGMA journal_mode=WAL");
            require(wal.row() && wal.string(0) == "wal", STORE_ERROR, "WAL unavailable");
        }
        exec(db_, "PRAGMA synchronous=FULL");
        width_ = width;
        height_ = height;
        return OK;
    } catch (const Failure& failure) {
        error_ = failure.text;
        close();
        return failure.status;
    } catch (const std::exception& failure) {
        error_ = failure.what();
        close();
        return STORE_ERROR;
    }
}
Status Store::revision(Id& out) {
    try {
        require(db_!=0,INVALID,"database not open"); Query q(db_,"SELECT revision FROM meta WHERE singleton=1"); require(q.row(),STORE_ERROR,"missing metadata"); Id current=q.integer(0); out=current; return OK;
    } HANDLE_FAILURE
}
Status Store::snapshot(Snapshot& out) {
    try {
        require(db_!=0,INVALID,"database not open"); Transaction tx(db_,false); Snapshot result;
        readSnapshot(db_,result); tx.finish(); publishSnapshot(out,result); return OK;
    } HANDLE_FAILURE
}
Status Store::importOnce(const Snapshot& data) {
    try {
        require(db_!=0,INVALID,"database not open"); Transaction tx(db_);
        if(scalar(db_,"SELECT initialized FROM meta WHERE singleton=1")) return COMMITTED;
        require(data.gold>=0 && data.gold<=4294967295LL,INVALID,"invalid gold");
        for(size_t i=0;i<data.pages.size();++i) { require(data.pages[i].id>0,INVALID,"import needs positive page IDs"); putPage(db_,data.pages[i]); }
        checkPageOrder(db_);
        for(size_t i=0;i<data.items.size();++i) { Item a=data.items[i]; require(a.id>0,INVALID,"import needs positive item IDs"); checkRect(db_,rect(a),width_,height_); a.version=1; putItem(db_,a); }
        checkParents(db_);
        Query q(db_,"UPDATE meta SET initialized=1,gold=?,revision=1 WHERE singleton=1"); q.number(1,data.gold).row(); tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::reserve(const Reservation& r) {
    try {
        require(db_!=0,INVALID,"database not open"); require(!r.operation.empty() && !r.session.empty() && r.operation.size()<=128 && r.session.size()<=128,INVALID,"invalid operation/session");
        Bytes request=encodeReservation(r); Transaction tx(db_); currentSession(db_,r.session);
        Query old(db_,"SELECT session,state,request FROM operations WHERE id=?"); old.text(1,r.operation);
        if(old.row()) { require(old.string(0)==r.session && old.bytes(2)==request,INVALID,"operation ID reused with different request"); return old.integer(1)==0 ? PENDING : old.integer(1)==1 ? COMMITTED : ABORTED; }
        require(scalar(db_,"SELECT initialized FROM meta WHERE singleton=1")!=0,INVALID,"stash not imported");
        require(!(r.layout || r.gold) || r.expectedRevision>=0,INVALID,"layout/gold change needs expected revision");
        if(r.expectedRevision>=0) require(scalar(db_,"SELECT revision FROM meta WHERE singleton=1")==r.expectedRevision,STALE,"stash revision changed");
        Query layout(db_,"SELECT 1 FROM locks WHERE kind=5"); require(!layout.row(),BUSY,"layout reserved");
        if(r.layout) require(scalar(db_,"SELECT count(*) FROM locks")==0,BUSY,"stash operations pending");
        Query add(db_,"INSERT INTO operations(id,session,state,request) VALUES(?,?,0,?)"); add.text(1,r.operation).text(2,r.session).blob(3,request).row();
        if(r.layout) lock(db_,r.operation,LAYOUT);
        if(r.gold) lock(db_,r.operation,GOLD);
        std::set<Id> sources;
        for(size_t i=0;i<r.items.size();++i) {
            require(sources.insert(r.items[i].id).second,INVALID,"duplicate source item"); Item a=getItem(db_,r.items[i].id); require(a.version==r.items[i].version,STALE,"item version changed");
            lockFamily(db_,r.operation,a.id);
        }
        for(size_t i=0;i<r.targets.size();++i) {
            const Rect& t=r.targets[i]; checkRect(db_,t,width_,height_);
            if(t.parent) { Item root=getItem(db_,t.parent); require(!root.parent && root.page==t.page,INVALID,"invalid target cube"); lockFamily(db_,r.operation,t.parent); }
            lockRect(db_,r.operation,t);
            Query occupied(db_,"SELECT DISTINCT item FROM cells WHERE page=? AND parent=? AND x>=? AND x<? AND y>=? AND y<?"); occupied.number(1,t.page).number(2,t.parent).number(3,t.x).number(4,t.x+t.width).number(5,t.y).number(6,t.y+t.height);
            while(occupied.row()) require(sources.count(occupied.integer(0))!=0,NO_ROOM,"target occupied by unreserved item");
        }
        tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::commit(const std::string& op,const std::string& session,const Mutation& change,std::vector<Id>& created,Snapshot* committedSnapshot) {
    try {
        require(db_!=0,INVALID,"database not open"); Bytes encoded=encodeMutation(change); Transaction tx(db_); currentSession(db_,session);
        Query state(db_,"SELECT session,state,mutation,result FROM operations WHERE id=?"); state.text(1,op); require(state.row(),NOT_FOUND,"operation not found"); require(state.string(0)==session,INVALID,"operation belongs to another session");
        if(state.integer(1)==1) {
            require(state.bytes(2)==encoded,INVALID,"committed operation has different mutation"); Bytes bytes=state.bytes(3); require(bytes.size()%8==0,STORE_ERROR,"invalid operation result"); std::vector<Id> ids;
            for(size_t k=0;k<bytes.size();k+=8) { unsigned long long n=0; for(int j=7;j>=0;--j) n=(n<<8)|bytes[k+j]; ids.push_back((Id)n); }
            Snapshot current; if(committedSnapshot) readSnapshot(db_,current); tx.finish(); created.swap(ids); if(committedSnapshot) publishSnapshot(*committedSnapshot,current); return COMMITTED;
        }
        require(state.integer(1)==0,ABORTED,"operation was canceled");
        bool layout=owns(db_,op,LAYOUT); require((change.putPages.empty() && change.erasePages.empty()) || layout,INVALID,"page changes need layout reservation");
        std::vector<Id> ids;
        std::map<Id,Id> newPages;
        std::set<Id> pageChanges;
        for(size_t i=0;i<change.putPages.size();++i) {
            Page p=change.putPages[i]; validPage(p);
            require(!p.id || pageChanges.insert(p.id).second,INVALID,"duplicate page mutation");
            if(p.id>0) { require(pageExists(db_,p.id),STALE,"page no longer exists"); Query q(db_,"UPDATE pages SET ordinal=?,flags=?,name=? WHERE id=?"); q.number(1,p.ordinal).number(2,p.flags).text(3,p.name).number(4,p.id).row(); }
            else { Query q(db_,"INSERT INTO pages(ordinal,flags,name) VALUES(?,?,?)"); q.number(1,p.ordinal).number(2,p.flags).text(3,p.name).row(); Id id=sqlite3_last_insert_rowid(db_); ids.push_back(id); if(p.id<0) newPages[p.id]=id; }
        }
        std::set<Id> removed, changedParents, explicitItems;
        for(size_t i=0;i<change.erase.size();++i) { Id id=change.erase[i]; require(removed.insert(id).second,INVALID,"duplicate item mutation"); Item a=getItem(db_,id); require(canChangeItem(db_,op,a),INVALID,"item not reserved"); if(a.parent) changedParents.insert(a.parent); eraseItem(db_,id); }
        std::vector<Item> puts;
        std::map<Id,Id> newItems;
        for(size_t i=0;i<change.put.size();++i) {
            Item a=change.put[i];
            if(a.page<0) { require(newPages.count(a.page)!=0,INVALID,"unknown new-page alias"); a.page=newPages[a.page]; }
            if(a.id>0) { require(removed.insert(a.id).second,INVALID,"duplicate item mutation"); Item before=getItem(db_,a.id); require(canChangeItem(db_,op,before),INVALID,"item not reserved"); require(a.version==before.version,STALE,"item version changed"); if(before.parent) changedParents.insert(before.parent); a.version=before.version+1; eraseItem(db_,a.id); }
            else { a.version=1; }
            puts.push_back(a);
        }
        for(size_t i=0;i<puts.size();++i) {
            Item& a=puts[i]; if(a.id<=0) { // allocate all IDs before resolving cube aliases; roots may follow their children
                Id alias=a.id; require(!alias || newItems.count(alias)==0,INVALID,"duplicate item alias");
                Query q(db_,"INSERT INTO items(page,version,x,y,w,h,data) VALUES(?,?,?,?,?,?,?)"); q.number(1,a.page).number(2,a.version).number(3,a.x).number(4,a.y).number(5,a.width).number(6,a.height).blob(7,a.data).row(); a.id=sqlite3_last_insert_rowid(db_); ids.push_back(a.id); if(alias) newItems[alias]=a.id; eraseItem(db_,a.id);
            }
            explicitItems.insert(a.id);
        }
        for(size_t i=0;i<puts.size();++i) {
            Item& a=puts[i];
            if(a.parent<0) { require(newItems.count(a.parent)!=0,INVALID,"unknown cube alias"); a.parent=newItems[a.parent]; }
            checkRect(db_,rect(a),width_,height_); checkTargetLock(db_,op,a);
            if(a.parent) changedParents.insert(a.parent);
            putItem(db_,a);
        }
        checkParents(db_);
        for(std::set<Id>::const_iterator p=changedParents.begin();p!=changedParents.end();++p) if(!explicitItems.count(*p)) {
            Query bump(db_,"UPDATE items SET version=version+1 WHERE id=?"); bump.number(1,*p).row();
        }
        for(size_t i=0;i<change.erasePages.size();++i) { Id page=change.erasePages[i]; require(pageExists(db_,page),STALE,"page no longer exists"); Query q(db_,"SELECT 1 FROM items WHERE page=?"); require(!q.number(1,page).row(),INVALID,"cannot delete occupied page"); Query erase(db_,"DELETE FROM pages WHERE id=?"); erase.number(1,page).row(); }
        checkPageOrder(db_);
        if(change.setGold) { require(owns(db_,op,GOLD),INVALID,"gold not reserved"); require(change.gold>=0 && change.gold<=4294967295LL,INVALID,"invalid gold"); Query q(db_,"UPDATE meta SET gold=? WHERE singleton=1"); q.number(1,change.gold).row(); }
        if(!change.put.empty() || !change.erase.empty() || !change.putPages.empty() || !change.erasePages.empty() || change.setGold)
            exec(db_,"UPDATE meta SET revision=revision+1 WHERE singleton=1");
        Bytes result; for(size_t i=0;i<ids.size();++i) encode(result,ids[i]);
        Query done(db_,"UPDATE operations SET state=1,mutation=?,result=? WHERE id=?"); done.blob(1,encoded).blob(2,result).text(3,op).row(); unlock(db_,op);
        Snapshot committed; if(committedSnapshot) readSnapshot(db_,committed); tx.finish(); created.swap(ids); if(committedSnapshot) publishSnapshot(*committedSnapshot,committed); return OK;
    } HANDLE_FAILURE
}
Status Store::cancel(const std::string& op,const std::string& session) {
    try {
        require(db_!=0,INVALID,"database not open"); Transaction tx(db_); currentSession(db_,session); Query q(db_,"SELECT session,state FROM operations WHERE id=?"); q.text(1,op); require(q.row(),NOT_FOUND,"operation not found"); require(q.string(0)==session,INVALID,"operation belongs to another session");
        if(q.integer(1)==1) return COMMITTED; if(q.integer(1)==2) return ABORTED;
        unlock(db_,op); Query cancel(db_,"UPDATE operations SET state=2 WHERE id=?"); cancel.text(1,op).row(); tx.finish(); return OK;
    } HANDLE_FAILURE
}
Status Store::backup(const char* destination) {
    sqlite3* target=0;
    try {
        require(db_!=0 && destination && *destination,INVALID,"invalid backup"); int opened=sqlite3_open(destination,&target); sqlCheck(target,opened);
        sqlite3_backup* b=sqlite3_backup_init(target,"main",db_,"main"); if(!b) sqlCheck(target,sqlite3_errcode(target));
        int rc=sqlite3_backup_step(b,-1); int finished=sqlite3_backup_finish(b); sqlCheck(target,rc); require(rc==SQLITE_DONE,STORE_ERROR,"backup incomplete"); sqlCheck(target,finished); sqlite3_close(target); return OK;
    } catch(const Failure& f) { error_=f.text; if(target) sqlite3_close(target); return f.status; } catch(const std::exception& f) { error_=f.what(); if(target) sqlite3_close(target); return STORE_ERROR; }
}
}
