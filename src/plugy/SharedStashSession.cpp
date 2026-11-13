#include "sharedStashSession.h"
#include "sharedStashSnapshot.h"
#include "sharedStashCubeSwapRules.h"
#include "SharedStashNative.h"
#include "sharedStashTransport.h"
#include "sharedSaveFile.h"
#include "savePlayerData.h"
#include "infinityStash.h"
#include "stashProtocol.h"
#include "updateClient.h"
#include "customLibraries.h"
#include "../common/SharedStashCleanup.h"
#ifdef PLUGY_TEST_HOOKS
#include "sharedStashTest.h"
#endif
#include <map>
#include <set>
#include <algorithm>
#include <stdio.h>

namespace {
using namespace SharedStash;
struct ParkedItem { Unit* item; BYTE x, y; DWORD gid; };
struct CommandStream {
	Id revision; bool active, discard;
	CommandStream() : revision(-1), active(false), discard(false) {}
};
struct FineAction {
	bool active, deposit, stamped;
	DWORD gid, ordinal;
	Id item, version, page, viewRevision, requestRevision;
	unsigned long long counter;
	int x, y, width, height;
	FineAction() : active(false), deposit(false), stamped(false), gid(0), ordinal(0), item(0), version(0),
		page(0), viewRevision(-1), requestRevision(-1), counter(0), x(0), y(0), width(0), height(0) {}
};
struct LiveSession {
	Store store;
	Unit* character;
	PlayerData* owner;
	Game* game;
	void* client;
	Snapshot snapshot;
	std::map<Id, DWORD> units;
	std::map<Id, Stash*> pages;
	std::string token, operation;
	Mutation mutation;
	CommandStream nameCommand, swapCommand;
	FineAction fine;
	Snapshot result;
	std::map<Id, DWORD> resultUnits;
	std::map<Id, Stash*> resultPages;
	bool repairCursor;
	std::vector<DWORD> actionGids;
	std::vector<ParkedItem> privateCube, saveCube, saveRoots;
	Stash* saveRootPage;
	bool privateCubeExposed;
	Id cube;
	DWORD cubeGid, poll;
	bool pending, applied, recorded, captureFailed, failed, closing, saving, syncing, passive, tearingDown;
	LiveSession() : character(NULL), owner(NULL), game(NULL), client(NULL), repairCursor(false), saveRootPage(NULL), privateCubeExposed(false), cube(0), cubeGid(0), poll(0),
		pending(false), applied(false), recorded(false), captureFailed(false), failed(false), closing(false), saving(false), syncing(false), passive(false), tearingDown(false) {}
};
struct SyncGuard {
	bool& value; bool before;
	SyncGuard(bool& flag) : value(flag), before(flag) { value = true; }
	~SyncGuard() { value = before; }
};
std::vector<LiveSession*> sessions;
bool installed = false;
LONG sessionCounter = 0;

void clearNativeState(LiveSession& session)
{
	session.character = NULL;
	session.owner = NULL;
	session.game = NULL;
	session.client = NULL;
	session.saveRootPage = NULL;
	session.pages.clear();
	session.resultPages.clear();
	session.privateCube.clear();
	session.saveCube.clear();
	session.saveRoots.clear();
}

void retryClosingSessions()
{
	SharedStash::retryRetiredSessions();
	for (size_t i = 0; i < sessions.size();) {
		LiveSession* session = sessions[i];
		if (session->closing && !session->owner && SharedStash::retireSession(session->store, session->token)) {
			delete session;
			sessions.erase(sessions.begin() + i);
		} else ++i;
	}
}

bool localDatabasePath(const char* path, char* canonical)
{
	char full[MAX_PATH];
	DWORD size = GetFullPathNameA(path, MAX_PATH, full, NULL);
	if (!size || size >= MAX_PATH) return false;
	char* slash = strrchr(full, '\\');
	if (!slash) return false;
	std::string filename(slash + 1);
	if (slash == full + 2 && full[1] == ':') slash[1] = 0;
	else *slash = 0;
	HANDLE directory = CreateFileA(full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (directory == INVALID_HANDLE_VALUE) return false;
	char resolved[MAX_PATH];
	size = GetFinalPathNameByHandleA(directory, resolved, MAX_PATH, FILE_NAME_NORMALIZED);
	CloseHandle(directory);
	if (!size || size >= MAX_PATH || !_strnicmp(resolved, "\\\\?\\UNC\\", 8)) return false;
	const char* normal = !_strnicmp(resolved, "\\\\?\\", 4) ? resolved + 4 : resolved;
	char volume[MAX_PATH];
	if (!GetVolumePathNameA(normal, volume, MAX_PATH)) return false;
	UINT type = GetDriveTypeA(volume);
	if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) return false;
	size_t length = strlen(normal);
	if (!formatPath(canonical, MAX_PATH, length && normal[length - 1] == '\\' ? "%s%s" : "%s\\%s", normal, filename.c_str())) return false;
	DWORD attributes = GetFileAttributesA(canonical);
	if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
	if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
	HANDLE file = CreateFileA(canonical, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (file == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION information;
	bool plainFile = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &information)
		&& information.nNumberOfLinks == 1 && !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
	CloseHandle(file);
	return plainFile;
}

std::string sqlitePath(const char* path)
{
	// PlugY paths use the Windows ANSI code page; SQLite narrow paths use UTF-8.
	wchar_t wide[MAX_PATH];
	if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH)) throw std::string("save path cannot be decoded");
	int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
	if (!count) throw std::string("save path cannot be encoded for SQLite");
	std::vector<char> utf8(count);
	if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, &utf8[0], count, NULL, NULL)) throw std::string("save path cannot be encoded for SQLite");
	return std::string(&utf8[0]);
}

bool markLegacyMigrated(const char* databasePath)
{
	size_t size = strlen(databasePath);
	if (size < 8 || _stricmp(databasePath + size - 8, ".sqlite3")) return false;
	std::string legacy(databasePath, size - 8); legacy += ".sss";
	static const BYTE marker[] = "PYDB\nThe shared stash is stored in SQLite.\n";
	BYTE* previous = NULL; DWORD previousSize = 0;
	int state = readSaveFileData(legacy.c_str(), &previous, &previousSize, 1);
	bool already = state > 0 && previousSize == sizeof(marker) - 1 && !memcmp(previous, marker, sizeof(marker) - 1);
	if (previous) D2FogMemDeAlloc(previous, __FILE__, __LINE__, 0);
	return already || writeFileAtomic(legacy.c_str(), marker, sizeof(marker) - 1);
}

LiveSession* sessionOf(const Unit* character)
{
	for (size_t i = 0; i < sessions.size(); ++i)
		if (!sessions[i]->closing && sessions[i]->character == character) return sessions[i];
	return NULL;
}

bool statusOK(Status s) { return s == OK || s == COMMITTED; }
const Item* itemOf(const Snapshot& snapshot, Id id)
{
	for (size_t i = 0; i < snapshot.items.size(); ++i) if (snapshot.items[i].id == id) return &snapshot.items[i];
	return NULL;
}
typedef std::map<Id, const Item*> ItemIndex;
ItemIndex indexItems(const Snapshot& snapshot)
{
	ItemIndex result;
	for (size_t i = 0; i < snapshot.items.size(); ++i) result[snapshot.items[i].id] = &snapshot.items[i];
	return result;
}
bool sameItem(const Item& a, const Item& b)
{
	return a.page == b.page && a.parent == b.parent && a.x == b.x && a.y == b.y
		&& a.width == b.width && a.height == b.height && a.data == b.data;
}
bool samePage(const Page& a, const Page& b)
{
	return a.ordinal == b.ordinal && a.flags == b.flags && a.name == b.name;
}
bool sameView(const Snapshot& a, const Snapshot& b)
{
	if (a.gold != b.gold || a.pages.size() != b.pages.size() || a.items.size() != b.items.size()) return false;
	for (size_t i = 0; i < a.pages.size(); ++i)
		if (a.pages[i].id != b.pages[i].id || !samePage(a.pages[i], b.pages[i])) return false;
	ItemIndex items = indexItems(b);
	for (size_t i = 0; i < a.items.size(); ++i)
		if (!items.count(a.items[i].id) || !sameItem(a.items[i], *items[a.items[i].id])) return false;
	return true;
}
bool isCube(Unit* item)
{
	ItemsBIN* definition = item ? D2GetItemsBIN(item->nTxtFileNo) : NULL;
	return definition && definition->ItemCode == 0x20786F62; // "box "
}
Unit* unitOf(LiveSession& session, Id id)
{
	std::map<Id, DWORD>::const_iterator i = session.units.find(id);
	return i == session.units.end() ? NULL : D2GameGetObject(session.game, UNIT_ITEM, i->second);
}
bool loadedDimensionsMatch(LiveSession& session, Unit* item, const Item& record)
{
	ItemsBIN* definition = item ? D2GetItemsBIN(item->nTxtFileNo) : NULL;
	if (definition && definition->InvWidth == record.width && definition->InvHeight == record.height) return true;
	log_msg("Shared stash item %I64d refused: database footprint %dx%d differs from this game's item data. All games sharing the database must use matching item definitions.\n",
		record.id, record.width, record.height);
	if (item) SharedNativeRemoveItem(session.character, item, false);
	session.failed = true;
	return false;
}
bool isLivePageItem(Unit* ptChar, Unit* item, BYTE page)
{
	return item && item->ptItemData && D2ItemGetPage(item) == page && item->ptItemData->ptInventory == PCInventory;
}

FineAction prepareFineAction(LiveSession& session, const BYTE* packet, DWORD size)
{
	FineAction action;
	if (!((packet[0] == 0x18 && size == 17) || (packet[0] == 0x19 && size == 5))) return action;
	Unit* ptChar = session.character;
	if (!PCPY->currentStash || !PCPY->currentStash->isShared) return action;
	memcpy(&action.gid, packet + 1, 4);
	Unit* item = D2GameGetObject(session.game, UNIT_ITEM, action.gid);
	if (!item || !item->ptItemData || !item->path || isCube(item)) return action;
	for (std::map<Id, Stash*>::const_iterator i = session.pages.begin(); i != session.pages.end(); ++i)
		if (i->second == PCPY->currentStash) { action.page = i->first; break; }
	if (!action.page) return action;
	action.ordinal = PCPY->currentStash->id;
	action.viewRevision = session.snapshot.revision;
	action.deposit = packet[0] == 0x18;
	if (action.deposit) {
		DWORD x, y, storage;
		memcpy(&x, packet + 5, 4); memcpy(&y, packet + 9, 4); memcpy(&storage, packet + 13, 4);
		if (storage != 4 || item != PCInventory->inventory1 || item->mode != 4 || x > 255 || y > 255) return FineAction();
		for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i)
			if (i->second == action.gid) return FineAction();
		ItemsBIN* definition = D2GetItemsBIN(item->nTxtFileNo);
		if (!definition || !definition->InvWidth || !definition->InvHeight) return FineAction();
		action.x = (int)x; action.y = (int)y;
		action.width = definition->InvWidth; action.height = definition->InvHeight;
	} else {
		if (item->mode != 0 || !isLivePageItem(ptChar, item, 4)) return FineAction();
		for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i)
			if (i->second == action.gid) { action.item = i->first; break; }
		const Item* source = itemOf(session.snapshot, action.item);
		if (!source || source->parent || source->page != action.page) return FineAction();
		action.version = source->version;
	}
	action.active = true;
	action.stamped = SharedTransportTakeRequest(ptChar, packet[0], action.requestRevision, action.counter);
	return action;
}

bool validFineAction(LiveSession& session, const FineAction& action)
{
	if (!action.stamped || action.requestRevision != action.viewRevision) return false;
	Unit* ptChar = session.character;
	std::map<Id, Stash*>::const_iterator page = session.pages.find(action.page);
	if (page == session.pages.end() || page->second != PCPY->currentStash || page->second->id != action.ordinal) return false;
	Unit* item = D2GameGetObject(session.game, UNIT_ITEM, action.gid);
	if (!item || !item->ptItemData || isCube(item)) return false;
	if (action.deposit) return item == PCInventory->inventory1 && item->mode == 4;
	const Item* source = itemOf(session.snapshot, action.item);
	return source && source->version == action.version && source->page == action.page && !source->parent
		&& session.units.count(action.item) && session.units[action.item] == action.gid
		&& item->mode == 0 && isLivePageItem(ptChar, item, 4);
}

bool tracked(const LiveSession& session, DWORD gid)
{
	for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i) {
		if (i->second == gid) return true;
		Unit* root = D2GameGetObject(session.game, UNIT_ITEM, i->second);
		Inventory* sockets = root ? *(Inventory**)((DWORD)root + shifting.ptInventory) : NULL;
		for (Unit* child = sockets ? D2InventoryGetFirstItem(sockets) : NULL; child; child = D2UnitGetNextItem(child))
			if (child->nItemNum == gid) return true;
	}
	return false;
}

struct NotificationView {
	std::map<DWORD, Id> identities;
	ItemIndex physical, authoritative;
};

NotificationView notificationView(LiveSession& session, const Snapshot& physical,
	const std::map<Id, DWORD>& units, const Snapshot& authoritative)
{
	NotificationView view;
	view.physical = indexItems(physical); view.authoritative = indexItems(authoritative);
	// Include removed shadow gids as well: their held updates must not be
	// mistaken for private/world traffic merely because the unit is now gone.
	for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i)
		view.identities[i->second] = i->first;
	for (std::map<Id, DWORD>::const_iterator i = units.begin(); i != units.end(); ++i) {
		view.identities[i->second] = i->first;
		Unit* root = D2GameGetObject(session.game, UNIT_ITEM, i->second);
		Inventory* sockets = root ? *(Inventory**)((DWORD)root + shifting.ptInventory) : NULL;
		for (Unit* child = sockets ? D2InventoryGetFirstItem(sockets) : NULL; child; child = D2UnitGetNextItem(child))
			view.identities[child->nItemNum] = i->first;
	}
	return view;
}

bool keepCurrentNotification(const BYTE* packet, DWORD size, void* context)
{
	DWORD gid = 0;
	if (!sharedStashItemPacket(packet, size, &gid)) return true;
	// Removal cleans up the old local gid. Reconciliation recreates an
	// authoritative replacement under a new gid when that row still exists.
	if (packet[0] == 0x0A) return true;
	const NotificationView& view = *(NotificationView*)context;
	std::map<DWORD, Id>::const_iterator identity = view.identities.find(gid);
	if (identity == view.identities.end()) return true;
	ItemIndex::const_iterator physical = view.physical.find(identity->second), authoritative = view.authoritative.find(identity->second);
	// A queued creation/quantity update may describe our already-committed row.
	// An unrelated foreign commit must not erase that still-needed client update.
	return physical != view.physical.end() && authoritative != view.authoritative.end()
		&& sameItem(*physical->second, *authoritative->second);
}

// Parked units remain owned by this session and in the game's unit table. No
// pointer is serialized; every park is undone before PlayerData is released.
void notifyRemoved(Unit* ptChar, DWORD gid)
{
	BYTE packet[6] = {0x0A, UNIT_ITEM, 0, 0, 0, 0};
	memcpy(packet + 2, &gid, sizeof(gid));
	D2SendPacket(D2GetClient(ptChar, __FILE__, __LINE__), packet, sizeof(packet));
}

bool parkCube(Unit* ptChar, std::vector<ParkedItem>& parked, bool notify = false, bool append = false)
{
	if (!append && !parked.empty()) return false;
	std::vector<Unit*> items;
	for (Unit* item = D2InventoryGetFirstItem(PCInventory); item; item = D2UnitGetNextItem(item))
		if (isLivePageItem(ptChar, item, 3)) items.push_back(item);
	parked.reserve(parked.size() + items.size());
	for (size_t i = 0; i < items.size(); ++i) {
		ParkedItem record = {items[i], (BYTE)items[i]->path->x, (BYTE)items[i]->path->y, items[i]->nItemNum};
		if (!D2InvRemoveItem(PCInventory, items[i])) return false;
		parked.push_back(record);
		if (notify && PCPY->clientReady) notifyRemoved(ptChar, record.item->nItemNum);
	}
	return true;
}
bool restoreCube(Unit* ptChar, std::vector<ParkedItem>& parked, bool notify = false)
{
	while (!parked.empty()) {
		ParkedItem item = parked.front();
		if (!SharedNativeAttachCubeItem(ptChar, item.item, item.x, item.y)) return false;
		parked.erase(parked.begin());
		// Ownership moved to the native inventory; a later packet failure must
		// not leave a second teardown owner in the parked vector.
		if (notify && PCPY->clientReady && !SharedNativeSendItem(ptChar, item.item, NULL)) return false;
	}
	return true;
}

bool parkSaveRoots(LiveSession& session)
{
	Unit* ptChar = session.character;
	if (!PCPY->currentStash || !PCPY->currentStash->isShared) return true;
	if (!session.saveRoots.empty()) return false;
	session.saveRootPage = PCPY->currentStash;
	std::vector<Unit*> items;
	for (Unit* item = D2InventoryGetFirstItem(PCInventory); item; item = D2UnitGetNextItem(item))
		if (isLivePageItem(ptChar, item, 4)) items.push_back(item);
	session.saveRoots.reserve(items.size());
	for (size_t i = 0; i < items.size(); ++i) {
		ParkedItem record = {items[i], (BYTE)items[i]->path->x, (BYTE)items[i]->path->y, items[i]->nItemNum};
		if (!D2InvRemoveItem(PCInventory, items[i])) return false;
		session.saveRoots.push_back(record);
	}
	return true;
}

bool restoreSaveRoots(LiveSession& session)
{
	Unit* ptChar = session.character;
	while (!session.saveRoots.empty()) {
		const ParkedItem& record = session.saveRoots.back();
		if (!session.saveRootPage || !attachItem(ptChar, record.item, session.saveRootPage, record.x, record.y)) return false;
		session.saveRoots.pop_back();
	}
	session.saveRootPage = NULL;
	return true;
}

bool preserveOrphanCubeContents(Unit* ptChar)
{
	// Legacy cube contents are character-owned page-3 units, even when the only
	// cube root was in the shared file. Do not silently give them to a global cube.
	if (isCube(PCInventory->inventory1)) return true;
	std::vector<Unit*> contents;
	for (Unit* item = D2InventoryGetFirstItem(PCInventory); item; item = D2UnitGetNextItem(item)) {
		if (!item->ptItemData || item->ptItemData->ptInventory != PCInventory) continue;
		BYTE page = D2ItemGetPage(item);
		if (isCube(item) && page != 3 && page != 4) return true;
		if (page == 3) contents.push_back(item);
	}
	if (contents.empty()) return true;
	for (Stash* page = PCPY->selfStash; page; page = page->nextStash) {
		Unit* first = page == PCPY->currentStash ? D2InventoryGetFirstItem(PCInventory) : page->ptListItem;
		for (Unit* item = first; item; item = D2UnitGetNextItem(item))
			if (D2ItemGetPage(item) == 4 && isCube(item)) return true;
	}
	Stash* target = PCPY->selfStash;
	while (target && target->nextStash) target = target->nextStash;
	for (size_t i = 0; i < contents.size(); ++i) {
		BYTE x = 0, y = 0;
		if (!target || !isMovedItemsPageName(target->name) || !stashFindSpot(ptChar, target, contents[i], &x, &y, false)) {
			target = addStash(ptChar, false, false, target);
			if (!target || !(target->name = _strdup(MOVED_ITEMS_PAGE_NAME))) return false;
			if (!stashFindSpot(ptChar, target, contents[i], &x, &y, false)) return false;
		}
		if (!D2InvRemoveItem(PCInventory, contents[i]) || !attachItem(ptChar, contents[i], target, x, y)) return false;
		++PCPY->selfItemsOnNewPages;
	}
	log_msg("Shared stash migration: %u character-owned cube contents without a personal cube were kept on personal Moved items pages.\n", (DWORD)contents.size());
	return true;
}

bool switchCube(LiveSession& session, DWORD gid)
{
	Unit* ptChar = session.character;
	if (session.cubeGid == gid) return true;
	Id target = 0;
	for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i)
		if (i->second == gid) { const Item* item = itemOf(session.snapshot, i->first); if (item && !item->parent) target = i->first; break; }
	if (session.cube) {
		for (size_t i = 0; i < session.snapshot.items.size(); ++i) {
			const Item& child = session.snapshot.items[i]; if (child.parent != session.cube) continue;
			Unit* item = unitOf(session, child.id);
			if (item && !SharedNativeRemoveItem(ptChar, item, PCPY->clientReady != 0)) return false;
			session.units.erase(child.id);
		}
		if (!restoreCube(ptChar, session.privateCube, true)) return false;
	}
	session.cube = 0; session.cubeGid = gid;
	if (!target) return true;
	if (!parkCube(ptChar, session.privateCube, true)) return false;
	session.cube = target;
	for (size_t i = 0; i < session.snapshot.items.size(); ++i) {
		const Item& child = session.snapshot.items[i]; if (child.parent != target) continue;
		Unit* item = SharedNativeLoadItem(ptChar, child.data, NULL, (BYTE)child.x, (BYTE)child.y, 3);
		if (!item) return false;
		if (!loadedDimensionsMatch(session, item, child)) return false;
		session.units[child.id] = item->nItemNum;
		if (PCPY->clientReady && !SharedNativeSendItem(ptChar, item, NULL)) return false;
	}
	return true;
}

DWORD actionCube(LiveSession& session, const BYTE* packet, DWORD size)
{
	DWORD gid = 0;
	if ((packet[0] == 0x18 || packet[0] == 0x19 || packet[0] == 0x20) && size >= 5) memcpy(&gid, packet + 1, 4);
	else if (packet[0] == 0x2A && size == 9) memcpy(&gid, packet + 5, 4);
	else if (packet[0] == 0x55 && size == 16 && packet[1] == PY_OP_MOVE) memcpy(&gid, packet + 4, 4);
	Unit* item = gid ? D2GameGetObject(session.game, UNIT_ITEM, gid) : NULL;
	return isCube(item) ? gid : 0;
}

bool privateCubeHasContents(LiveSession& session)
{
	if (!session.privateCube.empty()) return true;
	if (session.cube) return false;
	Unit* ptChar = session.character;
	for (Unit* item = D2InventoryGetFirstItem(PCInventory); item; item = D2UnitGetNextItem(item))
		if (isLivePageItem(ptChar, item, 3)) return true;
	return false;
}

std::vector<DWORD> movingPageCubes(Unit* ptChar, Stash* page)
{
	std::vector<DWORD> result;
	if (!page) return result;
	bool shown = page == PCPY->currentStash;
	Unit* first = shown ? D2InventoryGetFirstItem(PCInventory) : page->ptListItem;
	for (Unit* item = first; item; item = D2UnitGetNextItem(item)) {
		if (D2ItemGetPage(item) != 4 || (shown && item->ptItemData->ptInventory != PCInventory)) continue;
		if (isCube(item) && !isPinnedItem(ptChar, item, 0)) result.push_back(item->nItemNum);
	}
	return result;
}

bool pageSwapCube(LiveSession& session, const BYTE* packet, DWORD size, DWORD& gid)
{
	if (packet[0] != 0x3A || size != 3 || packet[1] != US_SWAP0_TOGGLE) return true;
	Unit* ptChar = session.character;
	Stash* current = PCPY->currentStash;
	DWORD page = PCPY->pageSwap | packet[2];
	Stash* target = getStash(ptChar, !current->isShared, page);
	if (!target && !canCreateStashPage(!current->isShared, page)) return true;
	Stash* shared = current->isShared ? current : target;
	Stash* personal = current->isShared ? target : current;
	std::vector<DWORD> sharedCubes = movingPageCubes(ptChar, shared);
	std::vector<DWORD> personalCubes = movingPageCubes(ptChar, personal);
	unsigned fullShared = 0;
	DWORD fullSharedGid = 0;
	for (size_t i = 0; i < sharedCubes.size(); ++i) {
		Id id = 0;
		for (std::map<Id, DWORD>::const_iterator unit = session.units.begin(); unit != session.units.end(); ++unit)
			if (unit->second == sharedCubes[i]) { id = unit->first; break; }
		if (!id) return false;
		for (size_t child = 0; child < session.snapshot.items.size(); ++child) {
			if (session.snapshot.items[child].parent != id) continue;
			++fullShared; fullSharedGid = sharedCubes[i]; break;
		}
	}
	CubePageTransfer transfer = cubePageTransfer((unsigned)sharedCubes.size(), fullShared,
		(unsigned)personalCubes.size(), privateCubeHasContents(session));
	if (transfer == CUBE_PAGE_NO_ROOM) {
		log_msg("Shared stash page swap refused: cube contents do not fit the character's single cube grid.\n");
		return false;
	}
	if (transfer == CUBE_PAGE_TO_PERSONAL) gid = fullSharedGid;
	else if (transfer == CUBE_PAGE_TO_SHARED) gid = personalCubes[0];
	return true;
}

bool cubeWithdrawalBlocked(LiveSession& session, DWORD gid, const BYTE* packet, DWORD size)
{
	if (!gid) return false;
	bool shared = false;
	for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i)
		if (i->second == gid) { const Item* item = itemOf(session.snapshot, i->first); shared = item && !item->parent; break; }
	if (!shared) return false;
	bool withdrawing = packet[0] == 0x19;
	if (packet[0] == 0x55 && size == 16 && packet[1] == PY_OP_MOVE) {
		Unit* ptChar = session.character;
		bool targetShared = (packet[3] & PY_FLAG_CURRENT) ? PCPY->currentStash->isShared != 0 : (packet[3] & PY_FLAG_SHARED) != 0;
		withdrawing = !targetShared;
	}
	if (!withdrawing) return false;
	return privateCubeHasContents(session);
}

bool capture(LiveSession& session, Snapshot& out, std::map<Id, DWORD>& units,
	std::map<Id, Stash*>& pages)
{
	Unit* ptChar = session.character;
	Snapshot result;
	std::vector<DWORD> gids;
	std::vector<Stash*> pointers;
	if (!buildSharedStashLiveSnapshot(ptChar, result, gids, pointers, session.cubeGid)) return false;
	if (result.pages.size() > 65535) return false;
	std::map<Id, Id> pageIds, itemIds;
	std::map<Stash*, Id> idsByPage;
	std::map<DWORD, Id> idsByGid;
	for (std::map<Id, Stash*>::const_iterator i = session.pages.begin(); i != session.pages.end(); ++i) idsByPage[i->second] = i->first;
	for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i) idsByGid[i->second] = i->first;
	ItemIndex beforeItems = indexItems(session.snapshot);
	Id nextPage = -1, nextItem = -1;
	for (size_t i = 0; i < result.pages.size(); ++i) {
		Id id = idsByPage.count(pointers[i]) ? idsByPage[pointers[i]] : 0;
		if (!id) id = nextPage--;
		pageIds[result.pages[i].id] = id; result.pages[i].id = id; pages[id] = pointers[i];
	}
	for (size_t i = 0; i < result.items.size(); ++i) {
		Id id = idsByGid.count(gids[i]) ? idsByGid[gids[i]] : 0;
		if (!id) id = nextItem--;
		itemIds[result.items[i].id] = id; units[id] = gids[i];
	}
	for (size_t i = 0; i < result.items.size(); ++i) {
		Item& item = result.items[i];
		item.id = itemIds[item.id]; item.page = pageIds[item.page];
		if (item.parent) item.parent = itemIds[item.parent];
		const Item* old = beforeItems.count(item.id) ? beforeItems[item.id] : NULL;
		item.version = old ? old->version : 1;
	}
	// Inactive cubes are database objects, not the player's one native cube grid.
	// Preserve their children unchanged; active cube contents are captured above.
	result.items.reserve(result.items.size() + session.snapshot.items.size());
	ItemIndex afterItems = indexItems(result);
	for (size_t i = 0; i < session.snapshot.items.size(); ++i) {
		const Item& child = session.snapshot.items[i];
		if (!child.parent || child.parent == session.cube) continue;
		const Item* root = afterItems.count(child.parent) ? afterItems[child.parent] : NULL;
		if (!root) continue;
		Item copy = child; copy.page = root->page; result.items.push_back(copy);
	}
	result.initialized = true; result.revision = session.snapshot.revision;
	out = result;
	return true;
}

bool makeMutation(LiveSession& session)
{
	Unit* ptChar = session.character;
	std::set<DWORD> candidates(session.actionGids.begin(), session.actionGids.end());
	for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i) candidates.insert(i->second);
	for (std::set<DWORD>::const_iterator i = candidates.begin(); i != candidates.end(); ++i) {
		Unit* item = D2GameGetObject(session.game, UNIT_ITEM, *i);
		if (!item || !item->ptItemData || item->mode != 0 || item->ptItemData->ptInventory != PCInventory) continue;
		BYTE page = D2ItemGetPage(item);
		if (page != 3 && page != 4) continue;
		bool linked = false;
		for (Unit* owned = D2InventoryGetFirstItem(PCInventory); owned; owned = D2UnitGetNextItem(owned))
			if (owned == item) { linked = true; break; }
		if (!linked) {
			// Native grid insertion may precede a failing inventory-chain insertion.
			// This is not an empty cell and must never release database ownership.
			log_msg("Shared stash finalization refused: item %lu is grid-owned but absent from its inventory chain.\n", *i);
			return false;
		}
	}
	Snapshot after;
	std::map<Id, DWORD> units;
	std::map<Id, Stash*> pages;
	if (!capture(session, after, units, pages)) return false;
	Mutation change;
	std::map<Id, const Page*> beforePages;
	for (size_t i = 0; i < session.snapshot.pages.size(); ++i) beforePages[session.snapshot.pages[i].id] = &session.snapshot.pages[i];
	ItemIndex beforeItems = indexItems(session.snapshot), afterItems = indexItems(after);
	for (size_t i = 0; i < after.pages.size(); ++i) {
		const Page& page = after.pages[i];
		const Page* before = beforePages.count(page.id) ? beforePages[page.id] : NULL;
		if (!before || !samePage(*before, page)) change.putPages.push_back(page);
	}
	for (size_t i = 0; i < session.snapshot.pages.size(); ++i)
		if (!pages.count(session.snapshot.pages[i].id)) change.erasePages.push_back(session.snapshot.pages[i].id);
	for (size_t i = 0; i < after.items.size(); ++i) {
		const Item& item = after.items[i]; const Item* before = beforeItems.count(item.id) ? beforeItems[item.id] : NULL;
		if (!before || !sameItem(*before, item)) change.put.push_back(item);
	}
	for (size_t i = 0; i < session.snapshot.items.size(); ++i)
		if (!afterItems.count(session.snapshot.items[i].id)) change.erase.push_back(session.snapshot.items[i].id);
	change.setGold = after.gold != session.snapshot.gold; change.gold = after.gold;
	if (session.fine.active) {
		// Plain pickup and exact-cell placement affect one non-container root.
		// Unrelated shadow updates must not be written under that root's locks.
		if (!change.putPages.empty() || !change.erasePages.empty() || change.setGold) return false;
		Id affected = session.fine.item;
		if (session.fine.deposit) {
			for (std::map<Id, DWORD>::const_iterator i = units.begin(); i != units.end(); ++i)
				if (i->second == session.fine.gid) { affected = i->first; break; }
		}
		Mutation scoped;
		for (size_t i = 0; i < change.put.size(); ++i) {
			if (change.put[i].id == affected) scoped.put.push_back(change.put[i]);
			else if (change.put[i].id < 0) return false; // unexpected new ownership needs a wider admission boundary
		}
		for (size_t i = 0; i < change.erase.size(); ++i) {
			if (change.erase[i] == affected) scoped.erase.push_back(change.erase[i]);
			else return false;
		}
		change = scoped;
	}
	session.mutation = change; session.result = after;
	session.resultUnits.swap(units); session.resultPages.swap(pages);
	return true;
}

bool changed(const Mutation& m)
{
	return !m.put.empty() || !m.erase.empty() || !m.putPages.empty() || !m.erasePages.empty() || m.setGold;
}

bool reconcile(LiveSession& session, const Snapshot& snapshot, bool initial = false, const std::set<Id>* publish = NULL);

bool finish(LiveSession& session)
{
	if (!session.pending || !session.applied) return !session.pending;
	if (session.captureFailed) {
		if (!session.recorded && (!session.character || !makeMutation(session))) return false;
		session.captureFailed = false;
	}
	std::vector<Id> created;
	Snapshot canonical;
	Status status = session.store.commit(session.operation, session.token, session.mutation, created, &canonical);
	if (!statusOK(status)) {
		log_msg("Shared stash finalization pending (%d): %s\n", status, session.store.error().c_str());
		return false;
	}
	session.recorded = true;
	std::map<Id, Id> pageIds, itemIds;
	size_t n = 0;
	for (size_t i = 0; i < session.mutation.putPages.size(); ++i) {
		Id alias = session.mutation.putPages[i].id;
		if (alias >= 0) continue;
		if (n >= created.size()) return false;
		pageIds[alias] = created[n++];
	}
	for (size_t i = 0; i < session.mutation.put.size(); ++i) {
		Id alias = session.mutation.put[i].id;
		if (alias >= 0) continue;
		if (n >= created.size()) return false;
		itemIds[alias] = created[n++];
	}
	std::map<Id, DWORD> nextUnits;
	std::map<Id, Stash*> nextPages;
	for (std::map<Id, DWORD>::const_iterator i = session.resultUnits.begin(); i != session.resultUnits.end(); ++i)
		nextUnits[i->first < 0 ? itemIds[i->first] : i->first] = i->second;
	for (std::map<Id, Stash*>::const_iterator i = session.resultPages.begin(); i != session.resultPages.end(); ++i)
		nextPages[i->first < 0 ? pageIds[i->first] : i->first] = i->second;
	// A deduplicated retry may return a newer database view. Keep an alias-resolved
	// description of the local native state so it can be reconciled with that view.
	Snapshot committed = session.result;
	committed.revision = -1;
	for (size_t i = 0; i < committed.pages.size(); ++i)
		if (committed.pages[i].id < 0) committed.pages[i].id = pageIds[committed.pages[i].id];
	for (size_t i = 0; i < committed.items.size(); ++i) {
		Item& item = committed.items[i];
		if (item.id < 0) item.id = itemIds[item.id];
		if (item.page < 0) item.page = pageIds[item.page];
		if (item.parent < 0) item.parent = itemIds[item.parent];
	}
	session.units.swap(nextUnits); session.pages.swap(nextPages);
	session.snapshot = committed;
	if (session.cube && !itemOf(committed, session.cube)) {
		// Taking the cube transfers its already-materialized page-3 contents to this
		// character. They no longer occur in the shared database.
		session.cube = 0; session.cubeGid = 0;
	}
	if (!session.cube && session.cubeGid)
		for (std::map<Id, DWORD>::const_iterator i = session.units.begin(); i != session.units.end(); ++i)
			if (i->second == session.cubeGid) {
				const Item* root = itemOf(committed, i->first);
				if (root && !root->parent) session.cube = root->id;
			}
	// A previously failed/ambiguous commit may have waited through native timer
	// updates. Describe the actual current units, not only the earlier capture,
	// before deciding which buffered packets still match database ownership.
	Snapshot physical = committed;
	if (session.character && !session.closing && !session.tearingDown) {
		std::map<Id, DWORD> physicalUnits; std::map<Id, Stash*> physicalPages;
		if (!capture(session, physical, physicalUnits, physicalPages)) return false;
		for (std::map<Id, DWORD>::const_iterator i = physicalUnits.begin(); i != physicalUnits.end(); ++i)
			if (i->first < 0) return false; // unexpected ownership created outside the admitted result
		session.units.swap(physicalUnits); session.pages.swap(physicalPages);
	}
	bool needsReconcile = !sameView(physical, canonical);
	physical.revision = -1;
	session.snapshot = needsReconcile ? physical : canonical;
	bool pageChanged = !session.mutation.putPages.empty() || !session.mutation.erasePages.empty();
	std::set<Id> ownRoots;
	for (std::map<Id, Id>::const_iterator i = itemIds.begin(); i != itemIds.end(); ++i) {
		Id id = i->second;
		const Item* item = itemOf(canonical, id);
		if (item && !item->parent) ownRoots.insert(id);
	}
	session.pending = false; session.applied = false; session.recorded = false; session.operation.clear(); session.fine = FineAction();
	if (session.character && !session.closing) {
		if (needsReconcile) {
			NotificationView view = notificationView(session, physical, session.units, canonical);
			if (!SharedNativeFilterNotifications(session.character, keepCurrentNotification, &view)) { session.failed = true; return false; }
		}
		if (needsReconcile && !reconcile(session, canonical, true, &ownRoots)) { session.failed = true; return false; }
		if (!needsReconcile) {
			// Raw bot requests do not perform the client's optimistic placement.
			// Publish a new stored root before clearing its old cursor view; the
			// native inventory pump may not have sent its creation packet yet.
			SyncGuard publishing(session.syncing);
			for (std::set<Id>::const_iterator i = ownRoots.begin(); i != ownRoots.end(); ++i) {
				Id id = *i;
				const Item* row = itemOf(canonical, id);
				Unit* item = unitOf(session, id);
				if (!item || !SharedNativeSendItem(session.character, item, session.pages[row->page])) { session.failed = true; return false; }
			}
		}
		if (session.repairCursor && !SharedNativeSendCursor(session.character)) session.failed = true;
		if (pageChanged) updateSelectedStashClient(session.character);
		SharedTransportSendRevision(session.character, session.snapshot.revision);
		if (!SharedNativeReleaseNotifications(session.character)) session.failed = true;
	}
	session.repairCursor = false;
	return true;
}

bool ensurePages(LiveSession& session, const Snapshot& snapshot)
{
	Unit* ptChar = session.character;
	std::vector<Stash*> order;
	for (size_t i = 0; i < snapshot.pages.size(); ++i) {
		const Page& record = snapshot.pages[i];
		Stash* page = session.pages.count(record.id) ? session.pages[record.id] : NULL;
		if (!page) {
			page = (Stash*)calloc(1, sizeof(Stash));
			if (!page) return false;
			session.pages[record.id] = page;
		}
		char* name = NULL;
		if (!record.name.empty()) { name = _strdup(record.name.c_str()); if (!name) return false; }
		free(page->name); page->name = name;
		page->id = (DWORD)record.ordinal; page->flags = record.flags | 1;
		order.push_back(page);
	}
	if (order.empty()) return false;
	std::set<Stash*> retained(order.begin(), order.end());
	for (std::map<Id, Stash*>::iterator i = session.pages.begin(); i != session.pages.end();) {
		if (!retained.count(i->second)) {
			if (i->second->ptListItem) return false;
			free(i->second->name); free(i->second);
			std::map<Id, Stash*>::iterator old = i++; session.pages.erase(old);
		} else ++i;
	}
	for (size_t i = 0; i < order.size(); ++i) {
		order[i]->previousStash = i ? order[i - 1] : NULL;
		order[i]->nextStash = i + 1 < order.size() ? order[i + 1] : NULL;
	}
	PCPY->sharedStash = order[0]; PCPY->nbSharedPages = (DWORD)order.size();
	return true;
}

bool reconcile(LiveSession& session, const Snapshot& snapshot, bool initial, const std::set<Id>* publish)
{
	Unit* ptChar = session.character;
	if (!initial && snapshot.revision == session.snapshot.revision) return true;
	SyncGuard syncing(session.syncing);
	Id shown = 0;
	Stash* original = PCPY->currentStash;
	bool originalPersonal = original && !original->isShared;
	for (std::map<Id, Stash*>::const_iterator i = session.pages.begin(); i != session.pages.end(); ++i)
		if (i->second == original) { shown = i->first; break; }
	if (!SharedNativeHoldNotifications(ptChar)) return false;
	// From this point a failure may leave a partially rebuilt shadow. Do not
	// admit actions or repeat reconstruction against that partial model.
	bool previousFailure = session.failed;
	session.failed = true;
	if (session.cube && !itemOf(snapshot, session.cube) && !SharedNativeCloseCube(ptChar)) return false;
	if (!PCPY->selfStash) addStash(ptChar, false, false);
	selectStash(ptChar, PCPY->selfStash);
	ItemIndex afterItems = indexItems(snapshot);
	// Remove children before their container and roots before changing page links.
	for (int children = 1; children >= 0; --children)
		for (size_t i = 0; i < session.snapshot.items.size(); ++i) {
			const Item& old = session.snapshot.items[i]; if ((old.parent != 0) != (children != 0)) continue;
			const Item* next = afterItems.count(old.id) ? afterItems[old.id] : NULL;
			if (next && sameItem(old, *next)) continue;
			if (!old.parent && old.id == session.cube && !SharedNativeCloseCube(ptChar)) return false;
			Unit* item = unitOf(session, old.id);
			if (item && !SharedNativeRemoveItem(ptChar, item, PCPY->clientReady != 0)) return false;
			session.units.erase(old.id);
		}
	if (!ensurePages(session, snapshot)) return false;
	if (PCPY->clientReady && !SharedTransportSendLayout(ptChar, (DWORD)snapshot.pages.size())) return false;
	std::map<Id, std::vector<const Item*> > pageItems;
	for (size_t i = 0; i < snapshot.items.size(); ++i)
		if (!snapshot.items[i].parent) pageItems[snapshot.items[i].page].push_back(&snapshot.items[i]);
	for (size_t p = 0; p < snapshot.pages.size(); ++p) {
		Stash* page = session.pages[snapshot.pages[p].id];
		selectStash(ptChar, page);
		const std::vector<const Item*>& contents = pageItems[snapshot.pages[p].id];
		for (size_t i = 0; i < contents.size(); ++i) {
			const Item& record = *contents[i];
			if (session.units.count(record.id)) {
				if (PCPY->clientReady) {
					if (publish && publish->count(record.id)) {
						// A concurrent commit can require a refresh before the native
						// inventory pump has announced our just-placed root to the client.
						Unit* item = unitOf(session, record.id);
						if (!item || !SharedNativeSendItem(ptChar, item, page)) return false;
					} else sendItemMoved(ptChar, session.units[record.id], NULL, false, page,
						(BYTE)record.x, (BYTE)record.y, PY_MOVED_REPAIR, 0);
				}
				continue;
			}
			Unit* item = SharedNativeLoadItem(ptChar, record.data, page, (BYTE)record.x, (BYTE)record.y);
			if (!item) return false;
			if (!loadedDimensionsMatch(session, item, record)) return false;
			session.units[record.id] = item->nItemNum;
			if (record.id == session.cube) session.cubeGid = item->nItemNum;
			if (PCPY->clientReady && !SharedNativeSendItem(ptChar, item, page)) return false;
		}
	}
	if (session.cube && !itemOf(snapshot, session.cube)) {
		session.cube = 0; session.cubeGid = 0;
		if (!restoreCube(ptChar, session.privateCube, true)) return false;
	}
	if (session.cube)
		for (size_t i = 0; i < snapshot.items.size(); ++i) {
			const Item& child = snapshot.items[i];
			if (child.parent != session.cube || session.units.count(child.id)) continue;
			Unit* item = SharedNativeLoadItem(ptChar, child.data, NULL, (BYTE)child.x, (BYTE)child.y, 3);
			if (!item) return false;
			if (!loadedDimensionsMatch(session, item, child)) return false;
			session.units[child.id] = item->nItemNum;
			if (PCPY->clientReady) SharedNativeSendItem(ptChar, item, NULL);
		}
	Stash* home = shown && session.pages.count(shown) ? session.pages[shown] :
		(originalPersonal ? original : PCPY->sharedStash);
	selectStash(ptChar, home, true);
	PCPY->sharedGold = (DWORD)snapshot.gold;
	updateClient(ptChar, UC_SHARED_GOLD, PCPY->sharedGold, 0, 0);
	session.snapshot = snapshot;
	if (PCPY->clientReady && !SharedNativeSendCursor(ptChar, false)) return false;
	if (PCPY->clientReady) SharedTransportSendRevision(ptChar, snapshot.revision);
	if (!SharedNativeReleaseNotifications(ptChar)) return false;
	session.failed = previousFailure;
	return true;
}

bool recoverOwners(LiveSession& session)
{
	std::vector<std::string> pending;
	if (!statusOK(session.store.pendingSessions(pending))) return false;
	for (size_t i = 0; i < pending.size(); ++i)
		if (pending[i] != session.token) session.store.recoverDeadSession(pending[i]);
	return true;
}

bool sync(LiveSession& session)
{
	if (session.pending || session.failed || session.saving) return false;
	Id revision = -1;
	if (!statusOK(session.store.revision(revision))) return false;
	if (revision == session.snapshot.revision) return true;
	Snapshot snapshot;
	if (!statusOK(session.store.snapshot(snapshot))) return false;
	return reconcile(session, snapshot);
}

bool flushPassive(LiveSession& session, bool inspectLocal = false)
{
	if (session.pending) return finish(session);
	if (session.failed || session.saving || session.syncing) return false;
	SyncGuard syncing(session.syncing);
	Id revision = -1;
	if (!statusOK(session.store.revision(revision))) return false;
	if (revision != session.snapshot.revision) {
		Snapshot snapshot;
		if (!statusOK(session.store.snapshot(snapshot))) return false;
		// The native callback may still have been using its item pointer. Only
		// this safe server-thread boundary removes the stale shadow units.
		Snapshot physical;
		std::map<Id, DWORD> units; std::map<Id, Stash*> pages;
		if (!capture(session, physical, units, pages)) return false;
		if (session.passive) {
			NotificationView view = notificationView(session, physical, units, snapshot);
			if (!SharedNativeFilterNotifications(session.character, keepCurrentNotification, &view)) return false;
		}
		// Compare the database with real native units, not the previous database
		// image, so discarded timer changes are actually undone in the shadow.
		physical.revision = -1;
		session.snapshot = physical; session.units.swap(units); session.pages.swap(pages);
		session.passive = false;
		return reconcile(session, snapshot);
	}
	if (!inspectLocal && !session.passive) return true;
	if (!makeMutation(session)) return false;
	if (!changed(session.mutation)) {
		bool held = session.passive; session.passive = false;
		if (held && !SharedNativeSendCursor(session.character, false)) return false;
		return !held || SharedNativeReleaseNotifications(session.character);
	}
	char suffix[50]; sprintf(suffix, ":passive:%lu", (DWORD)InterlockedIncrement(&sessionCounter));
	Reservation request; request.operation = session.token + suffix; request.session = session.token;
	request.layout = request.gold = true; request.expectedRevision = revision;
	if (session.store.reserve(request) != OK) { recoverOwners(session); return false; }
	if (!SharedNativeHoldNotifications(session.character)) { session.store.cancel(request.operation, request.session); return false; }
	session.operation = request.operation; session.pending = true; session.applied = true; session.recorded = false; session.captureFailed = false;
	session.passive = false;
	return finish(session);
}

bool beforeSend(void* client, const BYTE* packet, DWORD size)
{
	// Do not inspect or replace native units here: callers may still hold a
	// pointer into a half-finished native effect. Keep packets until the next
	// safe server boundary captures and commits the complete resulting state.
	for (size_t i = 0; i < sessions.size(); ++i) {
		LiveSession& session = *sessions[i];
		if (session.client != client || session.closing || session.tearingDown || session.syncing || session.saving) continue;
		Unit* ptChar = session.character;
		if (!PCPY->clientReady) return true;
		DWORD gid = 0;
		if (!sharedStashItemPacket(packet, size, &gid) || !tracked(session, gid)) return true;
		if (session.failed) return false;
		if (!SharedNativeHoldNotifications(ptChar)) { session.failed = true; return false; }
		session.passive = true;
		return true;
	}
	return true;
}

bool readOnly(const BYTE* packet, DWORD size)
{
	if (packet[0] == 0x55 && size == 16) return packet[1] == PY_OP_LIST;
	if (packet[0] != 0x3A || size < 3) return false;
	return packet[1] == US_PY_HELLO || packet[1] == US_PY_CLIENT_HELLO || packet[1] == US_PY_CLIENT_GRID
		|| packet[1] == US_SAVE;
}

CommandStream* commandStream(LiveSession& session, const BYTE* packet, DWORD size, bool& prefix)
{
	prefix = false;
	if (packet[0] != 0x3A || size < 3) return NULL;
	if (packet[1] == US_PAGENAME) { prefix = packet[2] != 0; return &session.nameCommand; }
	if (packet[1] == US_SWAP3 || packet[1] == US_SWAP2 || packet[1] == US_SWAP1
		|| packet[1] == US_SWAP0 || packet[1] == US_SWAP0_TOGGLE) {
		prefix = packet[1] == US_SWAP3 || packet[1] == US_SWAP2 || packet[1] == US_SWAP1;
		return &session.swapCommand;
	}
	return NULL;
}

void rejected(Unit* ptChar, const BYTE* packet, DWORD size, BYTE status)
{
	LiveSession* session = sessionOf(ptChar);
	bool prefix = false;
	CommandStream* command = session ? commandStream(*session, packet, size, prefix) : NULL;
	if (command) {
		command->active = false; command->discard = prefix;
		if (packet[1] == US_PAGENAME) PCPY->pageNameLength = 0;
		else PCPY->pageSwap = 0;
	}
	if (packet[0] == 0x55 && size == 16) sendStashAck(ptChar, packet[1], packet[2], status,
		*(const DWORD*)(packet + 4), 0, 0, PCPY->currentStash);
	// A native client may already have removed its cursor picture for a put.
	// Restore the server-owned cursor even when the request never entered native code.
	SharedNativeSendCursor(ptChar);
}
}

bool Install_SharedStashSession()
{
	if (installed) return true;
	if (version_D2Game != V114d || !active_multiPageStash || !active_SharedStashInMultiPlayer) return false;
	installed = Install_SharedStashTransport() && Install_SharedStashNative(SharedStashBeforeAction, SharedStashAfterAction);
	if (installed) SharedNativeSetBeforeSend(beforeSend);
	return installed;
}

bool SharedStashActive(const Unit* ptChar) { return sessionOf(ptChar) != NULL; }

bool SharedStashLoad(Unit* ptChar, bool isHardcore)
{
	if (!installed || !SharedNativeReady() || !SharedTransportReady() || !ptChar || !PCPlayerData || !PCGame
		|| !hasSessionLockOf(PCPlayerData) || customDlls) return false;
	retryClosingSessions();
	LiveSession* session = NULL;
	HANDLE migration = NULL;
	try {
		char path[MAX_PATH], full[MAX_PATH];
		if (!getSharedStashDatabasePath(path, isHardcore) || !localDatabasePath(path, full)) return false;
		_strlwr(full);
		char mutex[80]; sprintf(mutex, "Local\\PlugY_StashImport_%08X", fnv1a((BYTE*)full, strlen(full)));
		migration = CreateMutexA(NULL, FALSE, mutex);
		if (!migration) return false;
		DWORD wait = WaitForSingleObject(migration, 20000);
		if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(migration); return false; }
		session = new LiveSession(); session->character = ptChar; session->owner = PCPlayerData; session->game = PCGame;
		session->client = D2GetClient(ptChar, __FILE__, __LINE__);
		char token[100]; sprintf(token, "%lu-%lu-%lu", GetCurrentProcessId(), GetTickCount(), (DWORD)InterlockedIncrement(&sessionCounter));
		session->token = token;
		Snapshot imported; std::vector<DWORD> importUnits; std::vector<Stash*> importPages;
		char seedPath[MAX_PATH];
		if (!formatPath(seedPath, MAX_PATH, "%s.pre-sqlite.sss", path)) throw std::string("shared import backup path is too long");
		bool loadedSeed = false;
		bool fresh = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES &&
			(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
		if (fresh) {
			DWORD size = 0;
			BYTE* data = readSharedSaveFile(PCPlayerData->name, &size);
			if (!data) throw std::string("legacy shared stash could not be read");
			DWORD consumed = 0;
			int loaded = loadSharedSaveFile(ptChar, data, size, &consumed);
			if (loaded || consumed != size || !buildSharedStashLiveSnapshot(ptChar, imported, importUnits, importPages)) {
				D2FogMemDeAlloc(data, __FILE__, __LINE__, 0);
				throw std::string("legacy shared stash is damaged or has unsupported extension data");
			}
			bool backup = true;
			// Validate first: a refused extension must not leave an immutable bad
			// seed that prevents a later corrected file from being imported.
			// A valid seed still reaches disk before any database is created.
			{
				if (GetFileAttributesA(seedPath) != INVALID_FILE_ATTRIBUTES) {
					BYTE* old = NULL; DWORD oldSize = 0;
					backup = readSaveFileData(seedPath, &old, &oldSize, 1) > 0 && oldSize == size && !memcmp(old, data, size);
					if (old) D2FogMemDeAlloc(old, __FILE__, __LINE__, 0);
				} else backup = writeFileAtomic(seedPath, data, size);
			}
			D2FogMemDeAlloc(data, __FILE__, __LINE__, 0);
			if (!backup) throw std::string("legacy shared stash original-byte backup failed");
			loadedSeed = true;
		}
		InvGridInfo grid; D2GetInventoryGrid(0xC, 0, &grid);
		std::string databaseUtf8 = sqlitePath(full);
		if (!statusOK(session->store.open(databaseUtf8.c_str(), grid.gridX, grid.gridY))) throw session->store.error();
		Snapshot snapshot;
		if (!statusOK(session->store.snapshot(snapshot))) throw session->store.error();
		if (!snapshot.initialized) {
			std::vector<std::string> pending;
			if (!statusOK(session->store.pendingSessions(pending)) || !pending.empty()) throw std::string("unfinished import has pending game operations");
			if (!loadedSeed) {
				BYTE* data = NULL; DWORD size = 0;
				if (readSaveFileData(seedPath, &data, &size, 10) <= 0) throw std::string("unfinished import has no readable protected seed backup");
				DWORD consumed = 0;
				int loaded = loadSharedSaveFile(ptChar, data, size, &consumed);
				D2FogMemDeAlloc(data, __FILE__, __LINE__, 0);
				if (loaded || consumed != size || !buildSharedStashLiveSnapshot(ptChar, imported, importUnits, importPages))
					throw std::string("protected import seed is damaged or unsupported");
				loadedSeed = true;
			}
			if (!statusOK(session->store.importOnce(imported)) || !statusOK(session->store.snapshot(snapshot))) throw session->store.error();
		}
		if (!statusOK(session->store.registerSession(session->token))) throw session->store.error();
		if (snapshot.pages.size() > 65535) throw std::string("shared stash has more pages than the client layout protocol can represent");
		PCPY->sharedStashIsOpened = true;
		if (loadedSeed) {
			for (size_t i = 0; i < imported.pages.size(); ++i) session->pages[imported.pages[i].id] = importPages[i];
			for (size_t i = 0; i < imported.items.size(); ++i) session->units[imported.items[i].id] = importUnits[i];
			// The import mutex is local to a Windows session. SQLite may already
			// contain a concurrent import/change from another session, so retain
			// the actual loaded image until native reconciliation has completed.
			session->snapshot = imported;
			session->snapshot.revision = -1;
		}
		if (!reconcile(*session, snapshot, true)) throw std::string("shared item view could not be built");
		if (!preserveOrphanCubeContents(ptChar)) throw std::string("character-owned legacy cube contents could not be preserved");
		recoverOwners(*session);
		char backupPath[MAX_PATH];
		if (!formatPath(backupPath, MAX_PATH, "%s.backup.sqlite3", path) || !statusOK(session->store.backup(sqlitePath(backupPath).c_str())))
			throw std::string("shared database backup failed");
		if (!markLegacyMigrated(path)) throw std::string("legacy shared stash migration marker could not be secured");
		sessions.push_back(session);
		ReleaseMutex(migration); CloseHandle(migration);
		log_msg("Shared stash database loaded: %s, revision %I64d\n", path, snapshot.revision);
		return true;
	} catch (const std::string& error) {
		log_msg("PlugY could not load the shared stash database: %s. No legacy fallback is allowed.\n", error.c_str());
	} catch (...) { log_msg("Shared stash load refused after an unexpected allocation or native error.\n"); }
	if (session) { SharedNativeDiscardNotifications(ptChar); session->store.endSession(session->token); delete session; }
	if (migration) { ReleaseMutex(migration); CloseHandle(migration); }
	return false;
}

void SharedStashClientReady(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session) return;
	try {
		SyncGuard syncing(session->syncing);
		if (!SharedNativeHoldNotifications(ptChar)) { session->failed = true; return; }
		Stash* home = PCPY->currentStash;
		selectStash(ptChar, PCPY->selfStash, true);
		if (!SharedTransportSendLayout(ptChar, (DWORD)session->snapshot.pages.size())) { session->failed = true; return; }
		std::map<Id, std::vector<const Item*> > pageItems;
		for (size_t i = 0; i < session->snapshot.items.size(); ++i)
			if (!session->snapshot.items[i].parent) pageItems[session->snapshot.items[i].page].push_back(&session->snapshot.items[i]);
		for (size_t p = 0; p < session->snapshot.pages.size(); ++p) {
			Id id = session->snapshot.pages[p].id;
			Stash* page = session->pages[id];
			selectStash(ptChar, page, true);
			const std::vector<const Item*>& items = pageItems[id];
			for (size_t i = 0; i < items.size(); ++i) {
				Unit* item = unitOf(*session, items[i]->id);
				if (!item || !SharedNativeSendItem(ptChar, item, page)) { session->failed = true; return; }
			}
		}
		selectStash(ptChar, home, true);
		SharedTransportSendRevision(ptChar, session->snapshot.revision);
		if (!SharedNativeReleaseNotifications(ptChar)) session->failed = true;
	} catch (...) { session->failed = true; }
}

bool SharedStashBeforeAction(Game* game, Unit* ptChar, const BYTE* packet, DWORD size)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session) return true;
	if (!packet || !size || session->game != game) return false;
	// These commands only change this character. In particular rename moves the
	// files on the client before its name stream reaches the server.
	if (packet[0] == 0x3A && size == 3 && (packet[1] == US_RENAME
		|| (packet[1] >= US_UNASSIGN_STR_POINT && packet[1] <= US_UNASSIGN_SKILLS))) return true;
	if (!hasSessionLockOf(session->owner)) session->failed = true;
	if (session->failed || session->saving || session->tearingDown) { rejected(ptChar, packet, size, session->failed ? PY_STATUS_STORE_ERROR : PY_STATUS_BUSY); return false; }
	try {
		if (session->pending) { rejected(ptChar, packet, size, PY_STATUS_BUSY); return false; }
		// Save the intent before refreshing a stale shadow. Unrelated commits
		// may then proceed without turning an exact-cell request into a page
		// lookup against a different layout or a different source item.
		FineAction fine = prepareFineAction(*session, packet, size);
		if (!flushPassive(*session, true)) { rejected(ptChar, packet, size, PY_STATUS_BUSY); return false; }
		bool prefix = false;
		CommandStream* command = commandStream(*session, packet, size, prefix);
		if (command && prefix) {
			Id revision = -1; unsigned long long counter = 0;
			bool stamped = SharedTransportTakeRequest(ptChar, packet[0], revision, counter);
			if (packet[1] == US_SWAP3) *command = CommandStream(); // a new complete swap sequence
			if (!stamped || command->discard || revision != session->snapshot.revision
				|| (command->active && command->revision != revision)) {
				rejected(ptChar, packet, size, PY_STATUS_STALE); return false;
			}
			if (!command->active) { command->active = true; command->revision = revision; }
			return true; // only the final byte changes shared state
		}
		if (command && command->discard) {
			Id ignored = -1; unsigned long long counter = 0;
			SharedTransportTakeRequest(ptChar, packet[0], ignored, counter);
			rejected(ptChar, packet, size, PY_STATUS_STALE); return false;
		}
		if (readOnly(packet, size)) {
			if (sync(*session)) return true;
			rejected(ptChar, packet, size, PY_STATUS_STORE_ERROR); return false;
		}
		Id revision = -1; unsigned long long counter = 0;
		bool stamped;
		if (fine.active) { revision = fine.requestRevision; counter = fine.counter; stamped = fine.stamped; }
		else stamped = SharedTransportTakeRequest(ptChar, packet[0], revision, counter);
		Id currentRevision = -1;
		if (!statusOK(session->store.revision(currentRevision))) { rejected(ptChar, packet, size, PY_STATUS_STORE_ERROR); return false; }
		bool valid = fine.active ? validFineAction(*session, fine) :
			stamped && revision == currentRevision && revision == session->snapshot.revision
			&& !(command && command->active && command->revision != revision);
		if (!valid) {
			PCPY->pageNameLength = 0; PCPY->pageSwap = 0;
			Snapshot snapshot;
			if (!statusOK(session->store.snapshot(snapshot))) { rejected(ptChar, packet, size, PY_STATUS_STORE_ERROR); return false; }
			if (!reconcile(*session, snapshot)) session->failed = true;
			SharedTransportSendRevision(ptChar, snapshot.revision);
			rejected(ptChar, packet, size, PY_STATUS_STALE);
			return false;
		}
		if (command) *command = CommandStream();
		char suffix[50]; sprintf(suffix, ":%I64u", counter);
		Reservation request; request.operation = session->token + suffix; request.session = session->token;
		if (fine.active) {
			if (fine.deposit) request.targets.push_back(Rect(fine.page, fine.x, fine.y, fine.width, fine.height));
			else request.items.push_back(ItemVersion(fine.item, fine.version));
		} else { request.layout = request.gold = true; request.expectedRevision = revision; }
		Status reserved = session->store.reserve(request);
		if (reserved != OK) {
			recoverOwners(*session);
			rejected(ptChar, packet, size, reserved == STORE_ERROR ? PY_STATUS_STORE_ERROR : reserved == STALE ? PY_STATUS_STALE :
				reserved == NO_ROOM ? PY_STATUS_NO_ROOM : reserved == INVALID ? PY_STATUS_BAD_REQUEST : PY_STATUS_BUSY);
			return false;
		}
		if (!SharedNativeHoldNotifications(ptChar)) {
			session->store.cancel(request.operation, request.session); rejected(ptChar, packet, size, PY_STATUS_STORE_ERROR); return false;
		}
		session->operation = request.operation; session->pending = true; session->applied = false; session->recorded = false;
		session->fine = fine;
		session->repairCursor = packet[0] >= 0x16 && packet[0] <= 0x38;
		session->actionGids.clear();
		for (DWORD offset = 1; offset + 4 <= size; offset += 4) {
			DWORD gid; memcpy(&gid, packet + offset, 4); session->actionGids.push_back(gid);
		}
		if (packet[0] == 0x55 && size == 16) { DWORD gid; memcpy(&gid, packet + 4, 4); session->actionGids.push_back(gid); }
		DWORD cube = actionCube(*session, packet, size);
		if (!pageSwapCube(*session, packet, size, cube) || cubeWithdrawalBlocked(*session, cube, packet, size)) {
			session->store.cancel(request.operation, request.session); session->pending = false;
			SharedNativeReleaseNotifications(ptChar);
			rejected(ptChar, packet, size, PY_STATUS_NO_ROOM);
			return false;
		}
		if (cube && packet[0] == 0x3A && !SharedNativeCloseCube(ptChar)) {
			session->failed = true;
			return false;
		}
		if (cube && !switchCube(*session, cube)) {
			session->failed = true;
			return false;
		}
		return true;
	} catch (...) { session->failed = true; return false; }
}

void SharedStashAfterAction(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session || !session->pending || session->applied) return;
	try {
		session->applied = true;
		session->captureFailed = !makeMutation(*session);
		// The verified single-player handlers unlink logical ownership and create
		// cube outputs before returning. Their later queue clears flags/frees
		// detached units; it does not justify keeping a lease across a UI pause.
		if (!session->captureFailed) finish(*session);
	} catch (...) { session->captureFailed = true; }
}

void SharedStashTick(Game* game)
{
	retryClosingSessions();
	for (size_t i = 0; i < sessions.size(); ++i) {
		LiveSession* session = sessions[i];
		try {
			if (session->game == game && !session->tearingDown) {
				if (session->saving) SharedStashEndSave(session->character);
				if (session->pending) finish(*session);
				DWORD now = GetTickCount();
				if (!session->pending && (session->passive || now - session->poll >= 250)) {
					session->poll = now;
					if (!flushPassive(*session)) recoverOwners(*session);
				}
#ifdef PLUGY_TEST_HOOKS
				// Requests created by this test pump complete at the following frame,
				// just like requests arriving after this frame's safe boundary.
				SharedStashTestServerFrame(session->character);
#endif
			}
		} catch (...) { session->failed = true; }
	}
}

bool SharedStashCanSave(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session) return true;
	try {
		if (session->tearingDown) return false;
		if (!hasSessionLockOf(session->owner)) { session->failed = true; return false; }
		if (session->pending && !finish(*session)) return false;
		return !session->failed && (session->saving || (flushPassive(*session, true) && sync(*session)));
	} catch (...) { session->failed = true; return false; }
}

bool SharedStashBeginSave(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session || session->saving) return true;
	if (!SharedStashCanSave(ptChar)) return false;
	try {
		// Keep every parked unit in a session-owned vector even if a native
		// inventory call fails partway through the temporary save view.
		session->privateCube.reserve(session->privateCube.size() + 12);
		session->saveCube.reserve(12);
		session->saving = true;
		if (session->cube) {
			if (!parkCube(ptChar, session->saveCube)) { SharedStashEndSave(ptChar); session->failed = true; return false; }
			session->privateCubeExposed = true;
			if (!restoreCube(ptChar, session->privateCube)) { SharedStashEndSave(ptChar); session->failed = true; return false; }
		}
		// Vanilla serializes its visible stash grid. Shared roots belong only in
		// SQLite, so hide that grid too, without changing the client's page or gids.
		if (!parkSaveRoots(*session)) { SharedStashEndSave(ptChar); session->failed = true; return false; }
		return true;
	} catch (...) { SharedStashEndSave(ptChar); session->failed = true; return false; }
}

void SharedStashEndSave(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session || !session->saving) return;
	try {
		if (!restoreSaveRoots(*session)) { session->failed = true; return; }
		if (session->privateCubeExposed) {
			if (!parkCube(ptChar, session->privateCube, false, true)) { session->failed = true; return; }
			session->privateCubeExposed = false;
		}
		if (!restoreCube(ptChar, session->saveCube)) { session->failed = true; return; }
		session->saving = false;
	} catch (...) { session->failed = true; }
}

void SharedStashBeginClose(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session || session->tearingDown) return;
	session->tearingDown = true;
	// Native item removal is destruction, not a shared ownership mutation. Let
	// subsequent cleanup and game-end packets reach the client without opening
	// a new passive transaction against units that are being freed.
	try { SharedNativeDiscardNotifications(ptChar); session->passive = false; } catch (...) {}
}

void SharedStashClose(PlayerData* owner)
{
	for (size_t i = 0; i < sessions.size(); ++i) {
		LiveSession* session = sessions[i];
		if (session->owner != owner || session->closing) continue;
		try {
			// Item teardown has already run. No native unit pointer may be read here.
			SharedNativeDiscardNotifications(session->character);
			session->character = NULL;
			if (session->pending && !session->captureFailed) finish(*session);
		} catch (...) { session->failed = true; }
		session->closing = true; session->character = NULL;
	}
}

void SharedStashAfterClose(PlayerData* owner)
{
	for (size_t i = 0; i < sessions.size(); ++i)
		if (sessions[i]->closing && sessions[i]->owner == owner) {
			LiveSession* session = sessions[i];
			// The owning native state has been destroyed and can no longer execute or
			// save. An interrupted uncommitted action can now be abandoned safely.
			clearNativeState(*session);
			if (SharedStash::retireSession(session->store, session->token)) {
				delete session;
				sessions.erase(sessions.begin() + i);
			}
			return;
		}
}

Unit* SharedStashNextItemToFree(Unit* ptChar)
{
	LiveSession* session = sessionOf(ptChar);
	if (!session) return NULL;
	for (;;) {
		std::vector<ParkedItem>* list = !session->privateCube.empty() ? &session->privateCube :
			!session->saveCube.empty() ? &session->saveCube : &session->saveRoots;
		if (list->empty()) return NULL;
		ParkedItem record = list->back(); list->pop_back();
		Unit* item = D2GameGetObject(session->game, UNIT_ITEM, record.gid);
		if (item != record.item || !item || !item->ptItemData || item->ptItemData->ptInventory) continue;
		item->ptItemData->ptNextItem = NULL; item->ptItemData->ptPrevItem = NULL;
		return item;
	}
}
