#pragma once

#include "common.h"
#include "../common/SharedStashStore.h"

// Private transport extension; the public 0x55 request remains unchanged.
#define PY_SHARED_REVISION_MESSAGE 0x25
#define PY_SHARED_DETACH_MESSAGE 0x26
#define PY_SHARED_LAYOUT_MESSAGE 0x27
#define PY_SHARED_CUBE_CLOSE_MESSAGE 0x28
#define PY_SHARED_MAX_LAYOUT_PAGES 65535
#define PY_SHARED_STAMP_PACKET 0x56
#define PY_SHARED_STAMP_SIZE 26

bool Install_SharedStashTransport();
bool SharedTransportReady();
// Publish only after the corresponding client item/page updates have been queued.
bool SharedTransportSendRevision(Unit* ptChar, SharedStash::Id revision);
// Consume the one-use client stamp before native mutation. Never substitute the server's revision.
bool SharedTransportTakeRequest(Unit* ptChar, BYTE packetId, SharedStash::Id& revision,
    unsigned long long& counter);
void SharedTransportApplyClient(const BYTE* packet);
bool SharedTransportSendDetach(Unit* ptChar, DWORD itemGid);
void SharedTransportApplyDetach(const BYTE* packet);
// The server must select a personal page first. Detached shared units remain alive
// for the following ITEM_MOVED messages, preserving local gids across a reorder.
bool SharedTransportSendLayout(Unit* ptChar, DWORD pageCount);
void SharedTransportApplyLayout(const BYTE* packet);
bool SharedTransportSendCloseCube(Unit* ptChar);
void SharedTransportApplyCloseCube(const BYTE* packet);
void SharedTransportClose(PlayerData* playerData);
