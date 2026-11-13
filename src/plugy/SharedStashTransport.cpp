#include "sharedStashTransport.h"
#include "sharedStashNative.h"
#include "infinityStash.h"
#include "stashProtocol.h"
#include <map>
#include <set>
#include <string.h>
#include <stdlib.h>

namespace {
typedef unsigned long long Counter;
typedef DWORD (STDCALL *SendToServer)(DWORD, DWORD, void*);
SendToServer originalSend;
bool installed = false;
CRITICAL_SECTION sendLock, serverLock;

struct Guard {
    CRITICAL_SECTION* section;
    Guard(CRITICAL_SECTION& value) : section(&value) { EnterCriticalSection(section); }
    ~Guard() { LeaveCriticalSection(section); }
};
struct ServerStamp {
    Counter epoch, counter, consumed;
    SharedStash::Id revision;
    BYTE packet;
    bool present;
    ServerStamp() : epoch(0), counter(0), consumed(0), revision(-1), packet(0), present(false) {}
};
std::map<PlayerData*, ServerStamp> serverStamps;
LONG nextEpoch = 0;
struct ClientStamp {
    Counter epoch, counter;
    SharedStash::Id revision;
    Unit* player;
    bool refreshFailed;
    Counter failedEpoch;
    ClientStamp() : epoch(0), counter(0), revision(-1), player(NULL), refreshFailed(false),
        failedEpoch(0) {}
} clientStamp;

#pragma pack(push, 1)
struct RevisionMessage {
    BYTE type, function, size, zero;
    DWORD player;
    SharedStash::Id revision;
    Counter epoch;
    DWORD reserved[3];
};
#pragma pack(pop)
C_ASSERT(sizeof(RevisionMessage) == 36);

bool ensureEpoch(ServerStamp& state) {
    if (state.epoch) return true;
    DWORD sequence = (DWORD)InterlockedIncrement(&nextEpoch);
    if (!sequence) return false;
    state.epoch = ((Counter)GetCurrentProcessId() << 32) | sequence;
    return true;
}

void failClientRefresh(Unit* player, Counter epoch) {
    Guard lock(sendLock);
    if (clientStamp.player != player) {
        clientStamp = ClientStamp();
        clientStamp.player = player;
        clientStamp.epoch = epoch;
    }
    clientStamp.refreshFailed = true;
    clientStamp.failedEpoch = epoch ? epoch : clientStamp.epoch;
    // Keep the epoch and request counter. Erasing them would let the following
    // same-session revision message make this stale layout writable again.
}

template<class T> T readValue(const BYTE* data) {
    T value; memcpy(&value, data, sizeof(value)); return value;
}
template<class T> void writeValue(BYTE* data, T value) { memcpy(data, &value, sizeof(value)); }

DWORD STDCALL stampedSend(DWORD size, DWORD one, void* data) {
    // The lock covers both native sends. No other sending thread may place an action
    // between its stamp and packet. Calling the trampoline never enters this hook again.
    Guard lock(sendLock);
    if (sharedStashClosePacket((const BYTE*)data, size)) return originalSend(size, one, data);
    if (data && size && ClientGameMode == 0 && clientStamp.refreshFailed
        && D2GetClientPlayer() == clientStamp.player && sharedStashActionPacket(*(BYTE*)data)) return 0;
    if (data && size && ClientGameMode == 0 && clientStamp.epoch && clientStamp.revision >= 0) {
        Unit* player = D2GetClientPlayer();
        if (!player || player != clientStamp.player) clientStamp = ClientStamp();
        else if (sharedStashActionPacket(*(BYTE*)data)) {
            if (clientStamp.counter == ~(Counter)0) return 0; // never wrap a session request ID
            BYTE stamp[PY_SHARED_STAMP_SIZE] = { PY_SHARED_STAMP_PACKET };
            writeValue(stamp + 1, clientStamp.revision);
            writeValue(stamp + 9, ++clientStamp.counter);
            writeValue(stamp + 17, clientStamp.epoch);
            stamp[25] = *(BYTE*)data;
            if (!originalSend(sizeof(stamp), one, stamp)) return 0;
        }
    }
    return originalSend(size, one, data);
}

DWORD FASTCALL receiveStamp(Game*, Unit* ptChar, BYTE* packet, DWORD size) {
    if (size != PY_SHARED_STAMP_SIZE || !packet || packet[0] != PY_SHARED_STAMP_PACKET)
        return 3;
    if (!ptChar || ptChar->nUnitType != UNIT_PLAYER || !PCPlayerData) return 0;
    try {
        Guard lock(serverLock);
        std::map<PlayerData*, ServerStamp>::iterator found = serverStamps.find(PCPlayerData);
        if (found == serverStamps.end()) return 0;
        ServerStamp& state = found->second;
        state.present = false;
        Counter counter = readValue<Counter>(packet + 9);
        if (readValue<Counter>(packet + 17) != state.epoch || !counter
            || counter <= state.consumed || !sharedStashActionPacket(packet[25])) return 0;
        SharedStash::Id revision = readValue<SharedStash::Id>(packet + 1);
        if (revision < 0) return 0;
        state.counter = counter;
        state.revision = revision;
        state.packet = packet[25];
        state.present = true;
    } catch (...) { log_msg("Shared stash request stamp refused: memory failure.\n"); }
    return 0;
}
}

bool SharedTransportReady() { return installed; }

bool SharedTransportSendRevision(Unit* ptChar, SharedStash::Id revision) {
    if (!installed || revision < 0 || !ptChar || !PCPlayerData) return false;
    try {
        RevisionMessage packet = {};
        {
            Guard lock(serverLock);
            ServerStamp& state = serverStamps[PCPlayerData];
            if (!ensureEpoch(state)) return false;
            packet.type = 0x9D;
            packet.function = PY_SHARED_REVISION_MESSAGE;
            packet.size = sizeof(packet);
            packet.player = ptChar->nItemNum;
            packet.revision = revision;
            packet.epoch = state.epoch;
        }
        void* client = D2GetClient(ptChar, __FILE__, __LINE__);
        if (!client) return false;
        D2SendPacket(client, &packet, sizeof(packet));
        return true;
    } catch (...) { log_msg("Shared stash revision notification failed: memory failure.\n"); return false; }
}

bool SharedTransportTakeRequest(Unit* ptChar, BYTE packetId, SharedStash::Id& revision, Counter& counter) {
    revision = -1; counter = 0;
    if (!installed || !ptChar || !PCPlayerData) return false;
    Guard lock(serverLock);
    std::map<PlayerData*, ServerStamp>::iterator found = serverStamps.find(PCPlayerData);
    if (found == serverStamps.end()) return false;
    ServerStamp& state = found->second;
    bool valid = state.present && state.packet == packetId && state.counter > state.consumed;
    state.present = false;
    if (!valid) return false;
    revision = state.revision;
    counter = state.counter;
    state.consumed = counter;
    return true;
}

void SharedTransportApplyClient(const BYTE* data) {
    if (!installed || !data || data[0] != 0x9D || data[1] != PY_SHARED_REVISION_MESSAGE
        || data[2] != sizeof(RevisionMessage) || ClientGameMode != 0) return;
    RevisionMessage packet; memcpy(&packet, data, sizeof(packet));
    Unit* player = D2GetClientPlayer();
    if (!player || player->nItemNum != packet.player || !packet.epoch || packet.revision < 0) return;
    Guard lock(sendLock);
    if (clientStamp.player == player && clientStamp.epoch && packet.epoch < clientStamp.epoch) return;
    if (clientStamp.refreshFailed && clientStamp.player == player) {
        if (!clientStamp.failedEpoch || packet.epoch <= clientStamp.failedEpoch) return;
        // A different server session is safe to initialize. A revision from the
        // failed session is not evidence that its page refresh succeeded.
    }
    if (clientStamp.epoch != packet.epoch || clientStamp.player != player) {
        clientStamp = ClientStamp();
        clientStamp.epoch = packet.epoch;
        clientStamp.player = player;
    } else if (packet.revision < clientStamp.revision) return;
    clientStamp.revision = packet.revision;
}

bool SharedTransportSendDetach(Unit* ptChar, DWORD itemGid) {
    if (!installed || !ptChar || !PCPlayerData) return false;
    RevisionMessage packet = {};
    packet.type = 0x9D; packet.function = PY_SHARED_DETACH_MESSAGE; packet.size = sizeof(packet);
    packet.player = ptChar->nItemNum;
    packet.revision = itemGid;
    void* client = D2GetClient(ptChar, __FILE__, __LINE__);
    if (!client) return false;
    D2SendPacket(client, &packet, sizeof(packet));
    return true;
}

void SharedTransportApplyDetach(const BYTE* data) {
    if (!installed || !data || data[0] != 0x9D || data[1] != PY_SHARED_DETACH_MESSAGE
        || data[2] != sizeof(RevisionMessage) || ClientGameMode != 0) return;
    Unit* ptChar = D2GetClientPlayer();
    if (!hasStashData(ptChar) || ptChar->nItemNum != readValue<DWORD>(data + 4)) return;
    StashLoc where;
    Unit* item = findStashItem(ptChar, readValue<DWORD>(data + 8), &where);
    // Vanilla 0x0A cleans up the current inventory grid and frees the unit. It
    // cannot see a hidden PlugY list, so unlink that list first without freeing.
    if (item && where.kind == STASHLOC_LIST && !detachItem(ptChar, item, &where))
        log_msg("Shared stash client could not unlink hidden item %u before removal.\n", item->nItemNum);
}

bool SharedTransportSendLayout(Unit* ptChar, DWORD pageCount) {
    if (!installed || !ptChar || !PCPlayerData || !pageCount || pageCount > PY_SHARED_MAX_LAYOUT_PAGES)
        return false;
    RevisionMessage packet = {};
    packet.type = 0x9D; packet.function = PY_SHARED_LAYOUT_MESSAGE; packet.size = sizeof(packet);
    packet.player = ptChar->nItemNum;
    packet.revision = pageCount;
    try {
        Guard lock(serverLock);
        ServerStamp& state = serverStamps[PCPlayerData];
        if (!ensureEpoch(state)) return false;
        packet.epoch = state.epoch;
    } catch (...) { log_msg("Shared stash layout notification failed: memory failure.\n"); return false; }
    void* client = D2GetClient(ptChar, __FILE__, __LINE__);
    if (!client) return false;
    D2SendPacket(client, &packet, sizeof(packet));
    return true;
}

void SharedTransportApplyLayout(const BYTE* data) {
    if (!installed || !data || data[0] != 0x9D || data[1] != PY_SHARED_LAYOUT_MESSAGE
        || data[2] != sizeof(RevisionMessage) || ClientGameMode != 0) return;
    Unit* ptChar = D2GetClientPlayer();
    if (!hasStashData(ptChar) || ptChar->nItemNum != readValue<DWORD>(data + 4)) return;
    SharedStash::Id requested = readValue<SharedStash::Id>(data + 8);
    Counter epoch = readValue<Counter>(data + 16);
    {
        Guard lock(sendLock);
        if (epoch && clientStamp.player == ptChar && clientStamp.epoch > epoch) return;
    }
    std::vector<Stash*> oldPages, extraPages, result;
    try {
        if (requested <= 0 || requested > PY_SHARED_MAX_LAYOUT_PAGES
            || !PCPY->currentStash || PCPY->currentStash->isShared)
            throw "shared layout arrived before a personal-page selection";
        std::set<Stash*> seenPages;
        std::set<Unit*> seenItems;
        for (Stash* page = PCPY->sharedStash; page; page = page->nextStash) {
            if (!page->isShared || !seenPages.insert(page).second || oldPages.size() == PY_SHARED_MAX_LAYOUT_PAGES)
                throw "invalid shared page chain";
            oldPages.push_back(page);
            for (Unit* item = page->ptListItem; item; item = item->ptItemData->ptNextItem) {
                if (!seenItems.insert(item).second || item->nUnitType != UNIT_ITEM || !item->ptItemData
                    || item->mode != 0 || item->ptItemData->ptInventory || D2ItemGetPage(item) != 4)
                    throw "shared page holds an owned, active, or repeated item";
            }
        }
        const size_t count = (size_t)requested;
        result.reserve(count);
        extraPages.reserve(count > oldPages.size() ? count - oldPages.size() : 0);
        for (size_t i = 0; i < count; ++i) {
            if (i < oldPages.size()) result.push_back(oldPages[i]);
            else {
                Stash* page = (Stash*)calloc(1, sizeof(Stash));
                if (!page) throw "shared page allocation failed";
                extraPages.push_back(page);
                result.push_back(page);
            }
        }
        // Validation and allocation are complete. No unit is freed: the native
        // client hash still owns every item, and subsequent ITEM_MOVED attaches it.
        for (size_t i = 0; i < oldPages.size(); ++i) {
            Unit* item = oldPages[i]->ptListItem;
            oldPages[i]->ptListItem = NULL;
            while (item) {
                Unit* next = item->ptItemData->ptNextItem;
                item->ptItemData->ptNextItem = NULL;
                item->ptItemData->ptPrevItem = NULL;
                item = next;
            }
            free(oldPages[i]->name);
            oldPages[i]->name = NULL;
        }
        for (size_t i = 0; i < result.size(); ++i) {
            Stash* page = result[i];
            page->id = (DWORD)i;
            page->flags = 1; // server sends every page's flags and name after this reset
            page->previousStash = i ? result[i - 1] : NULL;
            page->nextStash = i + 1 < result.size() ? result[i + 1] : NULL;
        }
        for (size_t i = count; i < oldPages.size(); ++i) free(oldPages[i]);
        PCPY->sharedStash = result[0];
        PCPY->nbSharedPages = (DWORD)count;
        PCPY->showSharedStash = false;
        {
            Guard lock(sendLock);
            if (clientStamp.refreshFailed && clientStamp.player == ptChar
                && (!epoch || !clientStamp.failedEpoch || epoch >= clientStamp.failedEpoch)) {
                clientStamp.refreshFailed = false;
                clientStamp.failedEpoch = 0;
            }
        }
        log_msg("[CLIENT] Shared stash layout reset: %u pages; item gids preserved.\n", (DWORD)count);
    } catch (const char* reason) {
        for (size_t i = 0; i < extraPages.size(); ++i) free(extraPages[i]);
        PCPY->clientDesync++;
        failClientRefresh(ptChar, epoch);
        log_msg("[CLIENT] Shared stash layout refused: %s.\n", reason);
    } catch (...) {
        for (size_t i = 0; i < extraPages.size(); ++i) free(extraPages[i]);
        PCPY->clientDesync++;
        failClientRefresh(ptChar, epoch);
        log_msg("[CLIENT] Shared stash layout refused: memory failure.\n");
    }
}

bool SharedTransportSendCloseCube(Unit* ptChar) {
    if (!installed || !ptChar || !PCPlayerData) return false;
    RevisionMessage packet = {};
    packet.type = 0x9D; packet.function = PY_SHARED_CUBE_CLOSE_MESSAGE; packet.size = sizeof(packet);
    packet.player = ptChar->nItemNum;
    void* client = D2GetClient(ptChar, __FILE__, __LINE__);
    if (!client) return false;
    D2SendPacket(client, &packet, sizeof(packet));
    return true;
}

void SharedTransportApplyCloseCube(const BYTE* data) {
    if (!installed || !data || data[0] != 0x9D || data[1] != PY_SHARED_CUBE_CLOSE_MESSAGE
        || data[2] != sizeof(RevisionMessage) || ClientGameMode != 0) return;
    Unit* player = D2GetClientPlayer();
    if (!hasStashData(player) || player->nItemNum != readValue<DWORD>(data + 4)) return;
    SharedNativeCloseClientCube();
}

void SharedTransportClose(PlayerData* playerData) {
    if (!installed) return;
    Guard lock(serverLock);
    serverStamps.erase(playerData);
}
bool Install_SharedStashTransport() {
    if (installed) return true;
    if (version_D2Game != V114d || version_D2Client != V114d || version_D2Net != V114d) return false;
    BYTE* entry = (BYTE*)(offset_D2Net + 0x12AE50);
    // 1.14d: push ebp; mov ebp,esp; cmp dword ptr [connection global],0.
    // The copied ten bytes contain no relative operand; the following JNE stays in the game.
    BYTE expected[10] = { 0x55, 0x8B, 0xEC, 0x83, 0x3D, 0, 0, 0, 0, 0 };
    writeValue<DWORD>(expected + 5, offset_D2Net + 0x482B34);
    DWORD* sizeSlot = (DWORD*)(offset_D2Game + 0x330DC0 + PY_SHARED_STAMP_PACKET * 4);
    DWORD* handlerSlot = (DWORD*)(offset_D2Game + 0x2E0D18 + PY_SHARED_STAMP_PACKET * 8);
    if (memcmp(entry, expected, sizeof(expected)) || *sizeSlot || *handlerSlot) {
        log_msg("Shared stash transport not installed: native send or packet slot differs.\n");
        return false;
    }
    BYTE* trampoline = (BYTE*)VirtualAlloc(NULL, 15, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return false;
    memcpy(trampoline, entry, 10);
    trampoline[10] = 0xE9;
    writeValue<DWORD>(trampoline + 11, (DWORD)(entry + 10) - (DWORD)(trampoline + 15));
    DWORD entryProtection, handlerProtection, sizeProtection;
    if (!VirtualProtect(entry, 10, PAGE_EXECUTE_READWRITE, &entryProtection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE); return false;
    }
    if (!VirtualProtect(handlerSlot, 4, PAGE_READWRITE, &handlerProtection)) {
        DWORD unused; VirtualProtect(entry, 10, entryProtection, &unused);
        VirtualFree(trampoline, 0, MEM_RELEASE); return false;
    }
    if (!VirtualProtect(sizeSlot, 4, PAGE_READWRITE, &sizeProtection)) {
        DWORD unused; VirtualProtect(handlerSlot, 4, handlerProtection, &unused);
        VirtualProtect(entry, 10, entryProtection, &unused);
        VirtualFree(trampoline, 0, MEM_RELEASE); return false;
    }
    InitializeCriticalSection(&sendLock);
    InitializeCriticalSection(&serverLock);
    originalSend = (SendToServer)trampoline;
    *handlerSlot = (DWORD)receiveStamp;
    *sizeSlot = PY_SHARED_STAMP_SIZE;
    entry[0] = 0xE9;
    writeValue<DWORD>(entry + 1, (DWORD)stampedSend - (DWORD)(entry + 5));
    memset(entry + 5, 0x90, 5);
    FlushInstructionCache(GetCurrentProcess(), entry, 10);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 15);
    DWORD unused;
    VirtualProtect(sizeSlot, 4, sizeProtection, &unused);
    VirtualProtect(handlerSlot, 4, handlerProtection, &unused);
    VirtualProtect(entry, 10, entryProtection, &unused);
    installed = true;
    log_msg("Shared stash revision transport installed (client packet %02X).\n", PY_SHARED_STAMP_PACKET);
    return true;
}
