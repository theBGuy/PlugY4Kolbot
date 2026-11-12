// Test the production transport state machine with native game calls replaced by
// local stubs. Native patch installation and client item lifetime need live tests.
#include "../src/plugy/SharedStashTransport.cpp"
#include <stddef.h>
#include <stdio.h>

namespace {
struct PlayerBlock {
    PlayerData base;
    PYPlayerData stash;
};

Unit testPlayer = {};
PlayerBlock testData = {};
DWORD testMode = 0;
bool havePlayer = true;
Unit* testPlayerView = &testPlayer;
std::vector<SharedStash::Bytes> sentPackets;
int checks = 0;
int failures = 0;
int cubeCloses = 0;

void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

Unit* STDCALL getTestPlayer() { return havePlayer ? testPlayerView : NULL; }
BYTE STDCALL getTestPage(Unit*) { return 4; }

DWORD STDCALL capturePacket(DWORD size, DWORD, void* data) {
    BYTE* bytes = (BYTE*)data;
    sentPackets.push_back(SharedStash::Bytes(bytes, bytes + size));
    return 1;
}

void makeClientMessage(BYTE* packet, BYTE kind, SharedStash::Id value) {
    memset(packet, 0, 36);
    packet[0] = 0x9D;
    packet[1] = kind;
    packet[2] = 36;
    writeValue<DWORD>(packet + 4, testPlayer.nItemNum);
    writeValue(packet + 8, value);
}

void publishTestRevision(SharedStash::Id revision, Counter epoch) {
    BYTE packet[36];
    makeClientMessage(packet, PY_SHARED_REVISION_MESSAGE, revision);
    writeValue(packet + 16, epoch);
    SharedTransportApplyClient(packet);
}

void testRequestStamps() {
    sentPackets.clear();
    publishTestRevision(7, 99);
    CHECK(clientStamp.epoch == 99 && clientStamp.revision == 7);

    BYTE action[5] = {0x19, 1, 2, 3, 4};
    CHECK(stampedSend(sizeof(action), 0, action) == 1);
    CHECK(sentPackets.size() == 2);
    CHECK(sentPackets[0].size() == PY_SHARED_STAMP_SIZE);
    CHECK(sentPackets[0][0] == PY_SHARED_STAMP_PACKET && sentPackets[0][25] == 0x19);
    CHECK(sentPackets[1] == SharedStash::Bytes(action, action + sizeof(action)));
    CHECK(readValue<Counter>(&sentPackets[0][9]) == 1);
    CHECK(readValue<Counter>(&sentPackets[0][17]) == 99);

    ServerStamp& server = serverStamps[&testData.base];
    server.epoch = 99;
    SharedStash::Id revision = -1;
    Counter counter = 0;
    CHECK(receiveStamp(NULL, &testPlayer, &sentPackets[0][0], PY_SHARED_STAMP_SIZE) == 0);
    CHECK(SharedTransportTakeRequest(&testPlayer, 0x19, revision, counter));
    CHECK(revision == 7 && counter == 1);
    CHECK(!SharedTransportTakeRequest(&testPlayer, 0x19, revision, counter));

    // The same operation must not be admitted twice.
    receiveStamp(NULL, &testPlayer, &sentPackets[0][0], PY_SHARED_STAMP_SIZE);
    CHECK(!SharedTransportTakeRequest(&testPlayer, 0x19, revision, counter));

    // A stamp belongs to the immediately associated native action, not another ID.
    writeValue<Counter>(&sentPackets[0][9], 2);
    receiveStamp(NULL, &testPlayer, &sentPackets[0][0], PY_SHARED_STAMP_SIZE);
    CHECK(!SharedTransportTakeRequest(&testPlayer, 0x18, revision, counter));
    CHECK(!SharedTransportTakeRequest(&testPlayer, 0x19, revision, counter));

    // A new game must not accept a stamp from another session, even for the same gid.
    writeValue<Counter>(&sentPackets[0][17], 100);
    receiveStamp(NULL, &testPlayer, &sentPackets[0][0], PY_SHARED_STAMP_SIZE);
    CHECK(!SharedTransportTakeRequest(&testPlayer, 0x19, revision, counter));
    CHECK(receiveStamp(NULL, &testPlayer, &sentPackets[0][0], 3) == 3);

    publishTestRevision(6, 99);
    CHECK(clientStamp.revision == 7);
    publishTestRevision(1, 101);
    CHECK(clientStamp.epoch == 101 && clientStamp.revision == 1 && clientStamp.counter == 0);

    // The coordinator compares this client revision with the database revision.
    // The transport must preserve it, rather than replace it with server state.
    sentPackets.clear();
    stampedSend(sizeof(action), 0, action);
    CHECK(readValue<SharedStash::Id>(&sentPackets[0][1]) == 1);
    server = ServerStamp();
    server.epoch = 101;
    receiveStamp(NULL, &testPlayer, &sentPackets[0][0], PY_SHARED_STAMP_SIZE);
    CHECK(SharedTransportTakeRequest(&testPlayer, 0x19, revision, counter));
    CHECK(revision == 1);

    sentPackets.clear();
    clientStamp.counter = ~(Counter)0;
    CHECK(stampedSend(sizeof(action), 0, action) == 0);
    CHECK(sentPackets.empty());
    clientStamp = ClientStamp();

    BYTE unrelated[1] = {0x6D};
    CHECK(stampedSend(sizeof(unrelated), 0, unrelated) == 1);
    CHECK(sentPackets.size() == 1 && sentPackets[0][0] == 0x6D);

    // NPC sales, repairs, and quest services can also mutate a shown shared item.
    // Use the same action set for the real client stamp and server admission.
    publishTestRevision(1, 102);
    server = ServerStamp();
    server.epoch = 102;
    for (BYTE id = 0x31; id <= 0x38; ++id) {
        sentPackets.clear();
        action[0] = id;
        CHECK(stampedSend(sizeof(action), 0, action) == 1);
        CHECK(sentPackets.size() == 2 && sentPackets[0][25] == id);
        receiveStamp(NULL, &testPlayer, &sentPackets[0][0], PY_SHARED_STAMP_SIZE);
        CHECK(SharedTransportTakeRequest(&testPlayer, id, revision, counter));
    }
    SharedTransportClose(&testData.base);
    CHECK(serverStamps.empty());
}

Stash* makePage() {
    Stash* page = (Stash*)calloc(1, sizeof(Stash));
    if (!page) throw std::bad_alloc();
    page->flags = 1;
    return page;
}

void testLayoutChanges() {
    Stash personal = {};
    testData.stash.currentStash = &personal;
    Stash* first = makePage();
    Stash* second = makePage();
    first->nextStash = second;
    second->previousStash = first;
    second->id = 1;
    testData.stash.sharedStash = first;
    testData.stash.nbSharedPages = 2;

    Unit items[3] = {};
    ItemData data[3] = {};
    for (int i = 0; i < 3; ++i) {
        items[i].nUnitType = UNIT_ITEM;
        items[i].nItemNum = 100 + i;
        items[i].ptItemData = &data[i];
    }
    first->ptListItem = &items[0];
    data[0].ptNextItem = &items[1];
    data[1].ptPrevItem = &items[0];
    second->ptListItem = &items[2];

    BYTE packet[36];
    makeClientMessage(packet, PY_SHARED_LAYOUT_MESSAGE, 1);
    SharedTransportApplyLayout(packet);
    CHECK(testData.stash.nbSharedPages == 1 && testData.stash.sharedStash == first);
    CHECK(!first->nextStash && !first->ptListItem);
    CHECK(!data[0].ptNextItem && !data[1].ptPrevItem && !data[2].ptNextItem);
    CHECK(items[0].nItemNum == 100 && items[1].nItemNum == 101 && items[2].nItemNum == 102);
    CHECK(testData.stash.currentStash == &personal);

    makeClientMessage(packet, PY_SHARED_LAYOUT_MESSAGE, 3);
    SharedTransportApplyLayout(packet);
    CHECK(testData.stash.nbSharedPages == 3);
    CHECK(first->nextStash && first->nextStash->id == 1 && first->nextStash->nextStash->id == 2);
    CHECK(first->nextStash->previousStash == first);

    // A layout may only run after selecting personal storage.
    testData.stash.currentStash = first;
    DWORD before = testData.stash.clientDesync;
    makeClientMessage(packet, PY_SHARED_LAYOUT_MESSAGE, 1);
    SharedTransportApplyLayout(packet);
    CHECK(testData.stash.clientDesync == before + 1 && testData.stash.nbSharedPages == 3);
    CHECK(clientStamp.refreshFailed);

    // Validate every old list before changing any of them.
    testData.stash.currentStash = &personal;
    first->ptListItem = &items[0];
    data[0].ptNextItem = &items[0];
    before = testData.stash.clientDesync;
    SharedTransportApplyLayout(packet);
    CHECK(testData.stash.clientDesync == before + 1);
    CHECK(first->ptListItem == &items[0] && data[0].ptNextItem == &items[0]);
    data[0].ptNextItem = NULL;

    data[0].ptInventory = (Inventory*)1;
    before = testData.stash.clientDesync;
    SharedTransportApplyLayout(packet);
    CHECK(testData.stash.clientDesync == before + 1 && first->ptListItem == &items[0]);
    data[0].ptInventory = NULL;

    makeClientMessage(packet, PY_SHARED_LAYOUT_MESSAGE, PY_SHARED_MAX_LAYOUT_PAGES + 1);
    before = testData.stash.clientDesync;
    SharedTransportApplyLayout(packet);
    CHECK(testData.stash.clientDesync == before + 1 && testData.stash.nbSharedPages == 3);

    for (Stash* page = testData.stash.sharedStash; page;) {
        Stash* next = page->nextStash;
        free(page->name);
        free(page);
        page = next;
    }
    testData.stash.sharedStash = NULL;
    testData.stash.currentStash = NULL;
}

void testCubeClose() {
    BYTE packet[36];
    makeClientMessage(packet, PY_SHARED_CUBE_CLOSE_MESSAGE, 0);
    SharedTransportApplyCloseCube(packet);
    CHECK(cubeCloses == 1);
    writeValue<DWORD>(packet + 4, testPlayer.nItemNum + 1);
    SharedTransportApplyCloseCube(packet);
    CHECK(cubeCloses == 1);
    makeClientMessage(packet, PY_SHARED_CUBE_CLOSE_MESSAGE, 0);
    packet[2] = 1;
    SharedTransportApplyCloseCube(packet);
    CHECK(cubeCloses == 1);
}

void testRefreshFailureLatch() {
    clientStamp = ClientStamp();
    publishTestRevision(7, 201);
    Stash personal = {};
    Stash* shared = makePage();
    testData.stash.currentStash = shared;
    testData.stash.sharedStash = shared;
    testData.stash.nbSharedPages = 1;
    BYTE packet[36];
    makeClientMessage(packet, PY_SHARED_LAYOUT_MESSAGE, 1);
    writeValue<Counter>(packet + 16, 201);

    // A failed reset is followed by the normal server revision notification.
    // That notification must not make the still-stale shared layout writable.
    SharedTransportApplyLayout(packet);
    CHECK(clientStamp.refreshFailed && clientStamp.failedEpoch == 201);
    CHECK(clientStamp.epoch == 201 && clientStamp.revision == 7);
    publishTestRevision(8, 201);
    CHECK(clientStamp.refreshFailed && clientStamp.revision == 7);
    sentPackets.clear();
    BYTE action[5] = {0x19, 1, 2, 3, 4};
    CHECK(stampedSend(sizeof(action), 0, action) == 0);
    CHECK(sentPackets.empty());

    testData.stash.currentStash = &personal;
    SharedTransportApplyLayout(packet);
    CHECK(!clientStamp.refreshFailed && clientStamp.epoch == 201);
    publishTestRevision(8, 201);
    CHECK(clientStamp.revision == 8);
    CHECK(stampedSend(sizeof(action), 0, action) == 1);
    CHECK(sentPackets.size() == 2 && readValue<Counter>(&sentPackets[0][9]) == 1);
    publishTestRevision(7, 201);
    CHECK(clientStamp.revision == 8 && clientStamp.counter == 1);

    // A new server session clears the failure, but old session notifications
    // cannot roll the epoch back or reuse that session's request counter.
    testData.stash.currentStash = shared;
    SharedTransportApplyLayout(packet);
    publishTestRevision(1, 202);
    CHECK(!clientStamp.refreshFailed && clientStamp.epoch == 202 && clientStamp.counter == 0);
    publishTestRevision(99, 201);
    CHECK(clientStamp.epoch == 202 && clientStamp.revision == 1 && clientStamp.counter == 0);
    sentPackets.clear();
    CHECK(stampedSend(sizeof(action), 0, action) == 1);
    CHECK(readValue<Counter>(&sentPackets[0][9]) == 1);

    // A different client player may initialize its own state.
    writeValue<Counter>(packet + 16, 202);
    SharedTransportApplyLayout(packet);
    CHECK(clientStamp.refreshFailed);
    Unit otherPlayer = testPlayer;
    testPlayerView = &otherPlayer;
    publishTestRevision(2, 202);
    CHECK(!clientStamp.refreshFailed && clientStamp.player == &otherPlayer);
    CHECK(clientStamp.counter == 0 && clientStamp.revision == 2);
    testPlayerView = &testPlayer;
    clientStamp = ClientStamp();

    // The initial layout can fail before this player received any revision.
    // Its following revision publication must also stay blocked.
    SharedTransportApplyLayout(packet);
    CHECK(clientStamp.refreshFailed && clientStamp.failedEpoch == 202);
    CHECK(clientStamp.player == &testPlayer && clientStamp.revision == -1);
    publishTestRevision(9, 202);
    sentPackets.clear();
    CHECK(stampedSend(sizeof(action), 0, action) == 0 && sentPackets.empty());
    free(shared);
    testData.stash.sharedStash = NULL;
    testData.stash.currentStash = NULL;
}
}

// Only these native seams are reached by the model tests. Patch installation is
// never called; the tests do not open or change an installed game or its saves.
s_shifting shifting = {0, offsetof(Unit, ptPlayerData), offsetof(PlayerBlock, stash), 0, 0, 0, 0, 0};
DWORD* ptClientGameMode = &testMode;
TD2GetClientPlayer D2GetClientPlayer = getTestPlayer;
TD2ItemGetPage D2ItemGetPage = getTestPage;
TD2SendPacket D2SendPacket = NULL;
TD2GetClient D2GetClient = NULL;
int version_D2Client = 0, version_D2Game = 0, version_D2Net = 0;
DWORD offset_D2Game = 0, offset_D2Net = 0;
bool hasStashData(Unit* player) { return player && player->ptPlayerData; }
void log_msg(const char*, ...) {}
Unit* findStashItem(Unit*, DWORD, StashLoc*) { return NULL; }
bool detachItem(Unit*, Unit*, StashLoc*) { return false; }
void SharedNativeCloseClientCube() { ++cubeCloses; }

int main() {
    installed = true;
    InitializeCriticalSection(&sendLock);
    InitializeCriticalSection(&serverLock);
    originalSend = capturePacket;
    testPlayer.nItemNum = 42;
    testPlayer.nUnitType = UNIT_PLAYER;
    testPlayer.ptPlayerData = &testData.base;

    testRequestStamps();
    testLayoutChanges();
    testCubeClose();
    testRefreshFailureLatch();

    DeleteCriticalSection(&serverLock);
    DeleteCriticalSection(&sendLock);
    printf("SharedStashTransportTests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
