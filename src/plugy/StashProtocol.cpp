/*=================================================================
	Stash protocol for scripts (kolbot): a handshake over the 0x3A packet, then requests in the client
	packet PY_STASH_PACKET, each answered by at most one UC_ACK: none when the player has no PlugY data, and the
	game itself drops requests from a dead player. 1.14d only.

=================================================================*/

#include "stashProtocol.h"
#include "sharedStashSession.h"
#include "infinityStash.h"
#include "updateServer.h"
#include "common.h"

bool active_stashProtocol = true;

// the pin codes travel whole from PinnedItems through UC_PROTO to the client player
C_ASSERT(sizeof(((PYStashProto*)0)->pinCodes) == sizeof(pinCodes));
C_ASSERT(sizeof(((PYPlayerData*)0)->serverPinCodes) == sizeof(pinCodes));

// Client packet tables of the 1.14d Game.exe: the parser frames packet id N with the DWORD size at
// 0x730DC0 + N*4 (.data; 0 = unknown id), and the dispatcher at 0x54D750 calls the handler in the first DWORD of
// the 8-byte entry at 0x6E0D18 + N*8 (.rdata) as fastcall (ECX=game, EDX=player, stack: packet, size; callee pops 8),
// only for a living player unit; its caller ignores the result. Both are 0 for PY_STASH_PACKET in the vanilla exe.
#define STASH_PACKET_SIZE_SLOT		(0x330DC0 + PY_STASH_PACKET * 4)
#define STASH_PACKET_HANDLER_SLOT	(0x2E0D18 + PY_STASH_PACKET * 8)

static bool isStashPacketInstalled()
{
	return version_D2Game == V114d
		&& *(DWORD*)(offset_D2Game + STASH_PACKET_SIZE_SLOT) == PY_STASH_PACKET_SIZE
		&& *(DWORD*)(offset_D2Game + STASH_PACKET_HANDLER_SLOT) == (DWORD)handleStashPacket;
}

bool hasStashData(Unit* ptChar)
{
	return active_PlayerCustomData && ptChar && PCPlayerData;
}

// The grid D2InvAddItem checks a stash item against (inventory record 0xC, low resolution), as stashFindSpot uses.
// Returned packed 4 bits per side for the one-byte US_PY_CLIENT_GRID: grids up to 15x15 (6x8, or 10x10 big stash).
static BYTE getStashGrid(BYTE* gridX, BYTE* gridY)
{
	InvGridInfo grid;
	D2GetInventoryGrid(0xC, 0, &grid);
	*gridX = grid.gridX;
	*gridY = grid.gridY;
	return (grid.gridX & 0xF) | ((grid.gridY & 0xF) << 4);
}

static void sendToClient(Unit* ptChar, void* data, DWORD size)
{
	void* ptNetClient = D2GetClient(ptChar, __FILE__, __LINE__);
	if (ptNetClient)
		D2SendPacket(ptNetClient, data, size);
}

/////// server

void sendStashAck(Unit* ptChar, BYTE op, BYTE seq, BYTE status, DWORD gid, BYTE x, BYTE y, Stash* page, DWORD extraPageFlags)
{
	if (version_D2Game != V114d || !ptChar)
		return;
	PYStashAck ack;
	ZeroMemory(&ack, sizeof(ack));
	ack.mType = 0x9D;
	ack.mFunc = UC_ACK;
	ack.mSize = sizeof(ack);
	ack.seq = seq;
	ack.op = op;
	ack.status = status;
	ack.proto = PY_STASH_PROTO;
	BYTE grid = getStashGrid(&ack.gridX, &ack.gridY);
	ack.gid = gid;
	ack.x = x;
	ack.y = y;
	// set only when SELECT can work: a classic character has no stash pages
	if (active_multiPageStash && hasCurrentStash(ptChar))
		ack.features |= PY_FEATURE_MULTIPAGE;
	if (active_sharedStash)
		ack.features |= PY_FEATURE_SHARED;
	if (isStashPacketInstalled())
		ack.features |= PY_FEATURE_PACKET;
	if (serverPinsSwaps(ptChar))
		ack.features |= PY_FEATURE_PINNING;
	if (serverRunsPageOps(ptChar))
		ack.features |= PY_FEATURE_SERVER_PAGE_OPS;
	if (hasStashData(ptChar))
	{
		if (PCPY->sharedStashIsOpened)
			ack.features |= PY_FEATURE_SHARED_OPENED;
		if (PCPY->selfStashIsOpened)
			ack.features |= PY_FEATURE_SELF_OPENED;
		if (PCPY->sharedInConflict)
			ack.features |= PY_FEATURE_SHARED_CONFLICT;
		ack.clientProto = PCPY->clientProto;
		ack.clientGridMatch = PCPY->clientGrid && PCPY->clientGrid == grid;
		if (page)
		{
			ack.page = page->id;
			ack.pageFlags = page->flags & (PY_PAGE_SHARED | PY_PAGE_INDEX | PY_PAGE_MAIN_INDEX);
			if (page == PCPY->currentStash)
				ack.pageFlags |= PY_PAGE_SHOWN;
		}
		ack.nbSelfPages = (WORD)(PCPY->nbSelfPages > 0xFFFF ? 0xFFFF : PCPY->nbSelfPages);
		ack.nbSharedPages = (WORD)(PCPY->nbSharedPages > 0xFFFF ? 0xFFFF : PCPY->nbSharedPages);
	}
	ack.pageFlags |= extraPageFlags;
	log_msg("Stash ACK -> op=%02X seq=%u status=%u page=%u flags=%03X features=%02X client=%u/%u\n",
		ack.op, ack.seq, ack.status, ack.page, ack.pageFlags, ack.features, ack.clientProto, ack.clientGridMatch);
	sendToClient(ptChar, &ack, sizeof(ack));
}

// With ActiveStashProtocol=0 nothing announces the protocol: HELLO still gets its UC_ACK (features without
// PY_FEATURE_PACKET), so a script can tell "PlugY, protocol off" from "no PlugY".
void sendProto(Unit* ptChar)
{
	if (version_D2Game != V114d || !ptChar || !active_stashProtocol || !active_multiPageStash)
		return;
	PYStashProto proto;
	ZeroMemory(&proto, sizeof(proto));
	proto.mType = 0x9D;
	proto.mFunc = UC_PROTO;
	proto.mSize = sizeof(proto);
	proto.proto = PY_STASH_PROTO;
	getStashGrid(&proto.gridX, &proto.gridY);
	// the client pins exactly these on swaps, whatever its own PinnedItems says
	proto.nbPinCodes = (BYTE)nbPinCodes;
	for (DWORD i = 0; i < nbPinCodes && i < MAX_PIN_CODES; i++)
		proto.pinCodes[i] = pinCodes[i];
	proto.flags = PY_PROTO_SERVER_PAGE_OPS;
	if (SharedStashActive(ptChar))
		proto.flags |= PY_PROTO_SHARED_TRANSACTIONS;
	sendToClient(ptChar, &proto, sizeof(proto));
}

void answerStashHello(Unit* ptChar, BYTE seq)
{
	if (version_D2Game != V114d || !ptChar)
		return;
	sendProto(ptChar);
	sendStashAck(ptChar, PY_OP_HELLO, seq, PY_STATUS_OK, 0, 0, 0, hasStashData(ptChar) ? PCPY->currentStash : NULL);
}

void setStashClientProto(Unit* ptChar, BYTE proto)
{
	if (!hasStashData(ptChar))
		return;
	// Never lowered within a game: the client already runs page ops and pinning by what it was told, so a later,
	// lower confirm (a forged packet) would only make the server stop telling it and desync its pages.
	if (proto < PCPY->clientProto)
	{
		log_msg("Stash protocol: client confirm %u ignored, version %u already confirmed\n", proto, PCPY->clientProto);
		return;
	}
	log_msg("Stash protocol: client confirmed version %u\n", proto);
	PCPY->clientProto = proto;
}

void setStashClientGrid(Unit* ptChar, BYTE grid)
{
	if (!hasStashData(ptChar))
		return;
	log_msg("Stash protocol: client stash grid %ux%u\n", grid & 0xF, grid >> 4);
	PCPY->clientGrid = grid;
}

// The announcement alone is not enough: a client that never confirmed it (an older PlugY) changes its own pages before
// asking, and a UC_PAGE_OP would make it change them twice. The confirm comes in order before any later page request.
bool serverRunsPageOps(Unit* ptChar)
{
	return version_D2Game == V114d && active_stashProtocol && active_multiPageStash && hasStashData(ptChar)
		&& PCPY->clientProto >= PY_STASH_PROTO;
}

void sendPageOp(Unit* ptChar, BYTE op, Stash* shown)
{
	if (version_D2Game != V114d || !ptChar || !shown)
		return;
	PYPageOp msg;
	ZeroMemory(&msg, sizeof(msg));
	msg.mType = 0x9D;
	msg.mFunc = UC_PAGE_OP;
	msg.mSize = sizeof(msg);
	msg.op = op;
	msg.shared = shown->isShared ? 1 : 0;
	msg.page = shown->id;
	log_msg("Stash PAGE_OP -> %s on %s page %u\n", op == PY_PAGE_OP_INSERT ? "insert" : "delete", shown->isShared ? "shared" : "personal", shown->id + 1);
	sendToClient(ptChar, &msg, sizeof(msg));
}

// A page switch is mirrored by the client's own PlugY: one that never confirmed the protocol (none, an older one,
// multi-page off) or uses another stash grid would keep showing items the server has moved.
static bool clientCanMirror(Unit* ptChar)
{
	BYTE gridX, gridY;
	return PCPY->clientProto >= PY_STASH_PROTO && PCPY->clientGrid == getStashGrid(&gridX, &gridY);
}

// The page with this id, creating missing pages up to it as a swap does; NULL when it may not be created.
static Stash* getOrCreateStashPage(Unit* ptChar, bool isShared, DWORD page)
{
	Stash* target = getStash(ptChar, isShared, page);
	if (target)
		return target;
	if (!canCreateStashPage(isShared, page))
	{
		log_msg("Stash request refused: %s page %u is out of range\n", isShared ? "shared" : "personal", page);
		return NULL;
	}
	target = isShared ? PCPY->sharedStash : PCPY->selfStash;
	if (!target)
		target = addStash(ptChar, isShared, true, NULL);
	for (DWORD i = 0; i < page; i++)
	{
		if (!target->nextStash)
			addStash(ptChar, isShared, false, target);
		target = target->nextStash;
	}
	return target;
}

void sendItemMoved(Unit* ptChar, DWORD gid, Stash* src, bool srcWasShown, Stash* dst, BYTE x, BYTE y, BYTE reason, BYTE seq)
{
	if (version_D2Game != V114d || !ptChar || !dst)
		return;
	PYItemMoved msg;
	ZeroMemory(&msg, sizeof(msg));
	msg.mType = 0x9D;
	msg.mFunc = UC_ITEM_MOVED;
	msg.mSize = sizeof(msg);
	msg.seq = seq;
	if (src)
	{
		msg.sourceFlags = (src->isShared ? PY_MOVED_SHARED : 0) | (srcWasShown ? PY_MOVED_SRC_SHOWN : 0);
		msg.sourcePage = src->id;
	}
	else
	{
		msg.sourceFlags = PY_MOVED_SRC_UNKNOWN;
		msg.sourcePage = 0xFFFFFFFF;
	}
	msg.targetFlags = dst->isShared ? PY_MOVED_SHARED : 0;
	msg.reason = reason;
	msg.gid = gid;
	msg.x = x;
	msg.y = y;
	msg.targetPage = dst->id;
	log_msg("Stash ITEM_MOVED -> item %u to %s page %u at (%u,%u), reason %u, seq %u\n", gid, dst->isShared ? "shared" : "personal", dst->id + 1, x, y, reason, seq);
	sendToClient(ptChar, &msg, sizeof(msg));
}

#define PY_MOVE_FLAGS (PY_FLAG_SHARED | PY_FLAG_AUTO | PY_FLAG_ALLOW_PINNED | PY_FLAG_CURRENT | PY_FLAG_CROSS | PY_FLAG_ALLOW_SHARED)

// The MOVE itself; *where, *whereX, *whereY: where the item is at the end, left as they are when it is no stash item.
static BYTE moveItem(Unit* ptChar, DWORD gid, DWORD page, BYTE flags, BYTE x, BYTE y, BYTE seq, Stash** where, BYTE* whereX, BYTE* whereY)
{
	// a flag MOVE doesn't use means the script expects other behavior: refuse rather than guess
	if (flags & ~PY_MOVE_FLAGS)
		return PY_STATUS_BAD_REQUEST;
	if (!active_multiPageStash || !hasCurrentStash(ptChar))
		return PY_STATUS_FEATURE_OFF;
	if (!clientCanMirror(ptChar))
	{
		log_msg("Stash MOVE refused: the client confirmed protocol %u, grid %02X\n", PCPY->clientProto, PCPY->clientGrid);
		return PY_STATUS_CLIENT_MISMATCH;
	}

	StashLoc loc;
	Unit* item = findStashItem(ptChar, gid, &loc);
	if (!item)
		return PY_STATUS_NOT_FOUND;
	if (loc.kind == STASHLOC_PHANTOM)
	{
		// the player's own inventory, belt, equipped or cube item; anything else in the chain is a broken link
		if (item->ptItemData->ptInventory == PCInventory)
			return PY_STATUS_NOT_IN_STASH;
		log_msg("ERROR Stash MOVE refused: item %u is in the inventory chain but not held by it (owner %08X, page %u); not moved\n",
			gid, (DWORD)item->ptItemData->ptInventory, D2ItemGetPage(item));
		return PY_STATUS_INTERNAL;
	}
	Stash* source = loc.page;
	bool sourceShown = loc.kind == STASHLOC_CURRENT;
	BYTE fromX = (BYTE)item->path->x;
	BYTE fromY = (BYTE)item->path->y;
	*where = source;
	*whereX = fromX;
	*whereY = fromY;

	// A queued update node makes PlugY's updateItem hook show the item's page at the next flush, and a mode other than
	// stored means a game action holds the item: both pass soon, so the script retries.
	if (item->mode != 0 || D2InvHasUpdateNode(PCInventory, gid))
		return PY_STATUS_BUSY;
	if (isPinnedItem(ptChar, item, 0) && !(flags & PY_FLAG_ALLOW_PINNED))
		return PY_STATUS_PINNED;

	// Only a loaded shared stash is saved, and its conflict path can strand or duplicate an item moved meanwhile.
	bool targetShared = (flags & PY_FLAG_CURRENT) ? PCPY->currentStash->isShared != 0 : (flags & PY_FLAG_SHARED) != 0;
	bool sourceShared = source->isShared != 0;
	if (sourceShared || targetShared)
	{
		if (!(flags & PY_FLAG_ALLOW_SHARED) || !active_sharedStash || !PCPY->sharedStashIsOpened)
			return PY_STATUS_FEATURE_OFF;
		if (PCPY->sharedInConflict)
			return PY_STATUS_SHARED_CONFLICT;
		if (sourceShared != targetShared && !(flags & PY_FLAG_CROSS))
			return PY_STATUS_CROSS_LIST;
	}

	Stash* target = (flags & PY_FLAG_CURRENT) ? PCPY->currentStash : getStash(ptChar, targetShared, page);
	if (!target && !canCreateStashPage(targetShared, page))
	{
		log_msg("Stash request refused: %s page %u is out of range\n", targetShared ? "shared" : "personal", page);
		return PY_STATUS_PAGE_RANGE;
	}
	if (target == source)
	{
		// a retry after a lost UC_ITEM_MOVED lands here, so the answer repeats where the item is
		sendItemMoved(ptChar, gid, source, sourceShown, source, fromX, fromY, PY_MOVED_REPAIR, seq);
		return PY_STATUS_NOOP;
	}
	// A missing page is checked as the empty page it would be, so a refused MOVE creates no page the client never hears of.
	Stash emptyPage;
	ZeroMemory(&emptyPage, sizeof(emptyPage));
	if (!stashFindSpot(ptChar, target ? target : &emptyPage, item, &x, &y, !(flags & PY_FLAG_AUTO)))
		return PY_STATUS_NO_ROOM;
	if (!target)
		target = getOrCreateStashPage(ptChar, targetShared, page);
	const char* listPosition = loc.kind != STASHLOC_LIST ? "on the page shown"
		: loc.prev ? (item->ptItemData->ptNextItem ? "between two items of its list" : "the tail of its list")
		: (item->ptItemData->ptNextItem ? "the head of its list" : "the only item of its list");

	if (!detachItem(ptChar, item, &loc))
		return PY_STATUS_INTERNAL;
	if (!attachItem(ptChar, item, target, x, y))
	{
		// the item is in no list and no grid now: back where it was, else wherever a page switch would put it
		log_msg("ERROR Stash MOVE -> item %u refused at (%u,%u) of %s page %u; putting it back\n", gid, x, y, target->isShared ? "shared" : "personal", target->id + 1);
		item->ptItemData->ptNextItem = NULL;
		item->ptItemData->ptPrevItem = NULL;
		if (attachItem(ptChar, item, source, fromX, fromY))
			return PY_STATUS_INTERNAL;
		relocateStashItem(ptChar, item, source);
		flushPendingMoves(ptChar);
		StashLoc now;
		if (findStashItem(ptChar, gid, &now) && now.page)
		{
			*where = now.page;
			*whereX = (BYTE)item->path->x;
			*whereY = (BYTE)item->path->y;
		}
		return PY_STATUS_INTERNAL;
	}
	*where = target;
	*whereX = x;
	*whereY = y;
	sendItemMoved(ptChar, gid, source, sourceShown, target, x, y, PY_MOVED_REQUEST, seq);
	log_msg("Stash MOVE -> item %u from %s page %u (%u,%u) to %s page %u (%u,%u); it was %s\n", gid, sourceShared ? "shared" : "personal", source->id + 1, fromX, fromY,
		target->isShared ? "shared" : "personal", target->id + 1, x, y, listPosition);
	return PY_STATUS_OK;
}

BYTE moveStashItem(Unit* ptChar, DWORD gid, DWORD page, BYTE flags, BYTE x, BYTE y, BYTE seq)
{
	Stash* where = PCPY->currentStash;
	BYTE whereX = 0, whereY = 0;
	BYTE status = moveItem(ptChar, gid, page, flags, x, y, seq, &where, &whereX, &whereY);
	sendStashAck(ptChar, PY_OP_MOVE, seq, status, gid, whereX, whereY, where);
	return status;
}

// Shows a page by its id, creating missing pages up to it as a swap does. unchanged: the page was already shown.
static BYTE selectStashPage(Unit* ptChar, bool isShared, DWORD page, bool showCurrent, bool* unchanged)
{
	*unchanged = false;
	if (!active_multiPageStash || !hasCurrentStash(ptChar))
		return PY_STATUS_FEATURE_OFF;
	if (showCurrent)
	{
		*unchanged = true;
		return PY_STATUS_OK;
	}
	if (isShared && (!active_sharedStash || !PCPY->sharedStashIsOpened))
		return PY_STATUS_FEATURE_OFF;
	if (!clientCanMirror(ptChar))
	{
		log_msg("Stash SELECT refused: the client confirmed protocol %u, grid %02X\n", PCPY->clientProto, PCPY->clientGrid);
		return PY_STATUS_CLIENT_MISMATCH;
	}
	Stash* target = getOrCreateStashPage(ptChar, isShared, page);
	if (!target)
		return PY_STATUS_PAGE_RANGE;
	if (target == PCPY->currentStash)
	{
		*unchanged = true;
		return PY_STATUS_OK;
	}
	PCPY->showSharedStash = isShared;
	selectStash(ptChar, target, false);
	// relocations made by this switch go to the client before the ACK, whatever selectStash itself flushes
	flushPendingMoves(ptChar);
	return PY_STATUS_OK;
}

// The items saveStash writes for a page: the page shown holds them in the inventory chain, where only owned page-4
// items are stash items; any other page in its own list.
static Unit* firstPageItem(Unit* ptChar, Stash* page)
{
	return page == PCPY->currentStash ? D2InventoryGetFirstItem(PCInventory) : page->ptListItem;
}

static bool isPageItem(Unit* ptChar, Stash* page, Unit* item)
{
	return D2ItemGetPage(item) == 4 && (page != PCPY->currentStash || item->ptItemData->ptInventory == PCInventory);
}

static DWORD countPageItems(Unit* ptChar, Stash* page)
{
	DWORD nbItems = 0;
	for (Unit* item = firstPageItem(ptChar, page); item; item = D2UnitGetNextItem(item))
		if (isPageItem(ptChar, page, item))
			nbItems++;
	return nbItems;
}

// One page as UC_STASH_PAGE chunks of at most PY_PAGE_ITEMS_PER_CHUNK items; an empty page is one chunk.
static void sendStashPage(Unit* ptChar, Stash* page, DWORD nbItems, BYTE seq)
{
	DWORD nbChunks = nbItems ? (nbItems + PY_PAGE_ITEMS_PER_CHUNK - 1) / PY_PAGE_ITEMS_PER_CHUNK : 1;
	if (nbChunks > 0xFF)
	{
		// a page holds at most one item per cell (225 on a 15x15 grid), so only a broken page gets here
		log_msg("ERROR Stash LIST -> %s page %u holds %u items; only the first %u are listed\n", page->isShared ? "shared" : "personal",
			page->id + 1, nbItems, 0xFF * PY_PAGE_ITEMS_PER_CHUNK);
		nbChunks = 0xFF;
	}
	PYStashPage msg;
	ZeroMemory(&msg, sizeof(msg));
	msg.mType = 0x9D;
	msg.mFunc = UC_STASH_PAGE;
	msg.seq = seq;
	msg.page = page->id;
	msg.pageFlags = page->flags & (PY_PAGE_SHARED | PY_PAGE_INDEX | PY_PAGE_MAIN_INDEX);
	if (page == PCPY->currentStash)
		msg.pageFlags |= PY_PAGE_SHOWN;
	msg.chunkCount = (BYTE)nbChunks;
	if (page->name)
		strncpy(msg.name, page->name, sizeof(msg.name));
	Unit* item = firstPageItem(ptChar, page);
	for (DWORD chunk = 0; chunk < nbChunks; chunk++)
	{
		msg.chunkIndex = (BYTE)chunk;
		msg.nbItems = 0;
		for (; item && msg.nbItems < PY_PAGE_ITEMS_PER_CHUNK; item = D2UnitGetNextItem(item))
		{
			if (!isPageItem(ptChar, page, item))
				continue;
			PYStashPageItem* record = &msg.items[msg.nbItems++];
			record->gid = item->nItemNum;
			record->classid = (WORD)item->nTxtFileNo;
			record->x = (BYTE)item->path->x;
			record->y = (BYTE)item->path->y;
		}
		msg.mSize = (BYTE)(FIELD_OFFSET(PYStashPage, items) + msg.nbItems * sizeof(PYStashPageItem));
		sendToClient(ptChar, &msg, msg.mSize);
	}
}

// LIST (PY_OP_LIST): the pages of one list from page id first on, in id order, at most maxPages of them (0: no limit),
// ending at the last page with items unless PY_FLAG_WITH_EMPTY: page ids stay contiguous, so an empty page before it
// (an index page, say) is sent with no items. Read-only, so the client's PlugY need not mirror anything. Every page is
// at least one message sent at once, so a list of many pages is many packets: a script asks for fewer.
static BYTE listStashPages(Unit* ptChar, BYTE flags, DWORD first, WORD maxPages, BYTE seq, DWORD* nbPagesSent)
{
	*nbPagesSent = 0;
	// a flag LIST doesn't use means the script expects other behavior: refuse rather than guess
	if (flags & ~(PY_FLAG_SHARED | PY_FLAG_WITH_EMPTY))
		return PY_STATUS_BAD_REQUEST;
	if (!active_multiPageStash || !hasCurrentStash(ptChar))
		return PY_STATUS_FEATURE_OFF;
	bool isShared = (flags & PY_FLAG_SHARED) != 0;
	if (isShared && (!active_sharedStash || !PCPY->sharedStashIsOpened))
		return PY_STATUS_FEATURE_OFF;
	Stash* firstPage = isShared ? PCPY->sharedStash : PCPY->selfStash;
	Stash* lastPage = NULL;
	for (Stash* page = firstPage; page; page = page->nextStash)
		if (page->id >= first && ((flags & PY_FLAG_WITH_EMPTY) || countPageItems(ptChar, page)))
			lastPage = page;
	DWORD nbItemsSent = 0;
	for (Stash* page = firstPage; lastPage && page && (!maxPages || *nbPagesSent < maxPages); page = page->nextStash)
	{
		if (page->id < first)
			continue;
		DWORD nbItems = countPageItems(ptChar, page);
		sendStashPage(ptChar, page, nbItems, seq);
		(*nbPagesSent)++;
		nbItemsSent += nbItems;
		if (page == lastPage)
			break;
	}
	log_msg("Stash LIST -> %u %s pages from page %u, %u items\n", *nbPagesSent, isShared ? "shared" : "personal", first + 1, nbItemsSent);
	return PY_STATUS_OK;
}

DWORD FASTCALL handleStashPacket(Game* ptGame, Unit* ptChar, BYTE* packet, DWORD size)
{
	// the parser frames this id at PY_STASH_PACKET_SIZE, so another size is a framing bug: 3 reports a bad packet
	if (size != PY_STASH_PACKET_SIZE)
		return 3;
	if (!hasStashData(ptChar))
		return 0;
	BYTE op = packet[1];
	BYTE seq = packet[2];
	BYTE flags = packet[3];
	DWORD page = *(DWORD*)(packet + 8);
	log_msg("Stash request: op=%02X seq=%u flags=%02X gid=%u page=%u\n", op, seq, flags, *(DWORD*)(packet + 4), page);
	if (flags & PY_FLAG_RESERVED)
	{
		sendStashAck(ptChar, op, seq, PY_STATUS_BAD_REQUEST, 0, 0, 0, PCPY->currentStash);
		return 0;
	}
	switch (op)
	{
	case PY_OP_SELECT:
	{
		// a flag SELECT doesn't use means the script expects other behavior: refuse rather than guess
		if (flags & ~(PY_FLAG_SHARED | PY_FLAG_CURRENT))
		{
			sendStashAck(ptChar, op, seq, PY_STATUS_BAD_REQUEST, 0, 0, 0, PCPY->currentStash);
			return 0;
		}
		bool unchanged;
		BYTE status = selectStashPage(ptChar, (flags & PY_FLAG_SHARED) != 0, page, (flags & PY_FLAG_CURRENT) != 0, &unchanged);
		sendStashAck(ptChar, op, seq, status, 0, 0, 0, PCPY->currentStash, unchanged ? PY_PAGE_UNCHANGED : 0);
		return 0;
	}
	case PY_OP_MOVE:
		moveStashItem(ptChar, *(DWORD*)(packet + 4), page, flags, packet[12], packet[13], seq);
		return 0;
	case PY_OP_LIST:
	{
		DWORD nbPagesSent;
		BYTE status = listStashPages(ptChar, flags, page, *(WORD*)(packet + 14), seq, &nbPagesSent);
		sendStashAck(ptChar, op, seq, status, nbPagesSent, 0, 0, PCPY->currentStash);
		return 0;
	}
	default:
		sendStashAck(ptChar, op, seq, PY_STATUS_BAD_REQUEST, 0, 0, 0, PCPY->currentStash);
		return 0;
	}
}

/////// client

void applyProtoClient(const BYTE* msg)
{
	if (version_D2Common != V114d)
		return;
	const PYStashProto* proto = (const PYStashProto*)msg;
	Unit* ptChar = D2GetClientPlayer();
	BYTE nbCodes = proto->nbPinCodes < MAX_PIN_CODES ? proto->nbPinCodes : MAX_PIN_CODES;
	BYTE gridX, gridY;
	BYTE grid = getStashGrid(&gridX, &gridY);
	// Without player data this client holds no pages and no pin codes, so it confirms nothing: the server then keeps
	// page requests and pinning off for it rather than change items this client can't mirror.
	if (!hasStashData(ptChar))
	{
		log_msg("[CLIENT] Stash protocol %u announced, not confirmed: no PlugY player data here\n", proto->proto);
		return;
	}
	PCPY->serverProto = proto->proto;
	PCPY->serverProtoFlags = proto->flags;
	// replaced, never merged: the server's latest list is the rule, an empty one included
	ZeroMemory(PCPY->serverPinCodes, sizeof(PCPY->serverPinCodes));
	for (BYTE i = 0; i < nbCodes; i++)
		PCPY->serverPinCodes[i] = proto->pinCodes[i];
	PCPY->nbServerPinCodes = nbCodes;
	log_msg("[CLIENT] Stash protocol %u announced, flags %08X, server grid %ux%u, ours %ux%u, %u pinned item codes\n", proto->proto, proto->flags, proto->gridX, proto->gridY, gridX, gridY, nbCodes);
	updateServer(US_PY_CLIENT_HELLO | (PY_STASH_PROTO << 8));
	updateServer(US_PY_CLIENT_GRID | (grid << 8));
}

static void countClientDesync(Unit* ptChar, DWORD gid, const char* why)
{
	PCPY->clientDesync++;
	log_msg("[CLIENT] Stash ITEM_MOVED -> item %u %s; the client view differs from the server until the next join (desync %u)\n", gid, why, PCPY->clientDesync);
}

// The server decided where the item is: the client puts its own copy there, never checking the fit or choosing a
// spot. A copy it can't put there stays detached (in no list, still in the client unit table, freed by the teardown).
void applyItemMovedClient(const BYTE* msg)
{
	if (version_D2Client != V114d)
		return;
	const PYItemMoved* moved = (const PYItemMoved*)msg;
	Unit* ptChar = D2GetClientPlayer();
	if (!hasStashData(ptChar))
	{
		log_msg("[CLIENT] Stash ITEM_MOVED -> item %u ignored: no PlugY player data here\n", moved->gid);
		return;
	}
	if (!PCPY->currentStash)
	{
		countClientDesync(ptChar, moved->gid, "can't be placed: no stash pages here");
		return;
	}
	bool isShared = (moved->targetFlags & PY_MOVED_SHARED) != 0;
	log_msg("[CLIENT] Stash ITEM_MOVED -> item %u to %s page %u at (%u,%u), reason %u\n", moved->gid, isShared ? "shared" : "personal", moved->targetPage + 1, moved->x, moved->y, moved->reason);

	StashLoc loc;
	Unit* item = findStashItem(ptChar, moved->gid, &loc);
	if (item && loc.kind == STASHLOC_PHANTOM)
	{
		countClientDesync(ptChar, moved->gid, "is in the inventory chain but no stash item of the page shown");
		return;
	}
	if (!item)
	{
		// In no PlugY list and not in the chain: a copy this client's own page switch left detached. Taken only while
		// stored (mode 0, as a switch leaves it) and nothing holds or links it, so attaching it can't steal it from an
		// inventory, a list or the ground (D2InvAddItem runs a separate path for a mode-3, on-ground, item).
		item = D2FindClientUnit(moved->gid, UNIT_ITEM);
		if (!item || item->nUnitType != UNIT_ITEM || !item->ptItemData || item->mode != 0 || item->ptItemData->ptInventory
			|| item->ptItemData->ptNextItem || item->ptItemData->ptPrevItem)
		{
			countClientDesync(ptChar, moved->gid, item ? "is held or linked elsewhere" : "is unknown here");
			return;
		}
	}

	Stash* target = getStash(ptChar, isShared, moved->targetPage);
	if (target && loc.page == target && item->path->x == moved->x && item->path->y == moved->y)
		return;	// already there: a repair of a mirror that wasn't lost
	if (!target) do
		target = addStash(ptChar, isShared, false, target);
	while (target->id < moved->targetPage);

	if (loc.page && !detachItem(ptChar, item, &loc))
	{
		countClientDesync(ptChar, moved->gid, "can't leave its page");
		return;
	}
	if (!attachItem(ptChar, item, target, moved->x, moved->y))
		countClientDesync(ptChar, moved->gid, "doesn't fit where the server put it; left detached");
}

bool clientAwaitsPageOps(Unit* ptChar)
{
	return version_D2Client == V114d && hasStashData(ptChar) && (PCPY->serverProtoFlags & PY_PROTO_SERVER_PAGE_OPS);
}

// The server already changed its list; the client runs the same code on the same state. Only on the page the server
// names: another page shown means the views already differ, and shifting that page would widen the difference.
void applyPageOpClient(const BYTE* msg)
{
	if (version_D2Client != V114d)
		return;
	const PYPageOp* pageOp = (const PYPageOp*)msg;
	Unit* ptChar = D2GetClientPlayer();
	if (!hasStashData(ptChar))
	{
		log_msg("[CLIENT] Stash PAGE_OP -> ignored: no PlugY player data here\n");
		return;
	}
	const char* list = pageOp->shared ? "shared" : "personal";
	// a client that wasn't told changed its own pages before asking: running the op again would shift them twice
	if (!clientAwaitsPageOps(ptChar))
	{
		log_msg("[CLIENT] Stash PAGE_OP -> op %u on %s page %u ignored: the server never announced page ops\n", pageOp->op, list, pageOp->page + 1);
		return;
	}
	Stash* shown = PCPY->currentStash;
	if (!shown || shown->id != pageOp->page || (shown->isShared != 0) != (pageOp->shared != 0))
	{
		PCPY->clientDesync++;
		log_msg("[CLIENT] Stash PAGE_OP -> op %u on %s page %u not run: another page is shown here; the client view differs from the server until the next join (desync %u)\n",
			pageOp->op, list, pageOp->page + 1, PCPY->clientDesync);
		return;
	}
	log_msg("[CLIENT] Stash PAGE_OP -> op %u on %s page %u\n", pageOp->op, list, pageOp->page + 1);
	if (pageOp->op == PY_PAGE_OP_INSERT)
		insertStash(ptChar);
	else if (pageOp->op == PY_PAGE_OP_DELETE)
	{
		if (!deleteStash(ptChar, true))
		{
			PCPY->clientDesync++;
			log_msg("[CLIENT] Stash PAGE_OP -> delete refused here: the page shown holds items; the client view differs from the server until the next join (desync %u)\n", PCPY->clientDesync);
		}
	}
	else
		log_msg("[CLIENT] Stash PAGE_OP -> unknown op %u ignored\n", pageOp->op);
}

/////// install

void Install_StashProtocol()
{
	static int isInstalled = false;
	if (isInstalled) return;
	isInstalled = true;
	if (version_D2Game != V114d || !active_stashProtocol)
		return;

	log_msg("Patch D2Game for the stash request packet %02X. (StashProtocol)\n", PY_STASH_PACKET);
	DWORD* sizeSlot = (DWORD*)(offset_D2Game + STASH_PACKET_SIZE_SLOT);
	DWORD* handlerSlot = (DWORD*)(offset_D2Game + STASH_PACKET_HANDLER_SLOT);
	if (*sizeSlot || *handlerSlot)
	{
		log_msg("Stash protocol not installed: packet %02X is already in use (size %u, handler %08X)\n\n", PY_STASH_PACKET, *sizeSlot, *handlerSlot);
		return;
	}
	// The handler goes in first: once the size is set the parser frames the packet, and a framed id whose handler
	// is still 0 is a bad packet for the dispatcher.
	DWORD oldProtection;
	if (!VirtualProtect(handlerSlot, 4, PAGE_READWRITE, &oldProtection))
	{
		log_msg("Stash protocol not installed: the handler table can't be made writable (error %u)\n\n", GetLastError());
		return;
	}
	mem_seek((DWORD)handlerSlot);
	MEMT_DWORD(0x00000000, handleStashPacket);
	VirtualProtect(handlerSlot, 4, oldProtection, &oldProtection);
	mem_seek((DWORD)sizeSlot);
	memt_dword(0x00000000, PY_STASH_PACKET_SIZE);
	log_msg("\n");
}

/*================================= END OF FILE =================================*/
