#include "../src/common/SharedStashStore.h"
#include "../third_party/sqlite/sqlite3.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <sstream>

using namespace SharedStash;
static int failures=0,checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
#define STATUS(s,x,want) do { Status got=(x); ++checks; if(got!=(want)) { ++failures; printf("FAIL %d: %s => %d expected %d (%s)\n",__LINE__,#x,(int)got,(int)(want),(s).error().c_str()); } } while(0)
static std::string number(Id n) { std::ostringstream s; s<<n; return s.str(); }
static Item item(Id id,int x,int y,int w=1,int h=1) { Item a; a.id=id; a.page=1; a.x=x; a.y=y; a.width=w; a.height=h; a.version=1; a.data.push_back((unsigned char)id); return a; }
static Snapshot seed() { Snapshot s; Page p; p.id=1; p.name="Shared"; s.pages.push_back(p); s.gold=100; s.items.push_back(item(1,0,0,2,2)); s.items.push_back(item(2,3,0)); return s; }
static Reservation request(const char* name) { Reservation r; r.operation=name; r.session="session"; return r; }
static void sql(const std::string& path,const char* statement) { sqlite3* d=0; CHECK(sqlite3_open(path.c_str(),&d)==SQLITE_OK); char* error=0; int rc=sqlite3_exec(d,statement,0,0,&error); if(rc!=SQLITE_OK) printf("SQL: %s\n",error?error:""); CHECK(rc==SQLITE_OK); sqlite3_free(error); sqlite3_close(d); }
static int pragmaValue(sqlite3* db,const char* statement,int& value) {
    sqlite3_stmt* query=0; int rc=sqlite3_prepare_v2(db,statement,-1,&query,0);
    if(rc==SQLITE_OK) { rc=sqlite3_step(query); if(rc==SQLITE_ROW) { value=sqlite3_column_int(query,0); rc=SQLITE_OK; } else if(rc==SQLITE_DONE) rc=SQLITE_ERROR; }
    int finalized=sqlite3_finalize(query); return rc==SQLITE_OK ? finalized : rc;
}
static Id ledgerRows(const std::string& path,const char* statement,const std::string& token) {
    sqlite3* db=0; int rc=sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,0); CHECK(rc==SQLITE_OK); if(rc!=SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_stmt* query=0; rc=sqlite3_prepare_v2(db,statement,-1,&query,0); CHECK(rc==SQLITE_OK); Id rows=-1;
    if(rc==SQLITE_OK) { CHECK(sqlite3_bind_text(query,1,token.c_str(),(int)token.size(),SQLITE_TRANSIENT)==SQLITE_OK); rc=sqlite3_step(query); CHECK(rc==SQLITE_ROW); if(rc==SQLITE_ROW) rows=sqlite3_column_int64(query,0); }
    CHECK(sqlite3_finalize(query)==SQLITE_OK); CHECK(sqlite3_close(db)==SQLITE_OK); return rows;
}
// Explicit test executable command only; no corresponding control exists in PlugY.dll.
static int failFinalize(const char* path,const std::string& mode) {
    if(!path || !*path || (mode!="on" && mode!="off")) { fprintf(stderr,"Usage: SharedStashStoreTests.exe --fail-finalize <database> on|off\n"); return 2; }
    sqlite3* db=0; int rc=sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX,0);
    if(rc!=SQLITE_OK) { fprintf(stderr,"Cannot open test database: %s\n",sqlite3_errmsg(db)); sqlite3_close(db); return 1; }
    sqlite3_busy_timeout(db,5000); rc=sqlite3_exec(db,"BEGIN IMMEDIATE",0,0,0);
    int app=0,version=0; std::string error;
    if(rc==SQLITE_OK) rc=pragmaValue(db,"PRAGMA application_id",app);
    if(rc==SQLITE_OK) rc=pragmaValue(db,"PRAGMA user_version",version);
    if(rc!=SQLITE_OK) error=sqlite3_errmsg(db);
    else if(app!=0x50595348 || version!=2) error="Refusing database: expected shared stash application_id 0x50595348 and schema version 2";
    if(error.empty()) {
        const char* statement=mode=="on"
            ? "CREATE TRIGGER IF NOT EXISTS SharedStashTestFailFinalize BEFORE UPDATE OF state ON operations WHEN NEW.state=1 BEGIN SELECT RAISE(ABORT,'injected finalization failure'); END;"
            : "DROP TRIGGER IF EXISTS SharedStashTestFailFinalize;";
        rc=sqlite3_exec(db,statement,0,0,0); if(rc==SQLITE_OK) rc=sqlite3_exec(db,"COMMIT",0,0,0); if(rc!=SQLITE_OK) error=sqlite3_errmsg(db);
    }
    if(!error.empty()) { sqlite3_exec(db,"ROLLBACK",0,0,0); fprintf(stderr,"Finalization test control failed: %s\n",error.c_str()); sqlite3_close(db); return 1; }
    rc=sqlite3_close(db); if(rc!=SQLITE_OK) { fprintf(stderr,"Cannot close test database (%d)\n",rc); return 1; }
    printf("SharedStashTestFailFinalize %s: %s\n",mode.c_str(),path); return 0;
}

static void finalizeInjection(const std::string& path) {
    Store a,b; STATUS(a,a.open(path.c_str(),10,10),OK); STATUS(a,a.importOnce(seed()),OK); STATUS(a,a.registerSession("native-owner"),OK); STATUS(b,b.open(path.c_str(),10,10),OK); STATUS(b,b.registerSession("competitor"),OK);
    Reservation r=request("native-finalization"); r.session="native-owner"; r.items.push_back(ItemVersion(1,1)); STATUS(a,a.reserve(r),OK);
    CHECK(failFinalize(path.c_str(),"on")==0); CHECK(failFinalize(path.c_str(),"on")==0);
    Mutation move; move.erase.push_back(1); std::vector<Id> ids; Snapshot output; output.revision=-99; STATUS(a,a.commit(r.operation,r.session,move,ids,&output),STORE_ERROR); CHECK(a.error().find("injected finalization failure")!=std::string::npos && output.revision==-99);
    Snapshot beforeRetry; STATUS(a,a.snapshot(beforeRetry),OK); CHECK(beforeRetry.revision==1 && beforeRetry.items.size()==2 && beforeRetry.gold==100);
    Reservation competing=request("competing-native-action"); competing.session="competitor"; competing.items.push_back(ItemVersion(1,1)); STATUS(b,b.reserve(competing),BUSY);
    CHECK(failFinalize(path.c_str(),"off")==0); CHECK(failFinalize(path.c_str(),"off")==0);
    STATUS(a,a.commit(r.operation,r.session,move,ids,&output),OK); CHECK(output.revision==2 && output.items.size()==1);
    STATUS(a,a.commit(r.operation,r.session,move,ids,&output),COMMITTED); CHECK(output.revision==2 && output.items.size()==1); STATUS(b,b.reserve(competing),STALE);
    std::vector<std::string> pending; STATUS(a,a.pendingSessions(pending),OK); CHECK(pending.empty());
    // The CLI must never create a missing database or stamp a foreign/schema-1 database.
    std::string missing=path+".missing"; CHECK(failFinalize(missing.c_str(),"on")==1); CHECK(GetFileAttributesA(missing.c_str())==INVALID_FILE_ATTRIBUTES);
    std::string foreign=path+".foreign"; sql(foreign,"CREATE TABLE unrelated(value TEXT);"); CHECK(failFinalize(foreign.c_str(),"on")==1);
    sqlite3* foreignDb=0; CHECK(sqlite3_open_v2(foreign.c_str(),&foreignDb,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,0)==SQLITE_OK); int unchanged=9; CHECK(pragmaValue(foreignDb,"PRAGMA application_id",unchanged)==SQLITE_OK && unchanged==0); sqlite3_close(foreignDb);
    std::string version1=path+".v1"; sql(version1,"PRAGMA application_id=1348031304; PRAGMA user_version=1;"); CHECK(failFinalize(version1.c_str(),"off")==1);
}
static void basic(const std::string& path) {
    Store s; STATUS(s,s.open(path.c_str(),10,10),OK); STATUS(s,s.registerSession("session"),OK); STATUS(s,s.registerSession("session"),COMMITTED); Snapshot initial=seed(); STATUS(s,s.importOnce(initial),OK); STATUS(s,s.importOnce(initial),COMMITTED);
    Snapshot view; STATUS(s,s.snapshot(view),OK); CHECK(view.initialized && view.revision==1 && view.gold==100 && view.items.size()==2); CHECK(view.items[0].data==initial.items[0].data); Id polled=-1; STATUS(s,s.revision(polled),OK); CHECK(polled==view.revision);
    Reservation noop=request("no-shared-change"); noop.layout=true; noop.gold=true; noop.expectedRevision=view.revision; STATUS(s,s.reserve(noop),OK);
    Mutation unchanged; std::vector<Id> unchangedIds; unchangedIds.push_back(999); STATUS(s,s.commit(noop.operation,noop.session,unchanged,unchangedIds),OK); CHECK(unchangedIds.empty());
    Snapshot afterNoop; STATUS(s,s.snapshot(afterNoop),OK); CHECK(afterNoop.revision==view.revision && afterNoop.gold==view.gold && afterNoop.items.size()==view.items.size()); STATUS(s,s.revision(polled),OK); CHECK(polled==view.revision);
    STATUS(s,s.reserve(noop),COMMITTED); STATUS(s,s.commit(noop.operation,noop.session,unchanged,unchangedIds),COMMITTED); CHECK(unchangedIds.empty());
    std::vector<std::string> afterNoopPending; STATUS(s,s.pendingSessions(afterNoopPending),OK); CHECK(afterNoopPending.empty());
    Reservation r=request("move"); r.items.push_back(ItemVersion(1,1)); r.targets.push_back(Rect(1,5,5,2,2)); STATUS(s,s.reserve(r),OK); STATUS(s,s.reserve(r),PENDING);
    Reservation altered=r; altered.targets[0].x=6; STATUS(s,s.reserve(altered),INVALID); altered=r; altered.session="other"; STATUS(s,s.registerSession("other"),OK); STATUS(s,s.reserve(altered),INVALID);
    Store other; STATUS(other,other.open(path.c_str(),10,10),OK); Reservation conflict=request("take"); conflict.items.push_back(ItemVersion(1,1)); STATUS(other,other.reserve(conflict),BUSY);
    Reservation cell=request("cell"); cell.targets.push_back(Rect(1,6,6)); STATUS(other,other.reserve(cell),BUSY);
    Reservation page=request("layout-conflict"); page.layout=true; page.expectedRevision=view.revision; STATUS(other,other.reserve(page),BUSY);
    Mutation moved; Item a=initial.items[0]; a.x=5; a.y=5; moved.put.push_back(a); std::vector<Id> ids; STATUS(s,s.commit("move","other",moved,ids),INVALID); STATUS(s,s.commit("move","session",moved,ids),OK); STATUS(s,s.commit("move","session",moved,ids),COMMITTED);
    Mutation bad=moved; bad.put[0].x=6; STATUS(s,s.commit("move","session",bad,ids),INVALID); STATUS(s,s.cancel("move","session"),COMMITTED);
    STATUS(other,other.reserve(conflict),STALE); STATUS(s,s.snapshot(view),OK); CHECK(view.items[0].x==5 && view.items[0].version==2);
    Reservation occupied=request("occupied"); occupied.targets.push_back(Rect(1,6,6)); STATUS(other,other.reserve(occupied),NO_ROOM); // inside the 2x2 footprint
    Reservation independent=request("independent"); independent.items.push_back(ItemVersion(2,1)); independent.targets.push_back(Rect(1,7,0)); STATUS(other,other.reserve(independent),OK);
    Reservation freeCell=request("free"); freeCell.targets.push_back(Rect(1,8,8)); STATUS(s,s.reserve(freeCell),OK);
    std::vector<std::string> pendingOwners; STATUS(s,s.pendingSessions(pendingOwners),OK); CHECK(pendingOwners.size()==1 && pendingOwners[0]=="session");
    Mutation newItem; a=item(0,8,8); newItem.put.push_back(a); STATUS(s,s.commit("free","session",newItem,ids),OK); CHECK(ids.size()==1 && ids[0]>2); std::vector<Id> savedIds=ids; STATUS(s,s.commit("free","session",newItem,ids),COMMITTED); CHECK(ids==savedIds);
    STATUS(other,other.cancel("independent","wrong"),INVALID); STATUS(other,other.cancel("independent","session"),OK); STATUS(other,other.reserve(independent),ABORTED);
    Reservation invalid=request("outside"); invalid.targets.push_back(Rect(1,9,9,2,2)); STATUS(s,s.reserve(invalid),INVALID);
    Reservation unreserved=request("unreserved"); STATUS(s,s.reserve(unreserved),OK); Mutation theft; theft.erase.push_back(2); STATUS(s,s.commit("unreserved","session",theft,ids),INVALID); STATUS(s,s.cancel("unreserved","session"),OK);
    // Gold is version checked, serialized with item changes, and bounded like D2's DWORD.
    STATUS(s,s.snapshot(view),OK); Reservation gold=request("gold"); gold.gold=true; gold.expectedRevision=view.revision; STATUS(s,s.reserve(gold),OK);
    Mutation goldChange; goldChange.setGold=true; goldChange.gold=75; STATUS(s,s.commit("gold","session",goldChange,ids),OK);
    gold.operation="stale-gold"; STATUS(s,s.reserve(gold),STALE);
    STATUS(s,s.snapshot(view),OK); gold.operation="invalid-gold"; gold.expectedRevision=view.revision; STATUS(s,s.reserve(gold),OK); goldChange.gold=-1; STATUS(s,s.commit("invalid-gold","session",goldChange,ids),INVALID); STATUS(s,s.cancel("invalid-gold","session"),OK);
    // Pending ownership survives close/reopen. A failed finalize keeps it, with no partial item change.
    Reservation pending=request("pending"); pending.items.push_back(ItemVersion(2,1)); pending.targets.push_back(Rect(1,9,0)); STATUS(s,s.reserve(pending),OK); s.close(); STATUS(s,s.open(path.c_str(),10,10),OK); STATUS(s,s.reserve(pending),PENDING);
    sql(path,"CREATE TRIGGER injected_failure BEFORE INSERT ON items BEGIN SELECT RAISE(ABORT,'injected write failure'); END;");
    Mutation retry; a=initial.items[1]; a.x=9; retry.put.push_back(a); STATUS(s,s.commit("pending","session",retry,ids),STORE_ERROR);
    STATUS(s,s.snapshot(view),OK); CHECK(view.items[1].x==3 && view.items[1].version==1); conflict.operation="blocked-after-error"; conflict.items[0]=ItemVersion(2,1); STATUS(other,other.reserve(conflict),BUSY);
    sql(path,"DROP TRIGGER injected_failure;"); STATUS(s,s.commit("pending","session",retry,ids),OK);
    // A single mutation can exchange two occupied footprints without an intermediate overlap.
    STATUS(s,s.snapshot(view),OK); Reservation swap=request("swap"); swap.items.push_back(ItemVersion(1,2)); swap.items.push_back(ItemVersion(2,2)); swap.targets.push_back(Rect(1,5,5,2,2)); swap.targets.push_back(Rect(1,9,0)); STATUS(s,s.reserve(swap),OK);
    Mutation exchange; Item first=view.items[0],second=view.items[1]; first.x=8; first.y=0; // need a 2x2 target: reservation must cover its full size
    exchange.put.push_back(first); second.x=5; second.y=5; exchange.put.push_back(second); STATUS(s,s.commit("swap","session",exchange,ids),INVALID); STATUS(s,s.cancel("swap","session"),OK);
    swap.operation="swap-fit"; swap.targets.clear(); swap.targets.push_back(Rect(1,8,0,2,2)); swap.targets.push_back(Rect(1,5,5)); STATUS(s,s.reserve(swap),OK); STATUS(s,s.commit("swap-fit","session",exchange,ids),OK);
    // Layout changes require a current revision and wait for all other reservations.
    STATUS(s,s.snapshot(view),OK); Reservation layout=request("layout"); layout.layout=true; STATUS(s,s.reserve(layout),INVALID); layout.expectedRevision=view.revision; STATUS(s,s.reserve(layout),OK);
    freeCell.operation="during-layout"; freeCell.targets[0]=Rect(1,0,9); STATUS(other,other.reserve(freeCell),BUSY);
    Mutation addPage; Page p; p.id=-1; p.ordinal=1; p.name="More"; addPage.putPages.push_back(p); Item pageItem=item(0,0,0); pageItem.page=-1; addPage.put.push_back(pageItem); STATUS(s,s.commit("layout","session",addPage,ids),OK); CHECK(ids.size()==2 && ids[0]>1); STATUS(s,s.snapshot(view),OK); CHECK(view.items.back().page==ids[0] && view.items.back().id==ids[1]);
    layout.operation="stale-layout"; STATUS(s,s.reserve(layout),STALE);
    std::string backupPath=path+".backup"; STATUS(s,s.backup(backupPath.c_str()),OK); Store copy; STATUS(copy,copy.open(backupPath.c_str(),10,10),OK); Snapshot copied; STATUS(copy,copy.snapshot(copied),OK); STATUS(s,s.snapshot(view),OK); CHECK(copied.revision==view.revision && copied.gold==view.gold && copied.items.size()==view.items.size() && copied.pages.size()==2);
    Store mismatch; STATUS(mismatch,mismatch.open(path.c_str(),6,8),INVALID);
    // Real database writer contention has a bounded BUSY result.
    sqlite3* blocker=0; CHECK(sqlite3_open(path.c_str(),&blocker)==SQLITE_OK); CHECK(sqlite3_exec(blocker,"BEGIN IMMEDIATE",0,0,0)==SQLITE_OK); freeCell.operation="db-busy"; STATUS(s,s.reserve(freeCell),BUSY); sqlite3_exec(blocker,"ROLLBACK",0,0,0); sqlite3_close(blocker);
}

static Item findItem(const Snapshot& s,Id id) { for(size_t k=0;k<s.items.size();++k) if(s.items[k].id==id) return s.items[k]; return Item(); }
static void cubes(const std::string& path) {
    Store s,other; STATUS(s,s.open(path.c_str(),10,10),OK); STATUS(s,s.registerSession("cube"),OK);
    Snapshot initial=seed(); initial.items.clear(); initial.items.push_back(item(1,0,0,2,2)); Item a=item(2,0,0); a.parent=1; initial.items.push_back(a); initial.items.push_back(item(3,3,0,2,2)); a=item(4,0,0); a.parent=3; initial.items.push_back(a); a=item(5,1,0); a.parent=1; initial.items.push_back(a);
    STATUS(s,s.importOnce(initial),OK); STATUS(other,other.open(path.c_str(),10,10),OK); STATUS(other,other.registerSession("other-cube"),OK);
    Reservation r=request("child-move"); r.session="cube"; r.items.push_back(ItemVersion(2,1)); r.targets.push_back(Rect(1,1,1,1,1,1)); STATUS(s,s.reserve(r),OK); Reservation changedParent=r; changedParent.targets[0].parent=3; STATUS(s,s.reserve(changedParent),INVALID);
    std::vector<std::string> pending; STATUS(s,s.pendingSessions(pending),OK); CHECK(pending.size()==1 && pending[0]=="cube");
    STATUS(s,s.endSession("cube"),BUSY); STATUS(s,s.recoverDeadSession("cube"),BUSY);
    Reservation conflict=request("root-conflict"); conflict.session="other-cube"; conflict.items.push_back(ItemVersion(1,1)); STATUS(other,other.reserve(conflict),BUSY); conflict.operation="sibling-conflict"; conflict.items[0]=ItemVersion(5,1); STATUS(other,other.reserve(conflict),BUSY);
    conflict.operation="target-cube-conflict"; conflict.items.clear(); conflict.targets.push_back(Rect(1,2,3,1,1,1)); STATUS(other,other.reserve(conflict),BUSY);
    Reservation independent=request("other-child"); independent.session="other-cube"; independent.items.push_back(ItemVersion(4,1)); independent.targets.push_back(Rect(1,1,0,1,1,3)); STATUS(other,other.reserve(independent),OK); STATUS(other,other.cancel(independent.operation,independent.session),OK);
    Mutation move; a=findItem(initial,2); a.x=1; a.y=1; move.put.push_back(a); std::vector<Id> ids; STATUS(s,s.commit(r.operation,r.session,move,ids),OK); Snapshot view; STATUS(s,s.snapshot(view),OK); CHECK(findItem(view,1).version==2 && findItem(view,2).version==2 && findItem(view,2).parent==1); Mutation alteredParent=move; alteredParent.put[0].parent=3; STATUS(s,s.commit(r.operation,r.session,alteredParent,ids),INVALID);
    conflict=request("old-root"); conflict.session="cube"; conflict.items.push_back(ItemVersion(1,1)); STATUS(s,s.reserve(conflict),STALE);
    // Bounds are the cube's 3x4 grid, regardless of the stash's size.
    conflict=request("cube-outside"); conflict.session="cube"; conflict.targets.push_back(Rect(1,2,3,2,1,1)); STATUS(s,s.reserve(conflict),INVALID);
    Reservation page=request("layout-exclusive"); page.session="cube"; page.layout=true; page.expectedRevision=view.revision; STATUS(s,s.reserve(page),OK);
    Mutation nested; a=item(-10,0,0); a.parent=2; nested.put.push_back(a); STATUS(s,s.commit(page.operation,page.session,nested,ids),INVALID); STATUS(s,s.cancel(page.operation,page.session),OK);
    Reservation layout=request("root-move"); layout.session="cube"; layout.layout=true; layout.expectedRevision=view.revision; STATUS(s,s.reserve(layout),OK);
    Mutation relocate; Page p; p.id=-1; p.ordinal=1; relocate.putPages.push_back(p); a=findItem(view,1); a.page=-1; relocate.put.push_back(a); STATUS(s,s.commit(layout.operation,layout.session,relocate,ids),INVALID); // children must move with root
    a=findItem(view,2); a.page=-1; relocate.put.push_back(a); a=findItem(view,5); a.page=-1; relocate.put.push_back(a); STATUS(s,s.commit(layout.operation,layout.session,relocate,ids),OK); CHECK(ids.size()==1); Id newPage=ids.empty()?0:ids[0]; STATUS(s,s.snapshot(view),OK); CHECK(findItem(view,1).page==newPage && findItem(view,2).page==newPage && findItem(view,5).page==newPage);
    layout.operation="new-cube"; layout.expectedRevision=view.revision; STATUS(s,s.reserve(layout),OK); Mutation deposit; a=item(-11,0,0); a.parent=-10; deposit.put.push_back(a); a=item(-10,7,0,2,2); deposit.put.push_back(a); STATUS(s,s.commit(layout.operation,layout.session,deposit,ids),OK); CHECK(ids.size()==2); if(ids.size()==2) { STATUS(s,s.snapshot(view),OK); CHECK(findItem(view,ids[0]).parent==ids[1] && findItem(view,ids[1]).parent==0); }
    // Removing a cube alone cannot leave dangling children; the same pending transaction can retry correctly.
    Reservation erase=request("erase-cube"); erase.session="cube"; erase.items.push_back(ItemVersion(1,findItem(view,1).version)); STATUS(s,s.reserve(erase),OK); Mutation remove; remove.erase.push_back(1); STATUS(s,s.commit(erase.operation,erase.session,remove,ids),INVALID); remove.erase.push_back(2); remove.erase.push_back(5); STATUS(s,s.commit(erase.operation,erase.session,remove,ids),OK);
    STATUS(s,s.pendingSessions(pending),OK); CHECK(pending.empty()); STATUS(s,s.endSession("cube"),OK); STATUS(s,s.endSession("cube"),COMMITTED); STATUS(s,s.registerSession("cube"),INVALID); erase.operation="ended-session"; STATUS(s,s.reserve(erase),INVALID); STATUS(s,s.snapshot(view),OK);
}


static void commitSnapshots(const std::string& path) {
    Store a,b; STATUS(a,a.open(path.c_str(),10,10),OK); Snapshot initial=seed(); initial.items[1].parent=1; initial.items[1].x=0; STATUS(a,a.importOnce(initial),OK); STATUS(a,a.registerSession("a"),OK); STATUS(b,b.open(path.c_str(),10,10),OK); STATUS(b,b.registerSession("b"),OK);
    Reservation first=request("a-child-move"); first.session="a"; first.items.push_back(ItemVersion(2,1)); first.targets.push_back(Rect(1,1,0,1,1,1)); STATUS(a,a.reserve(first),OK);
    Mutation change; Item child=findItem(initial,2); child.x=1; change.put.push_back(child); std::vector<Id> created; Snapshot committed;
    STATUS(a,a.commit(first.operation,first.session,change,created,&committed),OK); CHECK(committed.revision==2 && findItem(committed,1).version==2 && findItem(committed,2).version==2 && findItem(committed,2).x==1 && findItem(committed,2).y==0);
    Reservation foreign=request("b-child-move"); foreign.session="b"; foreign.items.push_back(ItemVersion(2,2)); foreign.targets.push_back(Rect(1,1,1,1,1,1)); STATUS(b,b.reserve(foreign),OK);
    Mutation next; child=findItem(committed,2); child.y=1; next.put.push_back(child); STATUS(b,b.commit(foreign.operation,foreign.session,next,created),OK);
    CHECK(committed.revision==2 && findItem(committed,1).version==2 && findItem(committed,2).y==0); // A's returned result cannot become B's newer view
    Id latest=-1; STATUS(a,a.revision(latest),OK); CHECK(latest==3);
    STATUS(a,a.commit(first.operation,first.session,change,created,&committed),COMMITTED); CHECK(committed.revision==3 && findItem(committed,1).version==3 && findItem(committed,2).y==1); // retry returns current state for explicit reconciliation
    Reservation failing=request("a-failing-move"); failing.session="a"; failing.items.push_back(ItemVersion(2,3)); failing.targets.push_back(Rect(1,2,2,1,1,1)); STATUS(a,a.reserve(failing),OK);
    Mutation last; child=findItem(committed,2); child.x=2; child.y=2; last.put.push_back(child); created.push_back(987); Snapshot beforeFailure=committed;
    sql(path,"CREATE TRIGGER snapshot_failure BEFORE INSERT ON items BEGIN SELECT RAISE(ABORT,'injected commit snapshot failure'); END;");
    STATUS(a,a.commit(failing.operation,failing.session,last,created,&committed),STORE_ERROR); CHECK(created.size()==1 && created[0]==987); CHECK(committed.revision==beforeFailure.revision && committed.gold==beforeFailure.gold && committed.pages.size()==beforeFailure.pages.size() && committed.items.size()==beforeFailure.items.size() && findItem(committed,2).x==findItem(beforeFailure,2).x && findItem(committed,2).y==findItem(beforeFailure,2).y && findItem(committed,1).version==findItem(beforeFailure,1).version);
    sql(path,"DROP TRIGGER snapshot_failure;"); STATUS(a,a.commit(failing.operation,failing.session,last,created,&committed),OK); CHECK(created.empty() && committed.revision==4 && findItem(committed,1).version==4 && findItem(committed,2).version==4 && findItem(committed,2).x==2 && findItem(committed,2).y==2);
}
static void sessionLifecycle(const std::string& path) {
    Store s; STATUS(s,s.open(path.c_str(),10,10),OK); STATUS(s,s.importOnce(seed()),OK); STATUS(s,s.registerSession("old-game"),OK);
    Reservation r=request("old-native-action"); r.session="old-game"; r.items.push_back(ItemVersion(1,1)); STATUS(s,s.reserve(r),OK);
    STATUS(s,s.endSession("old-game"),BUSY); STATUS(s,s.recoverDeadSession("old-game"),BUSY);
    STATUS(s,s.abandonSession("old-game"),OK); STATUS(s,s.abandonSession("old-game"),COMMITTED); STATUS(s,s.registerSession("old-game"),INVALID);
    std::vector<std::string> pending; STATUS(s,s.pendingSessions(pending),OK); CHECK(pending.empty());
    Mutation take; take.erase.push_back(1); std::vector<Id> ids; STATUS(s,s.commit(r.operation,r.session,take,ids),INVALID);
    STATUS(s,s.registerSession("next-game"),OK); r.operation="next-native-action"; r.session="next-game"; STATUS(s,s.reserve(r),OK); STATUS(s,s.cancel(r.operation,r.session),OK);
    // A registered identity from a different creation time must not protect a reused PID forever.
    STATUS(s,s.registerSession("reused-pid"),OK); r.operation="old-pid-action"; r.session="reused-pid"; STATUS(s,s.reserve(r),OK); sql(path,"UPDATE sessions SET creation=creation-1 WHERE token='reused-pid';");
    STATUS(s,s.recoverDeadSession("reused-pid"),OK); STATUS(s,s.pendingSessions(pending),OK); CHECK(pending.empty());
    STATUS(s,s.registerSession("next-game"),COMMITTED); r.operation="after-pid-recovery"; r.session="next-game"; STATUS(s,s.reserve(r),OK); STATUS(s,s.cancel(r.operation,r.session),OK);
}

static void ledgerLifetime(const std::string& path) {
    Store s; STATUS(s,s.open(path.c_str(),10,10),OK); STATUS(s,s.importOnce(seed()),OK); STATUS(s,s.registerSession("game-a"),OK); STATUS(s,s.registerSession("game-b"),OK);
    Reservation finished=request("a-finished"); finished.session="game-a"; STATUS(s,s.reserve(finished),OK); Mutation noop; std::vector<Id> ids; STATUS(s,s.commit(finished.operation,finished.session,noop,ids),OK); STATUS(s,s.commit(finished.operation,finished.session,noop,ids),COMMITTED);
    Reservation aborted=request("a-aborted"); aborted.session="game-a"; STATUS(s,s.reserve(aborted),OK); STATUS(s,s.cancel(aborted.operation,aborted.session),OK);
    Reservation pendingA=request("a-pending"); pendingA.session="game-a"; pendingA.items.push_back(ItemVersion(1,1)); STATUS(s,s.reserve(pendingA),OK);
    Reservation pendingB=request("b-pending"); pendingB.session="game-b"; pendingB.items.push_back(ItemVersion(2,1)); STATUS(s,s.reserve(pendingB),OK);
    STATUS(s,s.endSession("game-a"),BUSY); CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?","game-a")==3); CHECK(ledgerRows(path,"SELECT count(*) FROM sessions WHERE token=? AND ended=1","game-a")==0);
    STATUS(s,s.cancel(pendingA.operation,pendingA.session),OK); STATUS(s,s.endSession("game-a"),OK);
    CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?","game-a")==0); CHECK(ledgerRows(path,"SELECT count(*) FROM sessions WHERE token=? AND ended=1","game-a")==1); CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?","game-b")==1);
    STATUS(s,s.commit(finished.operation,finished.session,noop,ids),INVALID); STATUS(s,s.reserve(finished),INVALID); STATUS(s,s.registerSession("game-a"),INVALID);
    std::vector<std::string> pending; STATUS(s,s.pendingSessions(pending),OK); CHECK(pending.size()==1 && pending[0]=="game-b"); STATUS(s,s.commit(pendingB.operation,pendingB.session,noop,ids),OK); STATUS(s,s.commit(pendingB.operation,pendingB.session,noop,ids),COMMITTED);
    STATUS(s,s.registerSession("game-c"),OK); Reservation c=request("c-pending"); c.session="game-c"; c.items.push_back(ItemVersion(1,1)); STATUS(s,s.reserve(c),OK); Reservation completedC=request("c-finished"); completedC.session="game-c"; STATUS(s,s.reserve(completedC),OK); STATUS(s,s.commit(completedC.operation,completedC.session,noop,ids),OK);
    STATUS(s,s.abandonSession("game-c"),OK); CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?","game-c")==0); CHECK(ledgerRows(path,"SELECT count(*) FROM sessions WHERE token=? AND ended=1","game-c")==1); CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?","game-b")==1);
    STATUS(s,s.reserve(c),INVALID); STATUS(s,s.commit(c.operation,c.session,noop,ids),INVALID); STATUS(s,s.pendingSessions(pending),OK); CHECK(pending.empty()); STATUS(s,s.endSession("game-b"),OK); CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?","game-b")==0);
}
static void unsupportedSchema(const std::string& path, bool unfinished) {
    sql(path, "CREATE TABLE saved_data(value BLOB); INSERT INTO saved_data VALUES(X'0123');"
        "PRAGMA application_id=1348031304; PRAGMA user_version=1;");
    if (unfinished)
        sql(path, "CREATE TABLE operations(state); INSERT INTO operations VALUES(0);");

    Store store;
    STATUS(store, store.open(path.c_str(), 10, 10), INVALID);
    sqlite3* db = 0;
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, 0) == SQLITE_OK);
    int value = 0;
    CHECK(pragmaValue(db, "PRAGMA application_id", value) == SQLITE_OK && value == 0x50595348);
    CHECK(pragmaValue(db, "PRAGMA user_version", value) == SQLITE_OK && value == 1);
    CHECK(pragmaValue(db, "SELECT count(*) FROM saved_data WHERE value=X'0123'", value) == SQLITE_OK && value == 1);
    CHECK(pragmaValue(db, "SELECT count(*) FROM sqlite_master WHERE type='table'", value) == SQLITE_OK && value == (unfinished ? 2 : 1));
    if (unfinished)
        CHECK(pragmaValue(db, "SELECT count(*) FROM operations WHERE state=0", value) == SQLITE_OK && value == 1);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static int child(int argc,char** argv) {
    if(argc!=7) return 9;
    Store s; Status status=s.open(argv[2],10,10,5000); if(status!=OK) { printf("child open %d: %s\n",status,s.error().c_str()); return 8; }
    if(s.registerSession(argv[4])!=OK) return 10;
    std::string eventName=argv[3],readyName=eventName+"-ready"; HANDLE event=OpenEventA(SYNCHRONIZE,FALSE,eventName.c_str()),ready=OpenSemaphoreA(SEMAPHORE_MODIFY_STATE,FALSE,readyName.c_str());
    if(!event || !ready) return 7; ReleaseSemaphore(ready,1,0); DWORD wait=WaitForSingleObject(event,15000); CloseHandle(event); CloseHandle(ready); if(wait!=WAIT_OBJECT_0) return 6;
    int index=atoi(argv[5]),mode=atoi(argv[6]);
    if(mode==3) { status=s.importOnce(seed()); return status==OK ? 0 : status==COMMITTED ? 2 : 5; }
    Reservation r=request(argv[4]); r.session=argv[4]; Mutation m;
    if(mode==5) { r.items.push_back(ItemVersion(2,1)); r.targets.push_back(Rect(1,1,1,1,1,1)); Item a=item(2,1,1); a.parent=1; m.put.push_back(a); }
    else if(mode==0 || mode==4) { r.items.push_back(ItemVersion(1,1)); m.erase.push_back(1); }
    else { int x=mode==1?5:index; r.targets.push_back(Rect(1,x,5)); m.put.push_back(item(0,x,5)); }
    status=s.reserve(r); if(status==BUSY || status==STALE || status==NO_ROOM) return 2; if(status!=OK) { printf("reserve %d: %s\n",status,s.error().c_str()); return 5; }
    if(mode==4) ExitProcess(0); // no Store destructor: database must keep the committed reservation
    Sleep(50); std::vector<Id> ids; status=s.commit(r.operation,r.session,m,ids); if(status!=OK) { printf("commit %d: %s\n",status,s.error().c_str()); return 4; } return 0;
}
static void race(const std::string& path,int mode) {
    { Store s; STATUS(s,s.open(path.c_str(),10,10,5000),OK); if(mode!=3) { Snapshot initial=seed(); if(mode==5) { initial.items[1].parent=1; initial.items[1].x=0; } STATUS(s,s.importOnce(initial),OK); } }
    const int count=8; std::string name="Local\\PlugYStashTest-"+number(GetCurrentProcessId())+"-"+number(mode),readyName=name+"-ready";
    HANDLE event=CreateEventA(0,TRUE,FALSE,name.c_str()),ready=CreateSemaphoreA(0,0,count,readyName.c_str()); CHECK(event && ready);
    char exe[MAX_PATH]; CHECK(GetModuleFileNameA(0,exe,MAX_PATH)>0); HANDLE processes[count]={0};
    for(int i=0;i<count;++i) { std::string command="\""+std::string(exe)+"\" --race \""+path+"\" \""+name+"\" op-"+number(i)+" "+number(i)+" "+number(mode); std::vector<char> cmd(command.begin(),command.end()); cmd.push_back(0); STARTUPINFOA si={sizeof(si)}; PROCESS_INFORMATION pi={0}; BOOL made=CreateProcessA(0,&cmd[0],0,0,FALSE,CREATE_NO_WINDOW,0,0,&si,&pi); CHECK(made!=FALSE); if(made) { processes[i]=pi.hProcess; CloseHandle(pi.hThread); } }
    for(int i=0;i<count;++i) CHECK(WaitForSingleObject(ready,15000)==WAIT_OBJECT_0); SetEvent(event);
    int won=0,lost=0,winner=-1;
    for(int i=0;i<count;++i) if(processes[i]) { CHECK(WaitForSingleObject(processes[i],15000)==WAIT_OBJECT_0); DWORD code=99; GetExitCodeProcess(processes[i],&code); if(code==0) { ++won; winner=i; } else if(code==2) ++lost; else { printf("unexpected child exit %lu\n",code); CHECK(false); } CloseHandle(processes[i]); }
    CloseHandle(event); CloseHandle(ready); CHECK(mode==2 ? won==count : won==1 && lost==count-1);
    Store s; STATUS(s,s.open(path.c_str(),10,10),OK); Snapshot v; STATUS(s,s.snapshot(v),OK); CHECK(v.items.size()==(mode==0?1:mode==1?3:mode==2?10:2));
    if(mode==4 && winner>=0) { STATUS(s,s.registerSession("session"),OK); Reservation r=request("after-owner-exit"); r.items.push_back(ItemVersion(1,1)); STATUS(s,s.reserve(r),BUSY); std::string old="op-"+number(winner); STATUS(s,s.cancel(old,old),INVALID); STATUS(s,s.recoverDeadSession(old),OK); CHECK(ledgerRows(path,"SELECT count(*) FROM operations WHERE session=?",old)==0); CHECK(ledgerRows(path,"SELECT count(*) FROM sessions WHERE token=? AND ended=1",old)==1); STATUS(s,s.recoverDeadSession(old),COMMITTED); STATUS(s,s.reserve(r),OK); STATUS(s,s.cancel(r.operation,r.session),OK); }
    if(mode==5) CHECK(findItem(v,1).version==2 && findItem(v,2).parent==1 && findItem(v,2).x==1 && findItem(v,2).y==1);
    printf("Process race %d: %d committed, %d refused\n",mode,won,lost);
}
int main(int argc,char** argv) {
    setvbuf(stdout,0,_IONBF,0);
    if(argc>1 && std::string(argv[1])=="--fail-finalize") {
        if(argc!=4) { fprintf(stderr,"Usage: SharedStashStoreTests.exe --fail-finalize <database> on|off\n"); return 2; }
        return failFinalize(argv[2],argv[3]);
    }
    if(argc>1 && std::string(argv[1])=="--race") return child(argc,argv);
    if(argc!=1) { fprintf(stderr,"Unknown test command\n"); return 2; }
    char temp[MAX_PATH]; CHECK(GetTempPathA(MAX_PATH,temp)>0); std::string folder=std::string(temp)+"PlugYStashTests-"+number(GetCurrentProcessId())+"-"+number(GetTickCount64()); CHECK(CreateDirectoryA(folder.c_str(),0)!=FALSE);
    basic(folder+"\\basic.sqlite3"); finalizeInjection(folder+"\\finalize-cli.sqlite3"); cubes(folder+"\\cube.sqlite3"); sessionLifecycle(folder+"\\sessions.sqlite3"); ledgerLifetime(folder+"\\ledger.sqlite3"); commitSnapshots(folder+"\\commit-snapshots.sqlite3"); unsupportedSchema(folder+"\\v1.sqlite3",false); unsupportedSchema(folder+"\\v1-pending.sqlite3",true); race(folder+"\\withdraw.sqlite3",0); race(folder+"\\cell.sqlite3",1); race(folder+"\\independent.sqlite3",2); race(folder+"\\concurrent-import.sqlite3",3); race(folder+"\\owner-exit.sqlite3",4); race(folder+"\\cube-race.sqlite3",5);
    Store closed; Id untouched=999; STATUS(closed,closed.revision(untouched),INVALID); CHECK(untouched==999);
    Store missingMeta; std::string missingMetaPath=folder+"\\missing-meta.sqlite3"; STATUS(missingMeta,missingMeta.open(missingMetaPath.c_str(),10,10),OK); sql(missingMetaPath,"DELETE FROM meta;"); STATUS(missingMeta,missingMeta.revision(untouched),STORE_ERROR); CHECK(untouched==999);
    Store foreign; std::string foreignPath=folder+"\\foreign.sqlite3"; sql(foreignPath,"CREATE TABLE unrelated(value TEXT);"); STATUS(foreign,foreign.open(foreignPath.c_str(),10,10),INVALID);
    Store invalidImport; STATUS(invalidImport,invalidImport.open((folder+"\\import.sqlite3").c_str(),10,10),OK); Snapshot bad=seed(); bad.items[1].x=1; STATUS(invalidImport,invalidImport.importOnce(bad),NO_ROOM); Snapshot clean; STATUS(invalidImport,invalidImport.snapshot(clean),OK); CHECK(!clean.initialized && clean.pages.empty() && clean.items.empty()); STATUS(invalidImport,invalidImport.importOnce(seed()),OK);
    printf("SharedStashStore: %d checks, %d failures; artifacts %s\n",checks,failures,folder.c_str()); return failures?1:0;
}
