#include "sharedStashSnapshot.h"
#include "infinityStash.h"
#include "customLibraries.h"

namespace {
bool saveItem(Unit* item, SharedStash::Bytes& bytes)
{
	const DWORD transient = ITEMFLAG_NEWITEM | ITEMFLAG_NEWITEM2;
	struct SavedFlags { Unit* item; DWORD bits; };
	std::vector<SavedFlags> flagged;
	SavedFlags root = {item, item->ptItemData->flags2 & transient};
	flagged.push_back(root);
	Inventory* sockets = *(Inventory**)((DWORD)item + shifting.ptInventory);
	for (Unit* child = sockets ? D2InventoryGetFirstItem(sockets) : NULL; child; child = D2UnitGetNextItem(child)) {
		SavedFlags saved = {child, child->ptItemData->flags2 & transient};
		flagged.push_back(saved);
	}
	for (size_t i = 0; i < flagged.size(); ++i) flagged[i].item->ptItemData->flags2 &= ~transient;
	DWORD buffer[0x2000 / sizeof(DWORD)];
	DWORD size = D2SaveItem(item, (saveBitField*)buffer, sizeof(buffer), 1, 1, 0);
	for (size_t i = 0; i < flagged.size(); ++i)
		flagged[i].item->ptItemData->flags2 = (flagged[i].item->ptItemData->flags2 & ~transient) | flagged[i].bits;
	if (!size || size > sizeof(buffer)) return false;
	const unsigned char* data = (const unsigned char*)buffer;
	bytes.assign(data, data + size);
	return true;
}
}

bool buildSharedStashLiveSnapshot(Unit* ptChar, SharedStash::Snapshot& out,
	std::vector<DWORD>& itemGids, std::vector<Stash*>& sourcePages, DWORD activeCubeGid)
{
	if (version_D2Game != V114d || !active_PlayerCustomData || !ptChar || !PCPlayerData
		|| !PCPY->sharedStashIsOpened || !PCPY->currentStash || !PCPY->sharedStash || customDlls)
		return false;

	SharedStash::Snapshot snapshot;
	std::vector<DWORD> gids;
	std::vector<Stash*> pagePointers;
	InvGridInfo grid;
	D2GetInventoryGrid(0xC, 0, &grid);
	if (!grid.gridX || !grid.gridY || grid.gridX > 15 || grid.gridY > 15)
		return false;
	snapshot.initialized = true;
	snapshot.gold = PCPY->sharedGold;
	SharedStash::Id nextItem = 1;
	int ordinal = 0;
	for (Stash* page = PCPY->sharedStash; page; page = page->nextStash)
	{
		if (ordinal >= 65535 || !page->isShared || page->id != (DWORD)ordinal)
			return false;
		bool used[225] = {};
		SharedStash::Page record;
		record.id = (SharedStash::Id)page->id + 1;
		record.ordinal = ordinal++;
		record.flags = page->flags;
		if (page->name) record.name = page->name;
		if (record.name.size() > 20)
			return false;
		snapshot.pages.push_back(record);
		pagePointers.push_back(page);
		bool shown = page == PCPY->currentStash;
		Unit* first = shown ? D2InventoryGetFirstItem(PCInventory) : page->ptListItem;
		for (Unit* item = first; item; item = D2UnitGetNextItem(item))
		{
			if (!item->ptItemData)
				return false;
			if (D2ItemGetPage(item) != 4 || (shown && item->ptItemData->ptInventory != PCInventory))
				continue;
			ItemsBIN* definition = D2GetItemsBIN(item->nTxtFileNo);
			if (!definition || !item->path || !definition->InvWidth || !definition->InvHeight
				|| item->path->x >= grid.gridX || item->path->y >= grid.gridY
				|| item->path->x + definition->InvWidth > grid.gridX
				|| item->path->y + definition->InvHeight > grid.gridY)
				return false;
			for (DWORD y = item->path->y; y < item->path->y + definition->InvHeight; ++y)
				for (DWORD x = item->path->x; x < item->path->x + definition->InvWidth; ++x)
				{
					if (used[y * grid.gridX + x]) return false;
					used[y * grid.gridX + x] = true;
				}
			SharedStash::Item entry;
			entry.id = nextItem++;
			entry.version = 1;
			entry.page = record.id;
			entry.x = item->path->x;
			entry.y = item->path->y;
			entry.width = definition->InvWidth;
			entry.height = definition->InvHeight;
			if (!saveItem(item, entry.data)) return false;
			snapshot.items.push_back(entry);
			gids.push_back(item->nItemNum);
		}
	}
	SharedStash::Id cube = 0, cubePage = 0;
	for (size_t i = 0; i < gids.size(); ++i)
		if (gids[i] == activeCubeGid) { cube = snapshot.items[i].id; cubePage = snapshot.items[i].page; break; }
	if (cube)
	{
		bool used[12] = {};
		for (Unit* item = D2InventoryGetFirstItem(PCInventory); item; item = D2UnitGetNextItem(item))
		{
			if (!item->ptItemData || D2ItemGetPage(item) != 3 || item->ptItemData->ptInventory != PCInventory) continue;
			ItemsBIN* definition = D2GetItemsBIN(item->nTxtFileNo);
			if (!definition || !item->path || !definition->InvWidth || !definition->InvHeight
				|| item->path->x >= 3 || item->path->y >= 4
				|| item->path->x + definition->InvWidth > 3 || item->path->y + definition->InvHeight > 4) return false;
			for (DWORD y = item->path->y; y < item->path->y + definition->InvHeight; ++y)
				for (DWORD x = item->path->x; x < item->path->x + definition->InvWidth; ++x)
				{ if (used[y * 3 + x]) return false; used[y * 3 + x] = true; }
			SharedStash::Item entry;
			entry.id = nextItem++;
			entry.parent = cube;
			entry.page = cubePage;
			entry.version = 1;
			entry.x = item->path->x; entry.y = item->path->y;
			entry.width = definition->InvWidth; entry.height = definition->InvHeight;
			if (!saveItem(item, entry.data)) return false;
			snapshot.items.push_back(entry); gids.push_back(item->nItemNum);
		}
	}
	// Failure leaves the caller's prior snapshot intact.
	out = snapshot;
	itemGids.swap(gids);
	sourcePages.swap(pagePointers);
	return true;
}
