#pragma once

#include "common.h"
#include "../common/SharedStashStore.h"
#include "sharedStashItemPacketRules.h"

typedef bool (*SharedNativeBeforeAction)(Game*, Unit*, const BYTE*, DWORD);
typedef void (*SharedNativeAfterAction)(Unit*);
typedef bool (*SharedNativeBeforeSend)(void*, const BYTE*, DWORD);

// Install after the custom 0x3A/0x55 handlers. Call on the install thread before play.
bool Install_SharedStashNative(SharedNativeBeforeAction before, SharedNativeAfterAction after);
bool SharedNativeReady();
// Set during startup. Runs outside the buffer lock; false suppresses the original packet.
void SharedNativeSetBeforeSend(SharedNativeBeforeSend callback);

// Server thread only. The loader bridge must be called first by placeLoadedItem.
Unit* SharedNativeLoadItem(Unit* ptChar, const SharedStash::Bytes& data, Stash* target, BYTE x, BYTE y, BYTE storagePage = 4);
bool SharedNativePlaceLoadedItem(Game* game, Unit* ptChar, Unit* item, Unit* parent, DWORD* result);
bool SharedNativeRemoveItem(Unit* ptChar, Unit* item, bool notify = true);
bool SharedNativeSendItem(Unit* ptChar, Unit* item, Stash* page);
// Repair optimistic client cursor state from the authoritative server inventory.
bool SharedNativeSendCursor(Unit* ptChar, bool clearIfEmpty = true);
// Call only when the coordinator loses its active shared cube; NPC/stash interactions are preserved.
bool SharedNativeCloseCube(Unit* ptChar);
void SharedNativeCloseClientCube();
bool SharedNativeAttachCubeItem(Unit* ptChar, Unit* item, BYTE x, BYTE y);

// A hold is per NetClient, not per process. It also catches later queued game updates.
bool SharedNativeHoldNotifications(Unit* ptChar);
bool SharedNativeReleaseNotifications(Unit* ptChar);
void SharedNativeDiscardNotifications(Unit* ptChar);
typedef bool (*SharedNativePacketFilter)(const BYTE*, DWORD, void*);
// Pure predicate only: it must not send packets or change the notification hold.
bool SharedNativeFilterNotifications(Unit* ptChar, SharedNativePacketFilter keep, void* context);

#ifdef PLUGY_TEST_HOOKS
// Real-engine fixture creation for isolated harness saves only. Requires an empty cursor.
Unit* SharedNativeCreateTestItem(Unit* ptChar, DWORD itemCode, DWORD quantity = 0, DWORD sockets = 0);
#endif
