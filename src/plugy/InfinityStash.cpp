/*=================================================================
	File created by Yohann NICOLAS.
	Add support 1.13d by L'Autour.
	Add support 1.14d by haxifix.

	Gestion of the infinity and shared Stash.

=================================================================*/

#include "infinityStash.h"
#include "stashProtocol.h"
#include "sharedStashNative.h"
#include "updateClient.h"
#include "interface_Stash.h"
#include "customLibraries.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define STASH_TAG 0x5453			//"ST"
#define JM_TAG 0x4D4A				//"JM"

DWORD maxSelfPages = -1;
DWORD maxSharedPages = -1;
DWORD nbPagesPerIndex = 10;
DWORD nbPagesPerIndex2 = 100;
bool active_multiPageStash = false;
bool autoRenameStashPage = false;
int active_SharedStashInMultiPlayer = 0;//0: disabled, 1:shared in SP, 2: shared in SP & TCP/IP
bool active_sharedStash = false;
bool separateHardSoftStash = false;
bool active_sharedGold=false;
char* sharedStashFilename = NULL;

typedef int (*TchangeToSelectedStash)(Unit* ptChar, Stash* newStash, DWORD bOnlyItems, DWORD bIsClient);

Unit* firstClassicStashItem(Unit* ptChar)
{
	Unit* ptItem = D2InventoryGetFirstItem(PCInventory);
	while (ptItem)
	{
		if (D2ItemGetPage(D2GetRealItem(ptItem)) == 4)
			return ptItem;
		ptItem = D2UnitGetNextItem(ptItem);
	}
	return NULL;
}

DWORD endStashList(Unit* ptChar, Stash* ptStash)
{
	Stash* stash = ptStash;

	while (stash)
	{
		if (stash->ptListItem || ((stash == PCPY->currentStash) && firstClassicStashItem(ptChar))) return 0;
		if (stash->name != NULL && stash->name[0]) return 0;
		stash = stash->nextStash;
	}
	return 1;
}

//bool isInList(Stash* list, Stash* pStash)
//{
//	Stash* ptStash = list;
//	while(ptStash)
//	{
//		if (ptStash->id == pStash->id) return true;
//		ptStash = ptStash->nextStash;
//	}
//	return false;
//}


Stash* getLastStash(Stash* ptStash)
{
	if (!ptStash)
		return NULL;
	while (ptStash->nextStash)
		ptStash = ptStash->nextStash;
	return ptStash;
}

Stash* newStash(DWORD id)
{
	d2_assert( id == 0xFFFFFFFF , "Too much stash", __FILE__, __LINE__);

	Stash* stash = (Stash*)malloc(sizeof(Stash));//D2AllocMem(memPool, sizeof(Stash),__FILE__,__LINE__,0);
	d2_assert(!stash , "Error on stash allocation.", __FILE__, __LINE__);
	ZeroMemory(stash, sizeof(Stash));
	stash->id = id;

	return stash;
}

Stash* addStash(Unit* ptChar, bool isShared, bool autoSetIndex, Stash* previous)
{
	previous = getLastStash(previous ? previous : isShared ? PCPY->sharedStash : PCPY->selfStash);
	if (previous)
		isShared = previous->isShared;

	Stash* stash = newStash(isShared ? PCPY->nbSharedPages++ : PCPY->nbSelfPages++);
	stash->isShared = isShared;
	stash->previousStash = previous;

	if (previous)
	{
		previous->nextStash = stash;
		if (autoSetIndex)
		{
			stash->isIndex = ((stash->id + 1) % nbPagesPerIndex) == 0;
			stash->isMainIndex = ((stash->id + 1) % nbPagesPerIndex2) == 0;
		}
	}
	else if (isShared)
	{
		PCPY->sharedStash = stash;
		stash->isIndex = 1;
		stash->isMainIndex = 1;
	}
	else
	{
		PCPY->selfStash = stash;
		stash->isIndex = 1;
		stash->isMainIndex = 1;
	}

	log_msg("AddStash: stash->id=%d\tstash->isShared=%d\tnbSelf=%d\tnbShared=%d\n", stash->id, stash->isShared, PCPY->nbSelfPages, PCPY->nbSharedPages);
	return stash;
}

Stash* getStash(Unit* ptChar, DWORD isShared, DWORD id)
{
	Stash* ptStash = isShared ? PCPY->sharedStash : PCPY->selfStash;

	while(ptStash)
	{
		if (ptStash->id == id) return ptStash;
		ptStash = ptStash->nextStash;
	}

	return NULL;
}


Unit* findStashItem(Unit* ptChar, DWORD gid, StashLoc* out)
{
	out->page = NULL;
	out->prev = NULL;
	out->kind = STASHLOC_PHANTOM;
	for (Unit* ptItem = D2InventoryGetFirstItem(PCInventory); ptItem; ptItem = D2UnitGetNextItem(ptItem))
	{
		if (ptItem->nItemNum != gid)
			continue;
		if (D2ItemGetPage(ptItem) == 4 && ptItem->ptItemData->ptInventory == PCInventory)
		{
			out->kind = STASHLOC_CURRENT;
			out->page = PCPY->currentStash;
		}
		return ptItem;
	}

	Stash* lists[2] = { PCPY->selfStash, PCPY->sharedStash };
	for (int i = 0; i < 2; i++)
		for (Stash* page = lists[i]; page; page = page->nextStash)
		{
			if (page == PCPY->currentStash)
				continue;
			Unit* prev = NULL;
			for (Unit* ptItem = page->ptListItem; ptItem; prev = ptItem, ptItem = D2UnitGetNextItem(ptItem))
				if (ptItem->nItemNum == gid)
				{
					out->kind = STASHLOC_LIST;
					out->page = page;
					out->prev = prev;
					return ptItem;
				}
		}
	return NULL;
}

bool detachItem(Unit* ptChar, Unit* item, StashLoc* loc)
{
	if (loc->kind == STASHLOC_CURRENT)
	{
		// The unlink clears next/prev; ItemData2 is cleared too but stays with a stash item, as in changeToSelectedStash.
		BYTE tmp = item->ptItemData->ItemData2;
		if (!D2InvRemoveItem(PCInventory, item))
		{
			log_msg("detachItem -> item %u: the inventory refused to release it; left in place\n", item->nItemNum);
			return false;
		}
		item->ptItemData->ItemData2 = tmp;
		return true;
	}
	if (loc->kind == STASHLOC_LIST)
	{
		Unit* next = item->ptItemData->ptNextItem;
		if (loc->prev)
			loc->prev->ptItemData->ptNextItem = next;
		else
			loc->page->ptListItem = next;
		item->ptItemData->ptNextItem = NULL;
		item->ptItemData->ptPrevItem = NULL;
		return true;
	}
	log_msg("detachItem -> item %u is not an owned stash item; left in place\n", item->nItemNum);
	return false;
}

bool attachItem(Unit* ptChar, Unit* item, Stash* target, BYTE x, BYTE y)
{
	// A still-linked item would drag the rest of its old list into the chain or the target list.
	d2_assert(item->ptItemData->ptNextItem != NULL, "attachItem : item is still linked", __FILE__, __LINE__);
	if (target == PCPY->currentStash)
		return D2InvAddItem(PCInventory, item, x, y, 0xC, 0, 4) != 0;	// 1.14d ignores the bIsClient argument
	item->path->x = x;
	item->path->y = y;
	D2ItemSetPage(item, 4);	// saveStash writes only page-4 items
	item->ptItemData->ptNextItem = target->ptListItem;
	target->ptListItem = item;
	return true;
}

bool stashFindSpot(Unit* ptChar, Stash* page, Unit* item, BYTE* x, BYTE* y, bool explicitXY)
{
	// the record and resolution D2InvAddItem checks a stash item against, so a spot found here is one it accepts
	InvGridInfo grid;
	D2GetInventoryGrid(0xC, 0, &grid);
	ItemsBIN* itemBin = D2GetItemsBIN(item->nTxtFileNo);
	if (!itemBin || !grid.gridX || !grid.gridY)
		return false;
	DWORD width = itemBin->InvWidth;
	DWORD height = itemBin->InvHeight;
	if (!width || !height || width > grid.gridX || height > grid.gridY)
		return false;
	BYTE* used = (BYTE*)calloc(grid.gridX * grid.gridY, 1);
	if (!used)
		return false;

	// the current page's items are the owned page-4 items of the inventory chain; other pages hold theirs in a list
	bool isCurrent = page == PCPY->currentStash;
	for (Unit* ptItem = isCurrent ? D2InventoryGetFirstItem(PCInventory) : page->ptListItem; ptItem; ptItem = D2UnitGetNextItem(ptItem))
	{
		if (ptItem == item || D2ItemGetPage(ptItem) != 4 || (isCurrent && ptItem->ptItemData->ptInventory != PCInventory))
			continue;
		ItemsBIN* bin = D2GetItemsBIN(ptItem->nTxtFileNo);
		if (!bin)
			continue;
		for (DWORD i = ptItem->path->x; i < ptItem->path->x + bin->InvWidth && i < grid.gridX; i++)
			for (DWORD j = ptItem->path->y; j < ptItem->path->y + bin->InvHeight && j < grid.gridY; j++)
				used[j * grid.gridX + i] = 1;
	}

	bool found = false;
	DWORD firstX = explicitXY ? *x : 0;
	DWORD lastX = explicitXY ? *x : grid.gridX - width;
	DWORD firstY = explicitXY ? *y : 0;
	DWORD lastY = explicitXY ? *y : grid.gridY - height;
	// column-major from (0,0), the order kolbot's Container.FindSpot uses
	for (DWORD i = firstX; !found && i <= lastX && i + width <= grid.gridX; i++)
		for (DWORD j = firstY; !found && j <= lastY && j + height <= grid.gridY; j++)
		{
			bool isFree = true;
			for (DWORD a = 0; isFree && a < width; a++)
				for (DWORD b = 0; isFree && b < height; b++)
					isFree = !used[(j + b) * grid.gridX + i + a];
			if (isFree)
			{
				*x = (BYTE)i;
				*y = (BYTE)j;
				found = true;
			}
		}
	free(used);
	return found;
}

static void placeUnplacedItems(Unit* ptChar, Unit* unplaced, Stash* source, DWORD bIsClient);


int changeToSelectedStash_9(Unit* ptChar, Stash* newStash, DWORD bOnlyItems, DWORD bIsClient)
{
	if (!newStash) return 0;

	Stash* currentStash = PCPY->currentStash;
	if (currentStash == newStash) return 0;

	log_msg("changeToSelectedStash ID:%d\tshared:%d\tonlyItems:%d\tclient:%d\n", newStash->id, newStash->isShared, bOnlyItems, bIsClient);

	d2_assert( currentStash && currentStash->ptListItem, "ERROR : currentStash isn't empty (ptListItem != NULL)",__FILE__,__LINE__);

	// collect items to remove
	Inventory* ptInventory = PCInventory;
	Unit* ptNextItem;
	Unit* ptPrevItem = NULL;
	Unit* ptItem = D2InventoryGetFirstItem(ptInventory);
	while (ptItem)
	{
		ptNextItem = D2UnitGetNextItem(ptItem);
		if (D2ItemGetPage(D2GetRealItem(ptItem)) == 4)
		{
			D2SetAnim(D2GetRealItem(ptItem),-1);
			if (ptPrevItem)	{
				ptPrevItem->mode = (DWORD)ptNextItem;//is ptPrevItem->nextNode = ptNextItem;
			} else {
				ptInventory->currentUsedSocket = (DWORD)ptNextItem;//is ptInventory->ptListItem = ptNextItem;
			}
			if (!ptNextItem)
				ptInventory->Inventory2C = (DWORD)ptPrevItem;

			ptInventory->Inventory30 = ptInventory->Inventory30 - 1;
			D2Common10250(__FILE__,__LINE__,ptInventory, D2GetPosX(D2GetRealItem(ptItem)), D2GetPosY(D2GetRealItem(ptItem)), 0xC, bIsClient, 4);
//			ptItem = D2InvRemoveItem(PCInventory, D2GetRealItem(ptItem));
//			D2Common10250(__FILE__,__LINE__,PCInventory, D2GetPosX(ptItem), D2GetPosY(ptItem), 0xC, bIsClient, 4);
			if (currentStash)
			{
//				ptItem = setNextItem(ptItem, PCPY->currentStash->ptListItem);
				ptItem->mode = (DWORD)currentStash->ptListItem;//is ptItem->nextNode = ptListItem
				currentStash->ptListItem = ptItem;
			};
		} else ptPrevItem = ptItem;
		ptItem = ptNextItem;
	}

	// add items of new stash
	ptItem = newStash->ptListItem;
	while (ptItem)
	{
		D2InvAddItem(PCInventory, D2GetRealItem(ptItem), D2GetPosX(D2GetRealItem(ptItem)), D2GetPosY(D2GetRealItem(ptItem)), 0xC, bIsClient, 4);
		D2Common10242(PCInventory, D2GetRealItem(ptItem), 1);
		ptItem = D2UnitGetNextItem(ptItem);
	}
	if (bOnlyItems)
		newStash->ptListItem = PCPY->currentStash->ptListItem;
	else
		PCPY->currentStash = newStash;
	PCPY->currentStash->ptListItem = NULL;

	return 1;
}

// keepPinned (page swaps, 1.14d): a pinned item of the page shown stays in the grid and a pinned item of newStash
// stays in its list, so a swap never carries one off its page; every other switch moves every item.
static int changeStashItems(Unit* ptChar, Stash* newStash, DWORD bOnlyItems, DWORD bIsClient, bool keepPinned)
{
	if (!newStash) return 0;

	Stash* currentStash = PCPY->currentStash;
	if (currentStash == newStash) return 0;

	log_msg("changeToSelectedStash ID:%d\tshared:%d\tonlyItems:%d\tclient:%d\n",newStash->id,newStash->isShared, bOnlyItems,bIsClient);

	d2_assert( currentStash && currentStash->ptListItem, "ERROR : currentStash isn't empty (ptListItem != NULL)",__FILE__,__LINE__);

	// Remove items from current page
	Unit* ptNextItem;
	Unit* ptItem = D2InventoryGetFirstItem(PCInventory);
	while (ptItem)
	{
		ptNextItem = D2UnitGetNextItem(ptItem);
		if (D2ItemGetPage(ptItem) == 4 && !(keepPinned && isPinnedItem(ptChar, ptItem, bIsClient)))
		{
			BYTE tmp = ptItem->ptItemData->ItemData2;
			ptItem = D2InvRemoveItem(PCInventory, ptItem);
			ptItem->ptItemData->ItemData2 = tmp;
			if (currentStash)
			{
				ptItem->ptItemData->ptNextItem = currentStash->ptListItem;
				currentStash->ptListItem = ptItem;
			}
		}
		ptItem = ptNextItem;
	}

	// add items of new stash
	ptItem = newStash->ptListItem;
	Unit* unplaced = NULL;
	Unit* lastUnplaced = NULL;
	Unit* kept = NULL;
	Unit* lastKept = NULL;
	if (version_D2Game == V114d)
	{
		// D2InvAddItem's tail link never clears the added item's ptNextItem, and a refused item stays out of the
		// chain, so each item leaves the page list before its add; refused ones are kept, in list order.
		while (ptItem)
		{
			ptNextItem = ptItem->ptItemData->ptNextItem;
			ptItem->ptItemData->ptNextItem = NULL;
			if (keepPinned && isPinnedItem(ptChar, ptItem, bIsClient))
			{
				if (lastKept)
					lastKept->ptItemData->ptNextItem = ptItem;
				else
					kept = ptItem;
				lastKept = ptItem;
				ptItem = ptNextItem;
				continue;
			}
			ptItem->ptItemData->ptPrevItem = NULL;
			if (!D2InvAddItem(PCInventory, ptItem, ptItem->path->x, ptItem->path->y, 0xC, bIsClient, 4))
			{
				if (lastUnplaced)
					lastUnplaced->ptItemData->ptNextItem = ptItem;
				else
					unplaced = ptItem;
				lastUnplaced = ptItem;
			}
			ptItem = ptNextItem;
		}
	}
	else while (ptItem)
	{
		D2InvAddItem(PCInventory, ptItem, ptItem->path->x, ptItem->path->y, 0xC, bIsClient, 4);
		ptItem = D2UnitGetNextItem(ptItem);
	}
	if (bOnlyItems)
		newStash->ptListItem = PCPY->currentStash->ptListItem;
	else
		PCPY->currentStash = newStash;
	PCPY->currentStash->ptListItem = NULL;
	// kept is set only on a swap (bOnlyItems): newStash's list is then its kept pinned items plus the grid's others
	if (kept)
	{
		lastKept->ptItemData->ptNextItem = newStash->ptListItem;
		newStash->ptListItem = kept;
	}

	if (unplaced)
		placeUnplacedItems(ptChar, unplaced, newStash, bIsClient);

	return 1;
}

int changeToSelectedStash_10(Unit* ptChar, Stash* newStash, DWORD bOnlyItems, DWORD bIsClient)
{
	return changeStashItems(ptChar, newStash, bOnlyItems, bIsClient, false);
}

TchangeToSelectedStash changeToSelectedStash;


// The game's inventory loader frees every item it can't place at its saved position (e.g. a page saved with
// ActiveBigStash=1 loaded with it off) and still reports success. While a PlugY page loads, such items are held
// instead, then re-placed by loadStash (same page) or loadStashList (new pages at the end).
static bool holdUnplacedStashItems = false;
static Unit** heldItems = NULL;
static DWORD nbHeldItems = 0;
static DWORD maxHeldItems = 0;
static DWORD nbMovedItems = 0;	// items held while loading the current stash list

static bool holdItem(Unit* ptItem)
{
	if (nbHeldItems == maxHeldItems)
	{
		DWORD newMax = maxHeldItems ? maxHeldItems * 2 : 16;
		Unit** newList = (Unit**)realloc(heldItems, newMax * sizeof(Unit*));
		if (!newList) return false;
		heldItems = newList;
		maxHeldItems = newMax;
	}
	heldItems[nbHeldItems++] = ptItem;
	nbMovedItems++;
	return true;
}

// Replaces the loader's call to D2LoadPlaceItem (same register/stack contract).
DWORD FASTCALL placeLoadedItem(Game* ptGame, Unit* ptChar, Unit* ptItem, Unit* ptParent)
{
	DWORD nativeResult;
	if (SharedNativePlaceLoadedItem(ptGame, ptChar, ptItem, ptParent, &nativeResult))
		return nativeResult;
	DWORD ret = D2LoadPlaceItem(ptGame, ptChar, ptItem, ptParent);
	// a failed stash placement leaves the item unplaced in cursor mode (4), the state D2PlaceItemInInventory needs
	if (ret && holdUnplacedStashItems && ptItem && ptItem->mode == 4 && D2ItemGetPage(ptItem) == 4 && holdItem(ptItem))
	{
		log_msg("loadStash -> item %u doesn't fit at its saved position (%u,%u); it will be moved\n", ptItem->nItemNum, ptItem->path->x, ptItem->path->y);
		return 0;
	}
	return ret;
}

// Uses the game's own free-spot search on the current page of the item's inventory page.
static bool placeInFreeSpot(Unit* ptChar, Unit* ptItem)
{
	return D2PlaceItemInInventory(__FILE__, __LINE__, PCGame, ptChar, ptItem->nItemNum, 0, 0, 1, 1, NULL) != 0;
}

static Stash* addMovedItemsPage(Unit* ptChar, Stash* lastStash)
{
	Stash* page = addStash(ptChar, lastStash->isShared, false, lastStash);
	page->name = (char*)malloc(sizeof(MOVED_ITEMS_PAGE_NAME));
	if (page->name)
		strcpy(page->name, MOVED_ITEMS_PAGE_NAME);
	return page;
}

// Items still held after their own page loaded go to new pages appended to the list; leaves the last one selected.
static DWORD moveHeldItemsToNewPages(Unit* ptChar, Stash* lastStash, DWORD* nbNewPages)
{
	Stash* page = NULL;
	for (DWORD i = 0; i < nbHeldItems; i++)
	{
		Unit* ptItem = heldItems[i];
		if (!page || !placeInFreeSpot(ptChar, ptItem))
		{
			page = addMovedItemsPage(ptChar, lastStash);
			changeToSelectedStash(ptChar, page, 0, false);
			(*nbNewPages)++;
			if (!placeInFreeSpot(ptChar, ptItem))
			{
				log_msg("loadStashList -> item %u doesn't fit even on an empty page\n", ptItem->nItemNum);
				nbHeldItems = 0;
				return 0x7;//Unable to enter game. Bad inventory data
			}
		}
		log_msg("loadStashList -> item %u moved to (%u,%u) on new page %u\n", ptItem->nItemNum, ptItem->path->x, ptItem->path->y, page->id + 1);
	}
	nbHeldItems = 0;
	return 0;
}

static void recordPendingMove(Unit* ptChar, Unit* ptItem, Stash* page, BYTE x, BYTE y)
{
	// before the join flush the client is built from the final server state, so there is nothing to report
	if (!PCPY->clientReady)
	{
		log_msg("page switch -> item %u: client not built yet, relocation not recorded\n", ptItem->nItemNum);
		return;
	}
	if (PCPY->nbPendingMoves >= sizeof(PCPY->pendingMoves) / sizeof(PCPY->pendingMoves[0]))
	{
		log_msg("page switch -> item %u: pending relocation list full, relocation not recorded\n", ptItem->nItemNum);
		return;
	}
	PYPendingMove* move = &PCPY->pendingMoves[PCPY->nbPendingMoves++];
	move->gid = ptItem->nItemNum;
	move->page = page->id;
	move->shared = (BYTE)page->isShared;
	move->x = x;
	move->y = y;
}

// deleteStash rewrites this page's list right after its switch, so nothing may be placed on it then. Server side only:
// a TCP/IP host runs its own client on another thread, and only the server relocates.
static Stash* pageRefilledByDelete = NULL;

// Items a page switch couldn't put back at their saved cells (unplaced, linked through ptNextItem, in list order).
// The client never decides where they go: it leaves each one detached in no list, like a hidden page item, still in
// the client unit table, so no PlugY pointer outlives a packet that frees it and the game's teardown frees it at
// exit; the server's report finds it by gid. The server keeps every one in the save set: a free spot of the page
// now shown, else the last page of the item's list if it is a "Moved items" page with room, else a new one.
static void placeUnplacedItems(Unit* ptChar, Unit* unplaced, Stash* source, DWORD bIsClient)
{
	while (unplaced)
	{
		Unit* ptItem = unplaced;
		unplaced = ptItem->ptItemData->ptNextItem;
		ptItem->ptItemData->ptNextItem = NULL;
		DWORD fromX = ptItem->path->x;
		DWORD fromY = ptItem->path->y;
		if (bIsClient)
		{
			PCPY->clientDesync++;
			log_msg("page switch (client) -> item %u can't go back to (%u,%u); left detached until the server places it (desync %u)\n", ptItem->nItemNum, fromX, fromY, PCPY->clientDesync);
			continue;
		}
		BYTE x = 0, y = 0;
		Stash* page = PCPY->currentStash;
		if (!stashFindSpot(ptChar, page, ptItem, &x, &y, false) || !attachItem(ptChar, ptItem, page, x, y))
		{
			Stash* last = getLastStash(source->isShared ? PCPY->sharedStash : PCPY->selfStash);
			if (last && last != PCPY->currentStash && last != pageRefilledByDelete && isMovedItemsPageName(last->name) && stashFindSpot(ptChar, last, ptItem, &x, &y, false))
				page = last;
			else
			{
				page = addMovedItemsPage(ptChar, last);
				x = y = 0;
				// Saved at (0,0) rather than dropped: if the game's own grid refuses it there too, that character's
				// next load fails (0x7) instead of losing the item.
				if (!stashFindSpot(ptChar, page, ptItem, &x, &y, false))
					log_msg("ERROR page switch -> item %u doesn't fit even on an empty page; saved at (0,0), the next load may refuse the character\n", ptItem->nItemNum);
			}
			attachItem(ptChar, ptItem, page, x, y);
		}
		log_msg("page switch -> item %u can't go back to (%u,%u); relocated to (%u,%u) on %s page %u%s\n", ptItem->nItemNum, fromX, fromY, x, y, page->isShared ? "shared" : "personal", page->id + 1, page == PCPY->currentStash ? " (shown)" : "");
		recordPendingMove(ptChar, ptItem, page, x, y);
	}
}

void flushPendingMoves(Unit* ptChar)
{
	if (!ptChar || !PCPlayerData)
		return;
	// Each record is resolved now, not when it was made: deleteStash moves page contents to other ids after its switch.
	// The client ran the same switch on its own copy, so it doesn't know where the item was: the source is unknown.
	for (DWORD i = 0; i < PCPY->nbPendingMoves; i++)
	{
		PYPendingMove* move = &PCPY->pendingMoves[i];
		StashLoc loc;
		Unit* ptItem = findStashItem(ptChar, move->gid, &loc);
		if (!ptItem || !loc.page)
		{
			log_msg("flushPendingMoves -> item %u is no longer a stash item; nothing to report\n", move->gid);
			continue;
		}
		move->page = loc.page->id;
		move->shared = (BYTE)loc.page->isShared;
		move->x = (BYTE)ptItem->path->x;
		move->y = (BYTE)ptItem->path->y;
		log_msg("flushPendingMoves -> item %u is on %s page %u at (%u,%u)\n", move->gid, move->shared ? "shared" : "personal", move->page + 1, move->x, move->y);
		sendItemMoved(ptChar, move->gid, NULL, false, loc.page, move->x, move->y, PY_MOVED_RELOCATED, 0);
	}
	PCPY->nbPendingMoves = 0;
}

// Server side only: puts an item that is in no list and no grid where a page switch puts one it can't place.
void relocateStashItem(Unit* ptChar, Unit* item, Stash* source)
{
	item->ptItemData->ptNextItem = NULL;
	item->ptItemData->ptPrevItem = NULL;
	placeUnplacedItems(ptChar, item, source, 0);
}

DWORD pinCodes[MAX_PIN_CODES];
DWORD nbPinCodes = 0;

// A client that never confirmed the protocol has no codes from the server and swaps every item: pinning on the
// server alone would leave the two sides showing different grids. The confirm only answers UC_PROTO, which
// ActiveStashProtocol=0 never sends, but a script can send it too.
bool serverPinsSwaps(Unit* ptChar)
{
	return version_D2Game == V114d && active_stashProtocol && nbPinCodes && ptChar && PCPlayerData
		&& PCPY->clientProto >= PY_STASH_PROTO;
}

// Item codes compared ignoring case, so PinnedItems=BOX pins the cube too.
static DWORD lowerItemCode(DWORD code)
{
	BYTE* c = (BYTE*)&code;
	for (int i = 0; i < 4; i++)
		if (c[i] >= 'A' && c[i] <= 'Z')
			c[i] += 'a' - 'A';
	return code;
}

bool isPinnedItem(Unit* ptChar, Unit* item, DWORD bIsClient)
{
	if (version_D2Common != V114d || !ptChar || !PCPlayerData || !item)
		return false;
	const DWORD* codes = pinCodes;
	DWORD nbCodes = nbPinCodes;
	if (bIsClient)
	{
		codes = PCPY->serverPinCodes;
		nbCodes = PCPY->nbServerPinCodes;
	}
	else if (!serverPinsSwaps(ptChar))
		return false;
	if (!nbCodes)
		return false;
	ItemsBIN* itemBin = D2GetItemsBIN(item->nTxtFileNo);
	if (!itemBin)
		return false;
	DWORD itemCode = lowerItemCode(itemBin->ItemCode);
	for (DWORD i = 0; i < nbCodes && i < MAX_PIN_CODES; i++)
		if (itemCode == lowerItemCode(codes[i]))
			return true;
	return false;
}

// Marks in owner the cells of a page's pinned items (pinned) or of its other items. The page shown holds its items in
// the inventory chain, where only owned page-4 items are stash items. Server side.
static void markPageCells(Unit* ptChar, Unit* first, bool isShown, bool pinned, InvGridInfo* grid, Unit** owner)
{
	for (Unit* ptItem = first; ptItem; ptItem = D2UnitGetNextItem(ptItem))
	{
		if (D2ItemGetPage(ptItem) != 4 || (isShown && ptItem->ptItemData->ptInventory != PCInventory) || isPinnedItem(ptChar, ptItem, 0) != pinned)
			continue;
		ItemsBIN* bin = D2GetItemsBIN(ptItem->nTxtFileNo);
		if (!bin)
			continue;
		for (DWORD i = ptItem->path->x; i < ptItem->path->x + bin->InvWidth && i < grid->gridX; i++)
			for (DWORD j = ptItem->path->y; j < ptItem->path->y + bin->InvHeight && j < grid->gridY; j++)
				owner[j * grid->gridX + i] = ptItem;
	}
}

// The item of moving on the first cell it shares with an item of staying (*stayingItem), NULL when none.
static Unit* findCellClash(Unit** moving, Unit** staying, DWORD nbCells, Unit** stayingItem)
{
	for (DWORD c = 0; c < nbCells; c++)
		if (moving[c] && staying[c])
		{
			*stayingItem = staying[c];
			return moving[c];
		}
	return NULL;
}

// Server only, before a swap creates any page. A swap keeps pinned items where they are, so an item swapped onto their
// cells could not be placed and would be relocated, possibly off its page: the swap is refused instead. Checked both
// ways: target's items against the pinned items staying in the grid, the grid's items against those staying in
// target's list.
static bool pinnedSwapBlocked(Unit* ptChar, Stash* target)
{
	if (!serverPinsSwaps(ptChar))
		return false;
	InvGridInfo grid;
	D2GetInventoryGrid(0xC, 0, &grid);
	DWORD nbCells = (DWORD)grid.gridX * grid.gridY;
	if (!nbCells)
		return false;
	Unit** cells = (Unit**)calloc(nbCells * 4, sizeof(Unit*));
	if (!cells)
	{
		log_msg("swap stash refused : memory allocation failed\n");
		return true;
	}
	Unit** shownPinned = cells;
	Unit** shownOthers = cells + nbCells;
	Unit** targetPinned = cells + 2 * nbCells;
	Unit** targetOthers = cells + 3 * nbCells;
	Unit* shownFirst = D2InventoryGetFirstItem(PCInventory);
	markPageCells(ptChar, shownFirst, true, true, &grid, shownPinned);
	markPageCells(ptChar, shownFirst, true, false, &grid, shownOthers);
	markPageCells(ptChar, target->ptListItem, false, true, &grid, targetPinned);
	markPageCells(ptChar, target->ptListItem, false, false, &grid, targetOthers);
	Unit* pinned = NULL;
	Unit* incoming = findCellClash(targetOthers, shownPinned, nbCells, &pinned);
	Unit* outgoing = incoming ? NULL : findCellClash(shownOthers, targetPinned, nbCells, &pinned);
	if (incoming)
		log_msg("swap stash refused : item %u of %s page %u would land on pinned item %u, which stays on the page shown\n",
			incoming->nItemNum, target->isShared ? "shared" : "personal", target->id + 1, pinned->nItemNum);
	else if (outgoing)
		log_msg("swap stash refused : item %u of the page shown would land on pinned item %u, which stays on %s page %u\n",
			outgoing->nItemNum, pinned->nItemNum, target->isShared ? "shared" : "personal", target->id + 1);
	free(cells);
	return incoming || outgoing;
}

int swapStashItems(Unit* ptChar, Stash* target, DWORD bIsClient, bool keepPinned)
{
	return changeStashItems(ptChar, target, 1, bIsClient, keepPinned && version_D2Game == V114d);
}

// Single player only: a modal box on a TCP/IP host would stall the server thread for every player.
void showMovedStashItemsNotice(Unit* ptChar)
{
	DWORD nbSelf = PCPY->selfItemsOnNewPages;
	DWORD nbShared = PCPY->sharedItemsOnNewPages;
	if (!nbSelf && !nbShared)
		return;
	char where[128];
	if (nbSelf && nbShared)
		_snprintf(where, sizeof(where), "%u at the end of your personal stash and %u at the end of your shared stash", nbSelf, nbShared);
	else
		_snprintf(where, sizeof(where), "%u at the end of your %s stash", nbSelf ? nbSelf : nbShared, nbSelf ? "personal" : "shared");
	where[sizeof(where) - 1] = NULL;
	// Names the pages as the stash shows them, in the ANSI code page MessageBoxA reads; a label that code page can't
	// spell gives the saved name instead. A UTF-8 code page spells every label and takes no lossy flag.
	WCHAR shown[STASH_NAME_MAX_SHOWN + 1];
	wcsncpy(shown, getMovedItemsPageLabel(), STASH_NAME_MAX_SHOWN);
	shown[STASH_NAME_MAX_SHOWN] = NULL;
	UINT codePage = GetACP();
	bool utf8 = codePage == CP_UTF8;
	char label[4 * STASH_NAME_MAX_SHOWN + 1];
	BOOL lossy = FALSE;
	if (!WideCharToMultiByte(codePage, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, shown, -1, label, sizeof(label), NULL, utf8 ? NULL : &lossy) || lossy)
		strcpy(label, MOVED_ITEMS_PAGE_NAME);
	log_box("Some stash items didn't fit on their page (the stash was probably saved with a different ActiveBigStash setting).\nPlugY kept them on new pages named \"%s\": %s.", label, where);
}


DWORD loadStash(Unit* ptChar, Stash* ptStash, BYTE data[], DWORD startSize, DWORD maxSize, DWORD* retSize)
{
	DWORD curSize = startSize;
	DWORD nbBytesRead = 0;

	log_msg("loadStash\n");

	// Every read below stays inside the file: a file cut short is refused, never read past its end.
	*retSize = curSize;
	if (curSize > maxSize || maxSize - curSize < 2)
	{
		log_msg("loadStash -> stash of character %s is truncated (no \"ST\" tag at %u, size %u)\n", PCPlayerData->name, curSize, maxSize);
		return 0x7;//Unable to enter game. Bad inventory data
	}
	if( *(WORD *)&data[curSize] != STASH_TAG )//"ST"
	{
		log_msg("loadStash -> Bad tag of stash of character %s : %04X\n",PCPlayerData->name,*(WORD *)&data[curSize]);
		return 0x7;//Unable to enter game. Bad inventory data
	}
	curSize += 2;

	// Read flags : they are present unless the bytes right after the first NUL are the "JM" tag (old format).
	BYTE* nul = (BYTE*)memchr(&data[curSize], 0, maxSize - curSize);
	if (!nul || maxSize - (DWORD)(nul + 1 - data) < 2)
	{
		log_msg("loadStash -> stash of character %s is truncated (flags or name at %u, size %u)\n", PCPlayerData->name, curSize, maxSize);
		return 0x7;//Unable to enter game. Bad inventory data
	}
	if (*(WORD*)(nul + 1) != JM_TAG)
	{
		if (maxSize - curSize < 4)
		{
			log_msg("loadStash -> stash of character %s is truncated (flags at %u, size %u)\n", PCPlayerData->name, curSize, maxSize);
			return 0x7;//Unable to enter game. Bad inventory data
		}
		// the list being loaded decides isShared: a page flagged for the other list would make saveStash serialize the wrong items
		DWORD isShared = ptStash->isShared;
		ptStash->flags = *(DWORD *)&data[curSize];
		ptStash->isShared = isShared;
		curSize += 4;
	}

	// Read Name
	nul = (BYTE*)memchr(&data[curSize], 0, maxSize - curSize);
	if (!nul)
	{
		log_msg("loadStash -> stash of character %s is truncated (name at %u, size %u)\n", PCPlayerData->name, curSize, maxSize);
		return 0x7;//Unable to enter game. Bad inventory data
	}
	DWORD nameLen = nul - &data[curSize];
	if (nameLen)
		ptStash->name = (char*)malloc(nameLen + 1);//D2AllocMem(PCGame->memoryPool, strlen((char *)&data[curSize]),__FILE__,__LINE__,0);
	if (ptStash->name)
		strcpy(ptStash->name, (char *)&data[curSize]);
	curSize += nameLen + 1;

	// Read inventory : at least its "JM" tag and item count.
	if (maxSize - curSize < 4)
	{
		log_msg("loadStash -> stash of character %s is truncated (inventory at %u, size %u)\n", PCPlayerData->name, curSize, maxSize);
		return 0x7;//Unable to enter game. Bad inventory data
	}
	DWORD firstHeld = nbHeldItems;
	holdUnplacedStashItems = version_D2Game == V114d;
	DWORD ret = D2LoadInventory(PCGame, ptChar, (saveBitField*)&data[curSize], 0x60, maxSize-curSize, 0, &nbBytesRead);
	holdUnplacedStashItems = false;
	if (ret) log_msg("loadStash -> D2LoadInventory failed\n");

	// Held items go to free spots of this page only once all of its items are placed, so none takes another's spot.
	DWORD stillHeld = firstHeld;
	for (DWORD i = firstHeld; !ret && i < nbHeldItems; i++)
	{
		if (placeInFreeSpot(ptChar, heldItems[i]))
			log_msg("loadStash -> item %u moved to (%u,%u) on page %u\n", heldItems[i]->nItemNum, heldItems[i]->path->x, heldItems[i]->path->y, ptStash->id + 1);
		else
			heldItems[stillHeld++] = heldItems[i];
	}
	if (!ret) nbHeldItems = stillHeld;
	log_msg("Stash loaded (%d : %s)\n", ptStash->id, ptStash->name);

	*retSize=curSize + nbBytesRead;
	return ret;
}

void autoSetIndex(Stash* firstStash)
{
	if (!firstStash)
		return;
	log_msg("autoSetIndex\n");
	Stash* stash = firstStash;
	while (stash)
	{
		if (stash->isIndex || stash->isMainIndex)
			return;
		stash = stash->nextStash;
	}
	stash = firstStash;
	stash->isIndex = 1;
	stash->isMainIndex = 1;
	stash = stash->nextStash;
	while (stash)
	{
		stash->isIndex = ((stash->id + 1) % nbPagesPerIndex) == 0;
		stash->isMainIndex = ((stash->id + 1) % nbPagesPerIndex2) == 0;
		stash = stash->nextStash;
	}
}

DWORD loadStashList(Unit* ptChar, BYTE* data, DWORD maxSize, DWORD* curSize, bool isShared)
{
	DWORD curStash = 0;
	Stash* newStash = NULL;

	if (*curSize > maxSize || maxSize - *curSize < 4)
	{
		log_msg("loadStashList -> file truncated before the number of stash pages (at %u, size %u)\n", *curSize, maxSize);
		return 0x7;//Unable to enter game. Bad inventory data
	}
	DWORD nbStash = *(DWORD*)&data[*curSize];
	*curSize += 4;

	log_msg("loadStashList nbStash=%d\n", nbStash);
	// every saved page takes at least 7 bytes ("ST", name NUL, "JM", item count), so a larger count is corrupt
	if (nbStash > (maxSize - *curSize) / 7)
	{
		log_msg("loadStashList -> bad number of stash pages : %u\n", nbStash);
		return 0x7;//Unable to enter game. Bad inventory data
	}

	nbHeldItems = 0;
	nbMovedItems = 0;
	while (curStash < nbStash)
	{
		newStash = addStash(ptChar, isShared, false, newStash);
		changeToSelectedStash(ptChar, newStash, 0, false);
		DWORD ret = loadStash(ptChar, newStash, data, *curSize, maxSize, curSize);
		if (ret) return ret;
		curStash++;
	}

	if (nbHeldItems)
	{
		DWORD nbItems = nbHeldItems;
		DWORD nbNewPages = 0;
		DWORD ret = moveHeldItemsToNewPages(ptChar, newStash, &nbNewPages);
		if (ret) return ret;
		if (isShared)
			PCPY->sharedItemsOnNewPages += nbItems;
		else
			PCPY->selfItemsOnNewPages += nbItems;
	}
	if (nbMovedItems)
	{
		log_msg("loadStashList -> %u items moved\n", nbMovedItems);
		if (isShared)
			PCPY->sharedMovedOnLoad = 1;
	}

	if (!isShared)
	{
		if (PCPY->selfStash)
			autoSetIndex(PCPY->selfStash);
		else
		{
			newStash = addStash(ptChar, isShared, true, newStash);
			PCPY->currentStash = newStash;
		}
	}

	if (isShared)
	{
		if (PCPY->sharedStash)
			autoSetIndex(PCPY->sharedStash);
		else
		{
			newStash = addStash(ptChar, isShared, true, newStash);
			if (!PCPY->currentStash)
				PCPY->currentStash = newStash;
		}
	}

	changeToSelectedStash(ptChar, isShared?PCPY->sharedStash:PCPY->selfStash, 0, false);

	return 0;
}

//ADDDATA(DWORD, curSize, 0);

#define DATA (*data + *curSize)
#define ADDDATA(T)		(T*)DATA;		*curSize += sizeof(T)
#define SETDATA(T,V)	*(T*)DATA = V;	*curSize += sizeof(T)

void saveStash(Unit* ptChar, Stash* ptStash, BYTE** data, DWORD* maxSize, DWORD* curSize)
{
	//write "ST"
	SETDATA(WORD, STASH_TAG);

	//Write flags.
	SETDATA(DWORD, ptStash->flags);

	//save name
	if (ptStash->name)
	{
		int size = strlen(ptStash->name);
		if (size > 20) size = 20;
		strncpy((char*)DATA, ptStash->name, size);
		*curSize += size;
	}
	SETDATA(char, NULL);

	//Write "JM" of inventory
	SETDATA(WORD, JM_TAG);

	//Get position of counter of items in inventory
	WORD* ptNBItem = ADDDATA(WORD);
	*ptNBItem = 0;

	//Get first item
	Unit* ptItem;
	if ((ptStash->id == PCPY->currentStash->id) && (ptStash->isShared == PCPY->currentStash->isShared))
		ptItem = D2InventoryGetFirstItem(PCInventory);
	else
		ptItem = ptStash->ptListItem;

	//Write all items
	while (ptItem)
	{
		if (D2ItemGetPage(D2GetRealItem(ptItem)) == 4)
		{
			int nbBytes = D2SaveItem(D2GetRealItem(ptItem), (saveBitField*)DATA, *maxSize - *curSize, 1, 1, 0);
			d2_assert(!nbBytes, "!\"Character has too many items\"", __FILE__, __LINE__ );
			*curSize += nbBytes;
			(*ptNBItem)++;
		}
		ptItem = D2UnitGetNextItem(ptItem);
	}
}


void saveStashList(Unit* ptChar, Stash* ptStash, BYTE** data, DWORD* maxSize, DWORD* curSize)
{
	DWORD curSizeNbStash = *curSize;
	*curSize += sizeof(DWORD);

	DWORD nbStash=0;

	while(ptStash)
	{
		if (*curSize + 0x2000 > *maxSize)
		{
			BYTE* oldData = *data;
			*maxSize *= 2;
			*data = (BYTE *)D2AllocMem(PCGame->memoryPool, *maxSize,__FILE__,__LINE__,0);
			d2_assert(!*data, "Error : Memory allocation", __FILE__, __LINE__);
			CopyMemory(*data, oldData, *curSize);
			D2FreeMem(PCGame->memoryPool, oldData,__FILE__,__LINE__,0);
		}
		saveStash(ptChar, ptStash, data, maxSize, curSize);
		nbStash++;
		ptStash = endStashList(ptChar, ptStash->nextStash) ? NULL : ptStash->nextStash;
	}
	*(DWORD*)(*data + curSizeNbStash) = nbStash;
}


static int compareItemHash(const void* a, const void* b)
{
	DWORD ha = *(const DWORD*)a;
	DWORD hb = *(const DWORD*)b;
	return ha < hb ? -1 : ha > hb ? 1 : 0;
}

// D2SaveItem output of an item and its socketed items without ITEMFLAG_NEWITEM: the game sets it on a socketed item
// as it loads and clears it later by itself, so it would make an untouched stash look modified. Server thread only;
// the flags are restored before anything else can see them.
static DWORD saveItemWithoutNewFlag(Unit* ptItem, saveBitField* data, DWORD maxSize)
{
	Unit* flagged[16];
	DWORD nbFlagged = 0;
	if (ptItem->ptItemData->flags2 & ITEMFLAG_NEWITEM)
		flagged[nbFlagged++] = ptItem;
	Inventory* ptSockets = *(Inventory**)((DWORD)ptItem + shifting.ptInventory);
	for (Unit* ptSocketed = ptSockets ? D2InventoryGetFirstItem(ptSockets) : NULL; ptSocketed && nbFlagged < 16; ptSocketed = D2UnitGetNextItem(ptSocketed))
		if (ptSocketed->ptItemData->flags2 & ITEMFLAG_NEWITEM)
			flagged[nbFlagged++] = ptSocketed;

	for (DWORD i = 0; i < nbFlagged; i++)
		flagged[i]->ptItemData->flags2 &= ~ITEMFLAG_NEWITEM;
	DWORD nbBytes = D2SaveItem(ptItem, data, maxSize, 1, 1, 0);
	for (DWORD i = 0; i < nbFlagged; i++)
		flagged[i]->ptItemData->flags2 |= ITEMFLAG_NEWITEM;
	return nbBytes;
}

// Same page content as saveStash, but with the items sorted: selecting a page reverses its item list.
static bool hashStashCanonical(Unit* ptChar, Stash* ptStash, DWORD* hash)
{
	*hash = fnv1a((BYTE*)&ptStash->flags, sizeof(ptStash->flags), *hash);
	char name[21] = {0};
	if (ptStash->name)
		strncpy(name, ptStash->name, 20);
	*hash = fnv1a((BYTE*)name, strlen(name) + 1, *hash);

	Unit* ptItem;
	if ((ptStash->id == PCPY->currentStash->id) && (ptStash->isShared == PCPY->currentStash->isShared))
		ptItem = D2InventoryGetFirstItem(PCInventory);
	else
		ptItem = ptStash->ptListItem;

	DWORD itemBuffer[0x2000 / sizeof(DWORD)];
	DWORD* itemHashes = NULL;
	DWORD nbItems = 0;
	DWORD maxItems = 0;
	bool ok = true;
	while (ok && ptItem)
	{
		if (D2ItemGetPage(D2GetRealItem(ptItem)) == 4)
		{
			DWORD nbBytes = saveItemWithoutNewFlag(D2GetRealItem(ptItem), (saveBitField*)itemBuffer, sizeof(itemBuffer));
			if (!nbBytes || nbBytes > sizeof(itemBuffer))
			{
				log_msg("hashStashCanonical : can't serialize an item of page %d\n", ptStash->id);
				ok = false;
				break;
			}
			if (nbItems == maxItems)
			{
				maxItems = maxItems ? maxItems * 2 : 64;
				DWORD* newHashes = (DWORD*)realloc(itemHashes, maxItems * sizeof(DWORD));
				if (!newHashes)
				{
					log_msg("hashStashCanonical : memory allocation failed\n");
					ok = false;
					break;
				}
				itemHashes = newHashes;
			}
			itemHashes[nbItems++] = fnv1a((BYTE*)itemBuffer, nbBytes);
		}
		ptItem = D2UnitGetNextItem(ptItem);
	}
	if (ok)
	{
		if (nbItems)
			qsort(itemHashes, nbItems, sizeof(DWORD), compareItemHash);
		*hash = fnv1a((BYTE*)&nbItems, sizeof(nbItems), *hash);
		*hash = fnv1a((BYTE*)itemHashes, nbItems * sizeof(DWORD), *hash);
	}
	free(itemHashes);
	return ok;
}

// Order-independent fingerprint of what saveSharedSaveFile writes, so an untouched shared stash matches its load-time value.
bool getSharedStashCanonicalHash(Unit* ptChar, DWORD* hash)
{
	*hash = FNV1A_INIT;
	if (!PCPY->currentStash)
		return false;
	*hash = fnv1a((BYTE*)&PCPY->sharedGold, sizeof(PCPY->sharedGold), *hash);

	Stash* ptStash = PCPY->sharedStash;
	while (ptStash)
	{
		if (!hashStashCanonical(ptChar, ptStash, hash))
			return false;
		ptStash = endStashList(ptChar, ptStash->nextStash) ? NULL : ptStash->nextStash;
	}

	TCustomDll* currentDll = customDlls;
	if (!currentDll)
		return true;
	DWORD curSize = 0;
	DWORD maxSize = 0x4000;
	BYTE* data = (BYTE *)D2AllocMem(PCGame->memoryPool, maxSize,__FILE__,__LINE__,0);
	if (!data)
	{
		log_msg("getSharedStashCanonicalHash : memory allocation failed\n");
		return false;
	}
	while (currentDll)
	{
		currentDll->saveSharedSaveFile(ptChar, &data, &maxSize, &curSize);
		currentDll=currentDll->nextDll;
	}
	*hash = fnv1a(data, curSize, *hash);
	D2FreeMem(PCGame->memoryPool, data,__FILE__,__LINE__,0);
	return true;
}

/////// client
void updateSelectedStashClient(Unit* ptChar)
{
	Stash* newStash = PCPY->currentStash;
	if (!newStash)
		return;
	updateClient(ptChar, UC_SELECT_STASH, newStash->id, newStash->flags, PCPY->flags);
	updateClient(ptChar, UC_PAGE_NAME, newStash->name);
}

void setSelectedStashClient(DWORD stashId, DWORD stashFlags, DWORD flags, bool bOnlyItems)
{
	log_msg("setSelectedStashClient ID:%d, stashFlags:%d, flags:%08X\n", stashId, stashFlags, flags);
	Unit* ptChar = D2GetClientPlayer();
	// A server update can arrive after the client player is gone (leaving a game).
	if (!hasStashData(ptChar))
		return;
	Stash* newStash = getStash(ptChar, (stashFlags & 1) == 1, stashId);
	if (!newStash) do
		newStash = addStash(ptChar, (stashFlags & 1) == 1, false, newStash);
	while (newStash->id < stashId);
	newStash->flags = stashFlags & ~STASH_SWAP_PINNED;
	// pinned items are kept only when the server kept them, so both sides run the same swap whatever codes this has
	if (bOnlyItems)
		swapStashItems(ptChar, newStash, 1, (stashFlags & STASH_SWAP_PINNED) != 0);
	else
		changeToSelectedStash(ptChar, newStash, 0, 1);
	PCPY->flags = flags;
}


void selectStash(Unit* ptChar, Stash* newStash, bool forceUpdate)
{
	if (!newStash)
		return;
	if (!forceUpdate && newStash == PCPY->currentStash)
		return;
	log_msg("selectStash ID:%d\tshared:%d\tforceUpdate:%d\n", newStash->id, newStash->isShared, forceUpdate);
	changeToSelectedStash(ptChar, newStash, 0, 0);
	updateSelectedStashClient(ptChar);
	flushPendingMoves(ptChar);	// after the client has run the same switch
}



///// public functions
bool hasCurrentStash(Unit* ptChar)
{
	// PCPY is an offset into PlayerData, valid only when PlayerCustomData extended it. The stat bonus installs the
	// 0x9D handler without it, so a stash message must not assume it.
	return hasStashData(ptChar) && PCPY->currentStash;
}

void toggleToSelfStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* selStash = PCPY->selfStash;
	if (selStash && (selStash != PCPY->currentStash))
	{
		PCPY->showSharedStash = false;
		selectStash(ptChar, selStash);
	}
}

void toggleToSharedStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* selStash = PCPY->sharedStash;
	if (selStash && (selStash != PCPY->currentStash))
	{
		PCPY->showSharedStash = true;
		selectStash(ptChar, selStash);
	}
}

void swapStash(Unit* ptChar, Stash* curStash, Stash* swpStash)
{
	if (!ptChar || !curStash || !swpStash || curStash == swpStash)
		return;
	log_msg("swapStash ID:%d\tshared:%d\tonlyItems:%d\tclient:%d\n", swpStash->id, swpStash->isShared, 1, 0);
	bool keepPinned = serverPinsSwaps(ptChar);
	swapStashItems(ptChar, swpStash, 0, keepPinned);
	updateClient(ptChar, UC_SELECT_STASH, swpStash->id, (swpStash->flags & ~STASH_SWAP_PINNED) | 8 | (keepPinned ? STASH_SWAP_PINNED : 0), PCPY->flags);
	flushPendingMoves(ptChar);	// after the client has run the same swap
}

// Page ids come from the client and every missing page up to one gets allocated, so creating pages is capped even
// for an unlimited stash; a page that already exists is always reachable.
bool canCreateStashPage(bool isShared, DWORD page)
{
	DWORD maxPages = isShared ? maxSharedPages : maxSelfPages;
	return (int)maxPages < 0 ? page < 10000 : page <= maxPages;
}

void toggleStash(Unit* ptChar, DWORD page)
{
	log_msg("toggle stash page = %u\n", page);
	if (!hasCurrentStash(ptChar)) return;
	Stash* curStash = PCPY->currentStash;
	Stash* swpStash = curStash->isShared ? PCPY->selfStash : PCPY->sharedStash;
	swapStash(ptChar, curStash, swpStash);
}

void swapStash(Unit* ptChar, DWORD page, bool toggle)
{
	log_msg("swap stash page = %u, toggle=%u\n", page, toggle);
	if (!hasCurrentStash(ptChar)) return;
	Stash* curStash = PCPY->currentStash;
	bool isShared = curStash->isShared != toggle;
	DWORD nbPages = isShared ? PCPY->nbSharedPages : PCPY->nbSelfPages;
	DWORD maxPages = isShared ? maxSharedPages : maxSelfPages;
	Stash* target = isShared ? PCPY->sharedStash : PCPY->selfStash;
	for (DWORD i = 0; target && i < page; i++)
		target = target->nextStash;
	if (!target && !canCreateStashPage(isShared, page))
	{
		log_msg("swap stash refused : page %u is out of range (pages=%u, max=%d)\n", page, nbPages, maxPages);
		return;
	}
	// a refused swap changes nothing and sends nothing, so the client, which only mirrors swaps, stays as it is
	if (target && target != curStash && pinnedSwapBlocked(ptChar, target))
		return;
	Stash* swpStash = curStash->isShared == toggle ? PCPY->selfStash : PCPY->sharedStash;
	if (!swpStash)
		swpStash = addStash(ptChar, !curStash->isShared, true, swpStash);
	for (DWORD i = 0; i < page; i++)
	{
		log_msg("swap stash : %i\n", i);
		if (swpStash->nextStash == NULL)
			addStash(ptChar, swpStash->isShared, false, swpStash);
		swpStash = swpStash->nextStash;
	}
	swapStash(ptChar, curStash, swpStash);
}

void insertStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* curStash = PCPY->currentStash;
	Stash* stash = addStash(ptChar, curStash->isShared, false, curStash);
	while (stash->previousStash != curStash)
	{
		stash->flags = stash->previousStash->flags;
		stash->name = stash->previousStash->name;
		stash->ptListItem = stash->previousStash->ptListItem;
		stash = stash->previousStash;
	}
	stash->isIndex = 0;
	stash->isMainIndex = 0;
	stash->name = NULL;
	stash->ptListItem = NULL;
}

bool canDeleteStash(Unit* ptChar)
{
	return hasCurrentStash(ptChar) && firstClassicStashItem(ptChar) == NULL;
}

bool deleteStash(Unit* ptChar, bool isClient)
{
	if (!canDeleteStash(ptChar))
		return false;
	//if (D2InventoryGetFirstItem())
	Stash* stash = PCPY->currentStash;
	if (stash->nextStash == NULL)
	{
		stash->isIndex = 0;
		stash->isMainIndex = 0;
		stash->name = NULL;
		return true;
	}
	stash->flags = stash->nextStash->flags;
	stash->name = stash->nextStash->name;
	if (stash->nextStash->ptListItem != NULL)
	{
		log_msg("deleteStash ID:%d\tshared:%d\tonlyItems:%d\tclient:%d\n", stash->id, stash->isShared, 1, isClient);
		if (!isClient)
			pageRefilledByDelete = stash->nextStash;
		changeToSelectedStash(ptChar, stash->nextStash, 1, isClient);
		if (!isClient)
			pageRefilledByDelete = NULL;
	}
	stash = stash->nextStash;
	while (stash->nextStash)
	{
		stash->flags = stash->nextStash->flags;
		stash->name = stash->nextStash->name;
		stash->ptListItem = stash->nextStash->ptListItem;
		stash = stash->nextStash;
	}
	stash->isIndex = 0;
	stash->isMainIndex = 0;
	stash->name = NULL;
	stash->ptListItem = NULL;
	return true;
}


void renameCurrentStash(Unit* ptChar, char* name)
{
	log_msg("renameCurrentStash : '%s'\n", name);
	if (!hasCurrentStash(ptChar)) return;
	Stash* stash = PCPY->currentStash;
	int len = 0;
	if (name != NULL)
		len = strlen(name);
	if (stash->name)
		D2FogMemDeAlloc(stash->name, __FILE__, __LINE__, 0);
	if (len > 0)
	{
		stash->name = (char *)malloc(len + 1);//D2FogMemAlloc(len,__FILE__,__LINE__,0);
		strcpy(stash->name, name);
	}
	else
		stash->name = NULL;
}


void setCurrentStashIndex(Unit* ptChar, int index)
{
	if (!hasCurrentStash(ptChar))
		return;
	PCPY->currentStash->isIndex = index >= 1;
	PCPY->currentStash->isMainIndex = index == 2;
	updateSelectedStashClient(ptChar);
}


void selectPreviousStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* selStash = PCPY->currentStash->previousStash;
	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}


void selectPrevious2Stash(Unit* ptChar)// Select first stash
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* selStash = PCPY->showSharedStash ? PCPY->sharedStash : PCPY->selfStash;
	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}

void selectNextStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* selStash = PCPY->currentStash;
	if (!selStash->isShared && (selStash->id >= maxSelfPages))	return;
	if (selStash->isShared && (selStash->id >= maxSharedPages)) return;

	selStash = selStash->nextStash ? selStash->nextStash : addStash(ptChar, PCPY->showSharedStash, true, selStash);

	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}


void selectNext2Stash(Unit* ptChar)//select last stash
{
	if (!hasCurrentStash(ptChar)) return;
	Stash* selStash = PCPY->showSharedStash ? PCPY->sharedStash : PCPY->selfStash;//PCPY->currentStash;
	Stash* lastStash = NULL;
	Unit* currentStashItem = firstClassicStashItem(ptChar);
	while (selStash)
	{
		if (selStash->ptListItem || (selStash == PCPY->currentStash) && currentStashItem) lastStash=selStash;
		selStash = selStash->nextStash;
	}
	if (!lastStash)
		lastStash = PCPY->showSharedStash ? PCPY->sharedStash : PCPY->selfStash;
	if (lastStash != PCPY->currentStash)
		selectStash(ptChar, lastStash);
}

void selectPreviousIndexStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	selectPreviousStash(ptChar);
	Stash* selStash = PCPY->currentStash;
	while (selStash && !selStash->isIndex)
		selStash = selStash->previousStash;

	if (selStash == NULL)
	{
		selStash = PCPY->currentStash;
		while (selStash->previousStash && ((selStash->id + 1) % nbPagesPerIndex != 0))
			selStash = selStash->previousStash;
	}

	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}

void selectPreviousIndex2Stash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	selectPreviousStash(ptChar);
	Stash* selStash = PCPY->currentStash;
	while (selStash && !selStash->isMainIndex)
		selStash = selStash->previousStash;

	if (selStash == NULL)
	{
		selStash = PCPY->currentStash;
		while (selStash->previousStash && ((selStash->id+1) % nbPagesPerIndex2 != 0))
			selStash = selStash->previousStash;
	}

	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}

void selectNextIndexStash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	selectNextStash(ptChar);
	Stash* selStash = PCPY->currentStash;
	while (selStash && !selStash->isIndex)
		selStash = selStash->nextStash;

	if (selStash == NULL)
	{
		selStash = PCPY->currentStash;
		while ((selStash->id + 1) % nbPagesPerIndex != 0)
		{
			if (!selStash->isShared && (selStash->id >= maxSelfPages))	break;
			if (selStash->isShared && (selStash->id >= maxSharedPages)) break;
			selStash = selStash->nextStash ? selStash->nextStash : addStash(ptChar, PCPY->showSharedStash, true, selStash);
		}
	}
	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}

void selectNextIndex2Stash(Unit* ptChar)
{
	if (!hasCurrentStash(ptChar)) return;
	selectNextStash(ptChar);
	Stash* selStash = PCPY->currentStash;
	while (selStash && !selStash->isMainIndex)
		selStash = selStash->nextStash;

	if (selStash == NULL)
	{
		selStash = PCPY->currentStash;
		while ((selStash->id+1) % nbPagesPerIndex2 != 0)
		{
			if (!selStash->isShared && (selStash->id >= maxSelfPages))	break;
			if (selStash->isShared && (selStash->id >= maxSharedPages)) break;
			selStash = selStash->nextStash ? selStash->nextStash : addStash(ptChar, PCPY->showSharedStash, true, selStash);
		}
	}
	if (selStash && (selStash != PCPY->currentStash))
		selectStash(ptChar, selStash);
}


WCHAR* getDefaultStashName(Unit* ptChar)
{
	if (!autoRenameStashPage)
		return getLocalString( PCPY->currentStash->isShared ? STR_SHARED_PAGE_NUMBER : STR_PERSONAL_PAGE_NUMBER);

	int onlyOneUnique = -1;
	int uniqueNameIndex = -1;
	int onlyOneSet = -1;
	int setNameIndex = -1;
	int onlyOneMisc = -1;
	int miscNameIndex = -1;

	Unit* ptItem = D2InventoryGetFirstItem(PCInventory);
	int nb = 0;
	while (ptItem)
	{
		if (D2ItemGetPage(ptItem) == 4)
		{
			if (onlyOneUnique != 0)
			{
				if (ptItem->ptItemData->quality != ITEMQUALITY_UNIQUE)
				{
					onlyOneUnique = 0;
					uniqueNameIndex = -1;
				}
				else
				{
					int uniqueId = D2GetUniqueID(ptItem);
					UniqueItemsBIN*	uniqueBIN = SgptDataTables->uniqueItems + uniqueId;
					if (onlyOneUnique < 0)
					{
						onlyOneUnique = 1;
						uniqueNameIndex = uniqueBIN->uniqueNameId;
					}
					else if (setNameIndex != uniqueBIN->uniqueNameId)
					{
						onlyOneUnique = 0;
						uniqueNameIndex = -1;
					}
				}
			}

			if (onlyOneSet != 0)
			{
				if (ptItem->ptItemData->quality != ITEMQUALITY_SET)
				{
					onlyOneSet = 0;
					setNameIndex = -1;
				}
				else
				{
					int uniqueID = ptItem->ptItemData->uniqueID;
					SetItemsBIN* itemBIN = &SgptDataTables->setItems[uniqueID];
					SetsBIN* setBIN = &SgptDataTables->sets[itemBIN->setId];

					if (onlyOneSet < 0)
					{
						onlyOneSet = 1;
						setNameIndex = setBIN->setNameIndex;
					}
					else if (setNameIndex != setBIN->setNameIndex)
					{
						onlyOneSet = 0;
						setNameIndex = -1;
					}
				}
			}

			if (onlyOneMisc != 0)
			{
				if (ptItem->ptItemData->quality != ITEMQUALITY_NORMAL)
				{
					onlyOneMisc = 0;
					miscNameIndex = -1;
				}
				else
				{
					ItemsBIN* ptItemsBin = D2GetItemsBIN(ptItem->nTxtFileNo);
					if (onlyOneMisc < 0)
					{
						onlyOneMisc = 1;
						miscNameIndex = ptItemsBin->NameStr;
					}
					else if (miscNameIndex != ptItemsBin->NameStr)
					{
						onlyOneMisc = 0;
						miscNameIndex = -1;
					}
				}
			}
		}
		ptItem = D2UnitGetNextItem(ptItem);
	}

	if (onlyOneUnique == 1 && uniqueNameIndex >= 0)
		return StripGender(D2GetStringFromIndex(uniqueNameIndex));
	if (onlyOneSet == 1 && setNameIndex >= 0)
		return StripGender(D2GetStringFromIndex(setNameIndex));
	if (onlyOneMisc == 1 && miscNameIndex >= 0)
		return StripGender(D2GetStringFromIndex(miscNameIndex));

	return getLocalString( PCPY->currentStash->isShared ? STR_SHARED_PAGE_NUMBER : STR_PERSONAL_PAGE_NUMBER);
}

LPCWSTR getMovedItemsPageLabel()
{
	return movedItemsPageLabel(getLocalString(STR_MOVED_ITEMS_PAGE));
}

void getCurrentStashName(WCHAR* buffer, DWORD maxSize, Unit* ptChar)
{
	if (isMovedItemsPageName(PCPY->currentStash->name))
	{
		wcsncpy(buffer, getMovedItemsPageLabel(), maxSize - 1);
	}
	else if (PCPY->currentStash->name && PCPY->currentStash->name[0])
	{
		mbstowcs(buffer, PCPY->currentStash->name, maxSize - 1);
	}
	else
	{
		wcsncpy(buffer, getDefaultStashName(ptChar), maxSize - 1);;
	}
	buffer[20] = NULL;
}


//////////////////////////////////////////////////////////////////////
Stash* curStash2;
DWORD currentSawStash2;

Unit* STDCALL getNextItem(Unit* ptChar, Unit* ptItem)
{
	Unit* item = D2UnitGetNextItem(ptItem);
	if (item) return item;

	if (!curStash2)
	{
		switch (currentSawStash2)
		{
		case 0: curStash2 = PCPY->selfStash;
				currentSawStash2 = 1;
				break;
		case 1: curStash2 = PCPY->sharedStash;
				currentSawStash2 = 2;
				break;
		default: return NULL;//case 2:
		}
	} else {
		curStash2 = curStash2->nextStash;
	}
	if (curStash2)
	{
		item = curStash2->ptListItem;
		if (item) return item;
	}
	return getNextItem(ptChar,item);
}

Unit* STDCALL initGetNextItem(Unit* ptChar, Unit* ptItem)
{
	if (ptChar->nUnitType != UNIT_PLAYER) return NULL;
	if (!PCPY) return NULL;

	curStash2 = NULL;
	currentSawStash2 = 0;
	if (ptItem) return ptItem;
	return getNextItem(ptChar,ptItem);
}

FCT_ASM ( caller_initGetNextItem )
	PUSH DWORD PTR SS:[ESP+0x20]
	PUSH DWORD PTR SS:[ESP+0xC]
	CALL initGetNextItem
	MOV EDI,EAX
	TEST EDI,EDI
	RETN
}}


FCT_ASM ( caller_getNextItem )
	PUSH DWORD PTR SS:[ESP+4]
	PUSH DWORD PTR SS:[ESP+0x10]
	CALL getNextItem
	RETN 4
}}


DWORD STDCALL carry1Limit(Unit* ptChar, Unit* ptItemParam, DWORD itemNum, BYTE page)
{
	if (!ptChar) return 0;
	Unit* ptItem = ptItemParam ? ptItemParam : D2GameGetObject(PCGame, UNIT_ITEM, itemNum);
	if ((page != 4) && (D2GetItemQuality(ptItem) == ITEMQUALITY_UNIQUE) && ptChar)
	{
		int uniqueID = D2GetUniqueID(ptItem);
		if ((uniqueID>=0) && (uniqueID < (int)SgptDataTables->nbUniqueItems))
		{
			UniqueItemsBIN*	uniqueItems = SgptDataTables->uniqueItems + uniqueID;
			if (uniqueItems && (uniqueItems->carry1==1))
			{
				ItemsBIN* ptItemsBin = D2GetItemsBIN(ptItem->nTxtFileNo);
				Unit* ptFirstItem = D2InventoryGetFirstItem(PCInventory);
				if (ptItemsBin && ptFirstItem)
					return D2VerifIfNotCarry1(ptItem, ptItemsBin, ptFirstItem);
			}
		}
	}
	return 0;
}

FCT_ASM ( caller_carry1Limit_114 )
	PUSH DWORD PTR SS:[ESP+0x08]//page
	PUSH 0//EDI
	PUSH DWORD PTR SS:[ESP+0x0C]
	PUSH ESI//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemSetPage
end_carry1Limit:
	ADD ESP,0xC
	XOR EAX,EAX
	POP EDI
	POP EBX
	POP ESI
	MOV ESP,EBP
	POP EBP
	RETN 8
}}

FCT_ASM ( caller_carry1Limit_111 )
	PUSH DWORD PTR SS:[ESP+0x08]//page
	PUSH 0//EDI
	PUSH DWORD PTR SS:[ESP+0x0C]
	PUSH ESI//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemSetPage
end_carry1Limit:
	ADD ESP,0xC
	XOR EAX,EAX
	POP EDI
	POP EBX
	POP ESI
	POP EBP
	RETN 8
}}

FCT_ASM ( caller_carry1Limit )
	PUSH DWORD PTR SS:[ESP+0x08]//page
	PUSH 0//EBP
	PUSH DWORD PTR SS:[ESP+0x0C]
	PUSH DWORD PTR SS:[ESP+0x28]//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemSetPage
end_carry1Limit:
	ADD ESP,0xC
	POP EDI
	POP ESI
	POP EBP
	XOR EAX,EAX
	POP EBX
	ADD ESP,0x24
	RETN 8
}}

FCT_ASM ( caller_carry1LimitSwap_114 )
	PUSH EAX
	PUSH DWORD PTR SS:[ESP+0x20]
	PUSH 0
	PUSH EDI//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemGetPage
end_carry1Limit:
	ADD ESP,8
	XOR EAX,EAX
	POP EBX
	POP EDI
	POP ESI
	MOV ESP, EBP
	POP EBP
	RETN 8
}}

FCT_ASM ( caller_carry1LimitSwap_112 )
	PUSH EAX
	PUSH DWORD PTR SS:[ESP+0x1C]
	PUSH 0
	PUSH ESI//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemGetPage
end_carry1Limit:
	ADD ESP,8
	XOR EAX,EAX
	POP EDI
	POP EBP
	POP ESI
	POP EBX
	POP ECX
	RETN 8
}}

FCT_ASM ( caller_carry1LimitSwap_111 )
	PUSH EAX
	PUSH EBP
	PUSH 0
	PUSH DWORD PTR SS:[ESP+0x24]//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemGetPage
end_carry1Limit:
	ADD ESP,8
	XOR EAX,EAX
	POP EDI
	POP EBP
	POP ESI
	POP EBX
	POP ECX
	RETN 8
}}

FCT_ASM ( caller_carry1LimitSwap )
	PUSH EAX
	PUSH DWORD PTR SS:[ESP+0x20]
	PUSH 0
	PUSH EBP//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2ItemGetPage
end_carry1Limit:
	ADD ESP,8
	POP EDI
	POP ESI
	POP EBP
	XOR EAX,EAX
	POP EBX
	ADD ESP,0x4C
	RETN 8
}}

/*
FCT_ASM ( caller_carry1LimitWhenDrop )
	PUSH EAX
	PUSH 0
	PUSH ESI//ptItem
	PUSH EDI//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	POP EAX
	JE END_carry1LimitWhenDrop
	MOV EDX,0x806
	RETN
END_carry1LimitWhenDrop:
	ADD DWORD PTR SS:[ESP],0x1F
	RETN
}}*/

FCT_ASM ( caller_carry1LimitWhenDrop_114 )
	PUSH 0
	PUSH 0
	PUSH DWORD PTR SS:[ESP+0x10] //ptItem
	PUSH EBX //ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2CanPutItemInInv
end_carry1Limit:
	XOR EAX,EAX
	RETN 0x1C
}}

FCT_ASM ( caller_carry1LimitWhenDrop_111 )
	PUSH 0
	PUSH 0
	PUSH DWORD PTR SS:[ESP+0x10] //ptItem
	PUSH ESI //ptChar
	CALL carry1Limit
	TEST EAX,EAX
	JNZ	end_carry1Limit
	JMP D2CanPutItemInInv
end_carry1Limit:
	XOR EAX,EAX
	RETN 0x1C
}}
/*
FCT_ASM ( caller_carry1LimitWhenDrop )
	PUSH 0
	PUSH 0
	PUSH DWORD PTR SS:[ESP+0x10] //ptItem
	PUSH EDI //ptChar
	CALL carry1Limit
	JNZ	end_carry1Limit
	JMP D2CanPutItemInInv
end_carry1Limit:
	XOR EAX,EAX
	RETN 0x1C
}}*/


FCT_ASM ( caller_carry1LimitWhenDrop )
	PUSH EAX
	PUSH 0
	PUSH 0
	PUSH ESI//ptItem
	PUSH EDI//ptChar
	CALL carry1Limit
	TEST EAX,EAX
	POP EAX
	JNZ END_carry1LimitWhenDrop
	MOV EDX,0x806
	RETN
END_carry1LimitWhenDrop:
	ADD DWORD PTR SS:[ESP],0x1F
	RETN
}}

FCT_ASM ( caller_carry1OutOfStash_114 )
	PUSH ESI
	CALL D2ItemGetPage
	CMP AL,4
	JNZ continue_carry1OutOfStash
	//SUB DWORD PTR SS:[ESP],0xC
	ADD DWORD PTR SS:[ESP],0x17D
	RETN
continue_carry1OutOfStash:
	MOV EDI,DWORD PTR SS:[EBP-4]
	TEST EDI,EDI
	RETN
}}

FCT_ASM ( caller_carry1OutOfStash_111 )
	PUSH EDI
	CALL D2ItemGetPage
	CMP AL,4
	JNZ continue_carry1OutOfStash
	ADD DWORD PTR SS:[ESP],0x17C
	RETN
continue_carry1OutOfStash:
	MOV ESI,DWORD PTR SS:[ESP+0x10]
	TEST ESI,ESI
	RETN
}}

FCT_ASM ( caller_carry1OutOfStash )
	PUSH ESI
	CALL D2ItemGetPage
	CMP AL,4
	JNZ continue_carry1OutOfStash
	ADD DWORD PTR SS:[ESP],0x1AF
	RETN
continue_carry1OutOfStash:
	MOV EAX,DWORD PTR SS:[ESP+0x14]
	TEST EAX,EAX
	RETN
}}


void Install_MultiPageStash()
{
	static int isInstalled = false;
	if (isInstalled) return;

	Install_PlayerCustomData();
	Install_InterfaceStash();

	changeToSelectedStash = version_D2Common >= V110 ? changeToSelectedStash_10 : changeToSelectedStash_9;

	Install_StashProtocol();

	if ( version_D2Game >= V110 )
	{
		log_msg("Patch D2Game for carry1 unique item. (MultiPageStash)\n");

		// Cannot put 2 items carry1 in inventory
		mem_seek R8(D2Game, 0000, 0000, 55050, 57CA3, 2FE63, 99B03, CF1E3, 6B013, 14AC7F);
		MEMJ_REF4( D2ItemSetPage , version_D2Game >= V114d ? caller_carry1Limit_114 : version_D2Game >= V111 ? caller_carry1Limit_111 : caller_carry1Limit);
		//6FC8504F   . E8 94670900    CALL <JMP.&D2Common.#10720>
		//01FD7CA2   . E8 6329FBFF    CALL <JMP.&D2Common.#10485>
		//01F9FE62   . E8 47A8FDFF    CALL <JMP.&D2Common.#10608>
		//6FCB9B02   . E8 9709F7FF    CALL <JMP.&D2Common.#10223>
		//6FCEF1E2   . E8 47B7F3FF    CALL <JMP.&D2Common.#10012>
		//6FC8B012   . E8 13F7F9FF    CALL <JMP.&D2Common.#11026>
		//0054AC7E  |. E8 FDD50D00    CALL Game.00628280                       ; \Game.00628280

		// Cannot put 2 items carry1 in inventory by swapping
		mem_seek R8(D2Game, 0000, 0000, 558D9, 58968, 310E8, 9B6E8, D10C8, 6BC78, 14B179);
		MEMJ_REF4( D2ItemGetPage , version_D2Game >= V114d ? caller_carry1LimitSwap_114 : version_D2Game >= V112 ? caller_carry1LimitSwap_112 : version_D2Game >= V111 ? caller_carry1LimitSwap_111 : caller_carry1LimitSwap);
		//6FC858D8   . E8 175F0900    CALL <JMP.&D2Common.#10719>
		//01FD8967   . E8 8E1DFBFF    CALL <JMP.&D2Common.#10820>
		//01FA10E7   . E8 9A96FDFF    CALL <JMP.&D2Common.#10505>
		//6FCBB6E7   . E8 CAEDF6FF    CALL <JMP.&D2Common.#10370>
		//6FCF10C7   . E8 F895F3FF    CALL <JMP.&D2Common.#10020>
		//6FC8BC77   . E8 22E9F9FF    CALL <JMP.&D2Common.#10810>
		//0054B178  |. E8 D3D00D00    CALL Game.00628250                       ; \Game.00628250

		if ( version_D2Game >= V111 )
		{
			// Cannot put 2 items carry1 in inventory when drop cube
			mem_seek R8(D2Game, 0000, 0000, 0000, 3D935, 49FD5, 17AD5, D7B75, B7B15, 163A89);
			MEMJ_REF4( D2CanPutItemInInv , version_D2Game >= V114d ? caller_carry1LimitWhenDrop_114 : caller_carry1LimitWhenDrop_111);
			//01FBD934  |. E8 5BD3FCFF    |CALL <JMP.&D2Common.#10855>
			//01FB9FD4  |. E8 3912FCFF    |CALL <JMP.&D2Common.#10813>
			//6FC37AD4  |. E8 0535FFFF    |CALL <JMP.&D2Common.#10289>
			//6FCF7B74  |. E8 232FF3FF    |CALL <JMP.&D2Common.#10133>
			//6FCD7B14  |. E8 7D32F5FF    |CALL <JMP.&D2Common.#10402>
			//00563A88   . E8 C37E0D00    CALL Game.0063B950                       ; \Game.0063B950
		} else {
			// Cannot put 2 items carry1 in inventory when drop cube
			mem_seek R8(D2Game, 0000, 0000, 14341, 0000, 0000, 0000, 0000, 0000, 0000);
			memt_byte( 0xBA ,0xE8);
			MEMT_REF4( 0x00000806 , caller_carry1LimitWhenDrop);
			//6FC44341  |. BA 06080000    |MOV EDX,806
		}

		// Verif only carry1 out of stash page when pick up an item
		mem_seek R8(D2Game, 0000, 0000, 1299E, 38E3B, 43F0B, 1209B, D211B, B301B, 15CADD);
		memt_byte( 0x8B ,0xE8);
		MEMT_REF4( version_D2Game >= V114d ? 0xFF85FC7D : version_D2Game >= V111 ? 0x850C2474 : 0x85102444 , version_D2Game >= V114d ? caller_carry1OutOfStash_114 : version_D2Game >= V111 ? caller_carry1OutOfStash_111 : caller_carry1OutOfStash);
		if (version_D2Game < V114d) memt_byte( version_D2Game >= V111 ? 0xF6 : 0xC0 ,0x90);
		//6FC4299E  |. 8B4424 10      |MOV EAX,DWORD PTR SS:[ESP+10]
		//6FC429A2  |. 85C0           |TEST EAX,EAX
		//01FB8E3B  |. 8B7424 0C      |MOV ESI,DWORD PTR SS:[ESP+C]
		//01FB8E3F  |. 85F6           |TEST ESI,ESI
		//01FB3F0B  |. 8B7424 0C      |MOV ESI,DWORD PTR SS:[ESP+C]
		//01FB3F0F  |. 85F6           |TEST ESI,ESI
		//6FC3209B  |. 8B7424 0C      |MOV ESI,DWORD PTR SS:[ESP+C]
		//6FC3209F  |. 85F6           |TEST ESI,ESI
		//6FCF211B  |. 8B7424 0C      |MOV ESI,DWORD PTR SS:[ESP+C]
		//6FCF211F  |. 85F6           |TEST ESI,ESI
		//6FCD301B  |. 8B7424 0C      |MOV ESI,DWORD PTR SS:[ESP+C]
		//6FCD301F  |. 85F6           |TEST ESI,ESI
		//0055CADD  |> 8B7D FC        |MOV EDI,DWORD PTR SS:[EBP-4]
		//0055CAE0  |. 85FF           |TEST EDI,EDI

		log_msg("\n");
	}

	isInstalled = true;
}


/*================================= END OF FILE =================================*/
