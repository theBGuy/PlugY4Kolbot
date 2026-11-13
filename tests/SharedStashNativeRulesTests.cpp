// Exercise the actual wrapper proof without installing patches in a game.
#include "../src/plugy/SharedStashNative.cpp"
#include <stdio.h>

int version_D2Game = V114d;
DWORD offset_D2Game = 0;
DWORD offset_D2Common = 0, offset_D2Client = 0;
int version_D2Client = 0;
s_shifting shifting = {};
TD2ItemGetPage D2ItemGetPage = NULL;
TD2LoadInventory D2LoadInventory = NULL;
TD2SendPacket D2SendPacket = NULL;
TD2TogglePage D2TogglePage = NULL;
TD2LoadPlaceItem D2LoadPlaceItem = NULL;
TD2InvAddItem D2InvAddItem = NULL;
TD2GetClient D2GetClient = NULL;
TD2GameGetObject D2GameGetObject = NULL;
TD2InvRemoveItem D2InvRemoveItem = NULL;
TD2ItemSetPage D2ItemSetPage = NULL;
bool stashFindSpot(Unit*, Stash*, Unit*, BYTE*, BYTE*, bool) { return false; }
bool detachItem(Unit*, Unit*, StashLoc*) { return false; }
bool attachItem(Unit*, Unit*, Stash*, BYTE, BYTE) { return false; }
Unit* findStashItem(Unit*, DWORD, StashLoc*) { return NULL; }
void selectStash(Unit*, Stash*, bool) {}
bool SharedTransportReady() { return false; }
bool SharedTransportSendCloseCube(Unit*) { return false; }
bool SharedTransportSendDetach(Unit*, DWORD) { return false; }
void log_msg(const char*, ...) {}
DWORD FASTCALL handleStashPacket(Game*, Unit*, BYTE*, DWORD) { return 0; }
static DWORD FASTCALL foreignHandler(Game*, Unit*, BYTE*, DWORD) { return 1; }

static unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; printf("FAIL %u: %s\n", __LINE__, #value); } } while (0)

int main()
{
	DWORD handlers[0x67 * 2] = {};
	offset_D2Game = (DWORD)handlers - 0x2E0D18;
	DWORD& slot = handlers[PY_STASH_PACKET * 2];
	originalHandlers[PY_STASH_PACKET] = handleStashPacket;
	slot = (DWORD)dispatch;
	CHECK(!SharedNativeWrapsStashPacket());
	installed = true;
	CHECK(SharedNativeWrapsStashPacket());
	originalHandlers[PY_STASH_PACKET] = foreignHandler;
	CHECK(!SharedNativeWrapsStashPacket());
	originalHandlers[PY_STASH_PACKET] = NULL;
	CHECK(!SharedNativeWrapsStashPacket());
	originalHandlers[PY_STASH_PACKET] = handleStashPacket;
	slot = (DWORD)foreignHandler;
	CHECK(!SharedNativeWrapsStashPacket());
	slot = (DWORD)handleStashPacket;
	CHECK(!SharedNativeWrapsStashPacket());
	slot = (DWORD)dispatch;
	version_D2Game = 0;
	CHECK(!SharedNativeWrapsStashPacket());
	version_D2Game = V114d;
	CHECK(SharedNativeWrapsStashPacket());
	printf("Shared stash native wrapper: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
