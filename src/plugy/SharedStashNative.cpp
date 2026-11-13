#include "SharedStashNative.h"
#include "infinityStash.h"
#include "sharedStashTransport.h"
#include "stashProtocol.h"
#include "sharedStashItemPacketRules.h"
#include <map>
#include <vector>
#include <string.h>

namespace {
typedef DWORD (FASTCALL *PacketHandler)(Game*, Unit*, BYTE*, DWORD);
typedef void (FASTCALL *FreeItem)(Game*, Unit*);
typedef void (FASTCALL *SendStorageItem)(void*, Unit*, DWORD, DWORD, DWORD);
typedef Unit* (STDCALL *GetCursorItem)(Inventory*);
typedef DWORD (STDCALL *GetInteraction)(Unit*, DWORD*, DWORD*);
typedef void (STDCALL *ClearInteraction)(Unit*);
PacketHandler originalHandlers[0x67] = {};
SharedNativeBeforeAction beforeAction = NULL;
SharedNativeAfterAction afterAction = NULL;
SharedNativeBeforeSend beforeSend = NULL;
__declspec(thread) bool runningBeforeSend = false;
bool installed = false;
DWORD sendTrampoline = 0;

struct HeldPackets {
	std::vector<SharedStash::Bytes> packets;
	bool failed;
	HeldPackets() : failed(false) {}
};
std::map<void*, HeldPackets> held;
CRITICAL_SECTION heldLock;

struct LoadContext {
	Unit* character;
	Stash* page;
	Unit* root;
	DWORD gid;
	BYTE x, y, storage;
	bool failed;
};
__declspec(thread) LoadContext* loading = NULL;

void* clientOf(Unit* ptChar)
{
	return ptChar ? D2GetClient(ptChar, __FILE__, __LINE__) : NULL;
}

DWORD FASTCALL dispatch(Game* game, Unit* ptChar, BYTE* packet, DWORD size)
{
	if (!packet || !size || packet[0] >= 0x67 || !originalHandlers[packet[0]]) return 3;
	if (sharedStashClosePacket(packet, size)) return originalHandlers[packet[0]](game, ptChar, packet, size);
	try {
		if (beforeAction && !beforeAction(game, ptChar, packet, size)) return 0;
	} catch (...) {
		log_msg("Shared stash: native action refused after a pre-action exception.\n");
		return 0;
	}
	DWORD result = originalHandlers[packet[0]](game, ptChar, packet, size);
	try {
		if (afterAction) afterAction(ptChar);
	} catch (...) {
		// The pre-action reservation and notification hold remain owned by the session.
		log_msg("Shared stash: native action finalization raised an exception; ownership remains reserved.\n");
	}
	return result;
}

// Native SCmd send is EDI=NetClient, stack=(packet,size), callee pops 8.
DWORD STDCALL captureSend(void* client, const BYTE* data, DWORD size)
{
	// Leaving the game must not wait for a shared-stash commit.
	if (sharedStashExitPacket(data, size)) return 0;
	DWORD captured = 0;
	// Passive item timers can send without an action handler. Let the session establish ownership first.
	EnterCriticalSection(&heldLock);
	bool alreadyHeld = held.find(client) != held.end();
	LeaveCriticalSection(&heldLock);
	if (!alreadyHeld && !runningBeforeSend && beforeSend) {
		runningBeforeSend = true;
		bool permit = false;
		try { permit = beforeSend(client, data, size); }
		catch (...) { log_msg("Shared stash: passive native output refused after an exception.\n"); }
		runningBeforeSend = false;
		if (!permit) return 1;
	}
	EnterCriticalSection(&heldLock);
	std::map<void*, HeldPackets>::iterator i = held.find(client);
	if (i != held.end()) {
		captured = 1;
		try {
			if (!data || !size || size > 65535) i->second.failed = true;
			else i->second.packets.push_back(SharedStash::Bytes(data, data + size));
		} catch (...) { i->second.failed = true; }
	}
	LeaveCriticalSection(&heldLock);
	return captured;
}

FCT_ASM(sendHook)
	PUSHFD
	PUSHAD
	PUSH DWORD PTR SS:[ESP+44]
	PUSH DWORD PTR SS:[ESP+44]
	PUSH EDI
	CALL captureSend
	TEST EAX,EAX
	JNZ heldPacket
	POPAD
	POPFD
	JMP sendTrampoline
heldPacket:
	POPAD
	POPFD
	MOV EAX,DWORD PTR SS:[ESP+8]
	RETN 8
}}

// Bypass the capture hook when releasing a completed operation's exact bytes.
void sendRaw(void* client, const BYTE* packetBytes, DWORD packetSize)
{
	__asm {
		PUSH EDI
		MOV EDI,client
		PUSH packetSize
		PUSH packetBytes
		CALL sendTrampoline
		POP EDI
	}
}

bool patchSend()
{
	BYTE* entry = (BYTE*)(offset_D2Game + 0x13B280);
	const BYTE expected[6] = {0x55, 0x8B, 0xEC, 0x53, 0x33, 0xDB};
	if (memcmp(entry, expected, sizeof(expected))) return false;
	BYTE* trampoline = (BYTE*)VirtualAlloc(NULL, 11, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!trampoline) return false;
	memcpy(trampoline, entry, 6);
	trampoline[6] = 0xE9;
	*(DWORD*)(trampoline + 7) = (DWORD)(entry + 6) - (DWORD)(trampoline + 11);
	DWORD old;
	if (!VirtualProtect(trampoline, 11, PAGE_EXECUTE_READ, &old)) { VirtualFree(trampoline, 0, MEM_RELEASE); return false; }
	if (!VirtualProtect(entry, 6, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(trampoline, 0, MEM_RELEASE); return false; }
	sendTrampoline = (DWORD)trampoline;
	entry[0] = 0xE9;
	*(DWORD*)(entry + 1) = (DWORD)sendHook - (DWORD)(entry + 5);
	entry[5] = 0x90;
	DWORD ignored;
	VirtualProtect(entry, 6, old, &ignored);
	FlushInstructionCache(GetCurrentProcess(), entry, 6);
	FlushInstructionCache(GetCurrentProcess(), trampoline, 11);
	return true;
}
}

bool Install_SharedStashNative(SharedNativeBeforeAction before, SharedNativeAfterAction after)
{
	if (installed) return before == beforeAction && after == afterAction;
	if (version_D2Game != V114d || !before || !after) return false;
	DWORD* handlers = (DWORD*)(offset_D2Game + 0x2E0D18);
	DWORD old;
	if (!VirtualProtect(handlers, 0x67 * 8, PAGE_READWRITE, &old)) return false;
	InitializeCriticalSection(&heldLock);
	if (!patchSend()) {
		DWORD ignored;
		VirtualProtect(handlers, 0x67 * 8, old, &ignored);
		DeleteCriticalSection(&heldLock);
		return false;
	}
	beforeAction = before;
	afterAction = after;
	for (DWORD id = 0; id < 0x67; ++id) {
		if (!sharedStashActionPacket((BYTE)id)) continue;
		if (!handlers[id * 2]) continue;
		originalHandlers[id] = (PacketHandler)handlers[id * 2];
		handlers[id * 2] = (DWORD)dispatch;
	}
	DWORD ignored;
	VirtualProtect(handlers, 0x67 * 8, old, &ignored);
	installed = true;
	return true;
}

bool SharedNativeReady() { return installed; }

bool SharedNativeWrapsStashPacket()
{
	return installed && version_D2Game == V114d
		&& originalHandlers[PY_STASH_PACKET] == handleStashPacket
		&& *(DWORD*)(offset_D2Game + 0x2E0D18 + PY_STASH_PACKET * 8) == (DWORD)dispatch;
}

void SharedNativeSetBeforeSend(SharedNativeBeforeSend callback) { beforeSend = callback; }

bool SharedNativeHoldNotifications(Unit* ptChar)
{
	void* client = installed ? clientOf(ptChar) : NULL;
	if (!client) return false;
	bool ok = true;
	EnterCriticalSection(&heldLock);
	try { held[client]; } catch (...) { ok = false; }
	LeaveCriticalSection(&heldLock);
	return ok;
}

bool SharedNativeReleaseNotifications(Unit* ptChar)
{
	void* client = installed ? clientOf(ptChar) : NULL;
	if (!client) return false;
	EnterCriticalSection(&heldLock);
	std::map<void*, HeldPackets>::iterator i = held.find(client);
	if (i == held.end()) { LeaveCriticalSection(&heldLock); return true; }
	if (i->second.failed) { LeaveCriticalSection(&heldLock); return false; }
	// Send while holding the lock so a concurrent sender cannot overtake the retained packets.
	for (size_t n = 0; n < i->second.packets.size(); ++n)
		sendRaw(client, &i->second.packets[n][0], (DWORD)i->second.packets[n].size());
	held.erase(i);
	LeaveCriticalSection(&heldLock);
	return true;
}

void SharedNativeDiscardNotifications(Unit* ptChar)
{
	void* client = installed ? clientOf(ptChar) : NULL;
	if (!client) return;
	EnterCriticalSection(&heldLock);
	held.erase(client);
	LeaveCriticalSection(&heldLock);
}

bool SharedNativeFilterNotifications(Unit* ptChar, SharedNativePacketFilter keep, void* context)
{
	void* client = installed ? clientOf(ptChar) : NULL;
	if (!client || !keep) return false;
	bool ok = true;
	EnterCriticalSection(&heldLock);
	std::map<void*, HeldPackets>::iterator i = held.find(client);
	if (i != held.end()) {
		try {
			std::vector<SharedStash::Bytes> filtered;
			filtered.reserve(i->second.packets.size());
			for (size_t n = 0; n < i->second.packets.size(); ++n) {
				const SharedStash::Bytes& packet = i->second.packets[n];
				if (keep(&packet[0], (DWORD)packet.size(), context)) filtered.push_back(packet);
			}
			i->second.packets.swap(filtered);
		} catch (...) { i->second.failed = true; ok = false; }
	}
	LeaveCriticalSection(&heldLock);
	return ok;
}

bool SharedNativePlaceLoadedItem(Game* game, Unit* ptChar, Unit* item, Unit* parent, DWORD* result)
{
	LoadContext* context = loading;
	if (!context || context->character != ptChar) return false;
	if (parent) {
		*result = D2LoadPlaceItem(game, ptChar, item, parent);
		if (*result) context->failed = true;
		return true;
	}
	if (context->root || !item || !item->path || !item->ptItemData) {
		context->failed = true;
		*result = 0xC;
		return true;
	}
	if (context->storage == 3) {
		*result = SharedNativeAttachCubeItem(ptChar, item, context->x, context->y) ? 0 : 0xC;
	} else {
		item->path->x = context->x;
		item->path->y = context->y;
		D2ItemSetPage(item, 4);
		item->mode = 0;
		BYTE x = context->x, y = context->y;
		*result = stashFindSpot(ptChar, context->page, item, &x, &y, true)
			&& attachItem(ptChar, item, context->page, x, y) ? 0 : 0xC;
	}
	if (*result) context->failed = true;
	else { context->root = item; context->gid = item->nItemNum; }
	return true;
}

Unit* SharedNativeLoadItem(Unit* ptChar, const SharedStash::Bytes& data, Stash* target, BYTE x, BYTE y, BYTE storagePage)
{
	if (!installed || !ptChar || !PCPlayerData || !PCGame || loading || data.empty() || data.size() > 0x2000
		|| (storagePage != 3 && storagePage != 4) || (storagePage == 4 && !target)) return NULL;
	try {
		SharedStash::Bytes inventory(4 + data.size());
		inventory[0] = 'J'; inventory[1] = 'M'; inventory[2] = 1; inventory[3] = 0;
		memcpy(&inventory[4], &data[0], data.size());
		LoadContext context = {ptChar, target, NULL, 0, x, y, storagePage, false};
		loading = &context;
		DWORD read = 0;
		DWORD result = D2LoadInventory(PCGame, ptChar, (saveBitField*)&inventory[0], 0x60, (DWORD)inventory.size(), 0, &read);
		loading = NULL;
		if (result || context.failed || read != inventory.size() || !context.root) {
			if (context.root && D2GameGetObject(PCGame, UNIT_ITEM, context.gid) == context.root)
				SharedNativeRemoveItem(ptChar, context.root, false);
			return NULL;
		}
		return context.root;
	} catch (...) { loading = NULL; return NULL; }
}

bool SharedNativeRemoveItem(Unit* ptChar, Unit* item, bool notify)
{
	if (!installed || !ptChar || !PCPlayerData || !item || item->nUnitType != UNIT_ITEM || !item->ptItemData) return false;
	if (notify && (!SharedTransportReady() || !clientOf(ptChar))) return false;
	StashLoc where;
	Unit* found = findStashItem(ptChar, item->nItemNum, &where);
	if (found == item && where.kind != STASHLOC_PHANTOM) {
		if (!detachItem(ptChar, item, &where)) return false;
	} else if (item->ptItemData->ptInventory == PCInventory && D2ItemGetPage(item) == 3) {
		if (!D2InvRemoveItem(PCInventory, item)) return false;
	} else return false;
	if (notify) {
		SharedTransportSendDetach(ptChar, item->nItemNum);
		BYTE packet[6] = {0x0A, UNIT_ITEM, 0, 0, 0, 0};
		memcpy(packet + 2, &item->nItemNum, 4);
		D2SendPacket(clientOf(ptChar), packet, sizeof(packet));
	}
	((FreeItem)(offset_D2Game + 0x157FD0))(PCGame, item);
	return true;
}

bool SharedNativeSendItem(Unit* ptChar, Unit* item, Stash* page)
{
	if (!installed || !ptChar || !item || !item->ptItemData || !clientOf(ptChar)) return false;
	Stash* home = PCPY->currentStash;
	if (page && page != home) selectStash(ptChar, page);
	// Native 0x9D rejects action 4. The 0x9C builder handles storage placement and derives owner from NetClient.
	((SendStorageItem)(offset_D2Game + 0x13EAE0))(clientOf(ptChar), item, 4, 0, 0);
	if (page && page != home) selectStash(ptChar, home);
	return SharedNativeSendCursor(ptChar, false);
}

bool SharedNativeSendCursor(Unit* ptChar, bool clearIfEmpty)
{
	if (!installed || !ptChar || !PCPlayerData || !PCInventory || !clientOf(ptChar)) return false;
	Unit* cursor = ((GetCursorItem)(offset_D2Common + 0x23C1E0))(PCInventory);
	if (cursor) {
		// Client 0x9C/18 recreates this exact gid from authoritative bytes, then restores cursor ownership and icon.
		((SendStorageItem)(offset_D2Game + 0x13EAE0))(clientOf(ptChar), cursor, 0x12, 0, 0);
	} else if (clearIfEmpty) {
		// Client 0x42 targets the owning player and clears any optimistic cursor item.
		BYTE packet[6] = {0x42, UNIT_PLAYER, 0, 0, 0, 0};
		memcpy(packet + 2, &ptChar->nItemNum, 4);
		D2SendPacket(clientOf(ptChar), packet, sizeof(packet));
	}
	return true;
}

bool SharedNativeCloseCube(Unit* ptChar)
{
	if (!installed || !ptChar || !PCPlayerData || !clientOf(ptChar)) return false;
	DWORD type = 6, gid = 0;
	if (((GetInteraction)(offset_D2Game + 0x154100))(ptChar, &type, &gid) && type == UNIT_ITEM)
		((ClearInteraction)(offset_D2Game + 0x154190))(ptChar);
	return SharedTransportSendCloseCube(ptChar);
}

void SharedNativeCloseClientCube()
{
	if (version_D2Client != V114d) return;
	typedef DWORD (FASTCALL *GetUiFlag)(DWORD);
	if (!((GetUiFlag)(offset_D2Client + 0x53A90))(0x1A)) return;
	// D2TogglePage's 0x1A branch opens/closes the supertransmogrifier panel; operation 1 means close.
	D2TogglePage(0x1A, 1, 0);
}

bool SharedNativeAttachCubeItem(Unit* ptChar, Unit* item, BYTE x, BYTE y)
{
	if (!installed || !ptChar || !PCPlayerData || !PCGame || !item || !item->path || !item->ptItemData || item->ptItemData->ptInventory
		|| item->ptItemData->ptNextItem || item->ptItemData->ptPrevItem) return false;
	// 1.14d's player cube uses record 9. Low-level placement does not queue game updates.
	item->path->x = x; item->path->y = y;
	D2ItemSetPage(item, 3);
	item->mode = 0;
	return D2InvAddItem(PCInventory, item, x, y, 9, 0, 3) != 0;
}

#ifdef PLUGY_TEST_HOOKS
Unit* SharedNativeCreateTestItem(Unit* ptChar, DWORD itemCode, DWORD quantity, DWORD sockets)
{
	if (!installed || !ptChar || !PCPlayerData || !PCGame || !PCInventory || sockets > 6
		|| ((GetCursorItem)(offset_D2Common + 0x23C1E0))(PCInventory)) return NULL;
	DWORD classId = 0;
	for (;;) {
		ItemsBIN* definition = D2GetItemsBIN(classId);
		if (!definition) return NULL;
		if (definition->ItemCode == itemCode) break;
		++classId;
	}
	C_ASSERT(sizeof(ItemGenerationData) == 0x84);
	ItemGenerationData parameters = {};
	parameters.ptMonster = ptChar;
	parameters.ptGame = PCGame;
	parameters.ilvl = 1;
	parameters.nTxtFileNo = classId;
	parameters.unknow1 = 4;
	parameters.unknow2 = 1;
	parameters.gameUnknown2 = *(WORD*)((BYTE*)PCGame + 0x78);
	parameters.quality = ITEMQUALITY_NORMAL;
	parameters.flags = 0x0A;
	for (int i = 0; i < 3; ++i) parameters.prefixID[i] = parameters.suffixID[i] = -1;
	typedef Unit* (FASTCALL *CreateItem)(Game*, ItemGenerationData*, DWORD);
	Unit* item = ((CreateItem)(offset_D2Game + 0x158D90))(PCGame, &parameters, 0);
	if (!item) return NULL;
	if (quantity) {
		typedef DWORD (STDCALL *GetMaxStack)(Unit*);
		DWORD maximum = ((GetMaxStack)(offset_D2Common + 0x2295B0))(item);
		if (!maximum || quantity > maximum) { ((FreeItem)(offset_D2Game + 0x157FD0))(PCGame, item); return NULL; }
		typedef void (STDCALL *SetStat)(Unit*, DWORD, DWORD, DWORD);
		((SetStat)(offset_D2Common + 0x227260))(item, STATS_QUANTITY, quantity, 0);
	}
	if (sockets) {
		typedef void (STDCALL *SetSockets)(Unit*, DWORD);
		((SetSockets)(offset_D2Common + 0x22BCB0))(item, sockets);
		if (D2GetPlayerStat(item, STATS_ITEM_NUMSOCKETS, 0) != sockets) { ((FreeItem)(offset_D2Game + 0x157FD0))(PCGame, item); return NULL; }
	}
	typedef void (STDCALL *SetCursor)(Inventory*, Unit*);
	((SetCursor)(offset_D2Common + 0x23C180))(PCInventory, item);
	SharedNativeSendCursor(ptChar);
	return item;
}
#endif
