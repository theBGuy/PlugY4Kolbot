#include "sharedStashTest.h"

#ifdef PLUGY_TEST_HOOKS
#include "common.h"
#include "infinityStash.h"
#include "sharedStashNative.h"
#include "sharedStashSession.h"
#include <ctype.h>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace {
char directory[MAX_PATH] = {};
bool configured = false;
DWORD lastFrame = 0;
std::set<std::string> consumedTokens;
const BYTE fixturePacket = 0x57;
const DWORD fixturePacketSize = 13;

DWORD FASTCALL fixtureHandler(Game* game, Unit* ptChar, BYTE* packet, DWORD size)
{
    if (!packet || size != fixturePacketSize || packet[0] != fixturePacket
        || !ptChar || ptChar->nUnitType != UNIT_PLAYER || !PCPlayerData || PCGame != game
        || ClientGameMode != 0 || !SharedStashActive(ptChar) || !SharedStashCanSave(ptChar)) return 0;
    DWORD code, quantity, sockets;
    memcpy(&code, packet + 1, sizeof(code));
    memcpy(&quantity, packet + 5, sizeof(quantity));
    memcpy(&sockets, packet + 9, sizeof(sockets));
    try {
        // The native fixture helper checks the item code, quantity, socket count,
        // and empty cursor; it also sends the actual new cursor item to the client.
        Unit* item = SharedNativeCreateTestItem(ptChar, code, quantity, sockets);
        log_msg("Shared stash TEST fixture: code %08X, quantity %u, sockets %u -> item %u.\n",
            code, quantity, sockets, item ? item->nItemNum : 0);
    } catch (...) {
        log_msg("Shared stash TEST fixture failed.\n");
    }
    return 0;
}

void installFixtureHandler()
{
    static bool attempted = false;
    if (attempted || !SharedNativeReady()) return;
    attempted = true;
    // Confirmed unused in the installed 1.14d binary; also check the live tables
    // to refuse a slot already taken by another plugin.
    DWORD* sizeSlot = (DWORD*)(offset_D2Game + 0x330DC0 + fixturePacket * 4);
    DWORD* handlerSlot = (DWORD*)(offset_D2Game + 0x2E0D18 + fixturePacket * 8);
    if (*sizeSlot || *handlerSlot) {
        log_msg("Shared stash TEST fixture packet not installed: slot %02X is in use.\n", fixturePacket);
        return;
    }
    DWORD handlerProtection, sizeProtection;
    if (!VirtualProtect(handlerSlot, sizeof(DWORD), PAGE_READWRITE, &handlerProtection)) return;
    if (!VirtualProtect(sizeSlot, sizeof(DWORD), PAGE_READWRITE, &sizeProtection)) {
        DWORD ignored;
        VirtualProtect(handlerSlot, sizeof(DWORD), handlerProtection, &ignored);
        return;
    }
    *handlerSlot = (DWORD)fixtureHandler;
    *sizeSlot = fixturePacketSize;
    DWORD ignored;
    VirtualProtect(sizeSlot, sizeof(DWORD), sizeProtection, &ignored);
    VirtualProtect(handlerSlot, sizeof(DWORD), handlerProtection, &ignored);
    log_msg("Shared stash TEST fixture packet installed: %02X (%u bytes).\n", fixturePacket, fixturePacketSize);
}

bool configureDirectory(char* out)
{
    DWORD size = GetEnvironmentVariableA("PLUGY_STASH_TEST_DIR", out, MAX_PATH);
    if (!size || size >= MAX_PATH) { out[0] = 0; return false; }
    bool absolute = (size >= 3 && isalpha((unsigned char)out[0]) && out[1] == ':' && (out[2] == '\\' || out[2] == '/'))
        || (size >= 3 && out[0] == '\\' && out[1] == '\\');
    DWORD attributes = absolute ? GetFileAttributesA(out) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) { out[0] = 0; return false; }
    return true;
}

bool testPathAt(char* out, const char* base, const char* suffix)
{
    return formatPath(out, MAX_PATH, "%s\\%lu.%s", base, GetCurrentProcessId(), suffix);
}

bool testPath(char* out, const char* suffix)
{
    return testPathAt(out, directory, suffix);
}

bool writeResultAt(const char* base, const char* suffix, const std::string& bytes)
{
    char path[MAX_PATH], temporary[MAX_PATH];
    if (!testPathAt(path, base, suffix) || !formatPath(temporary, MAX_PATH, "%s.tmp", path)) return false;
    HANDLE file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, NULL) && written == bytes.size();
    CloseHandle(file);
    if (ok) ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileA(temporary);
    return ok;
}

bool writeResult(const char* suffix, const std::string& bytes)
{
    return writeResultAt(directory, suffix, bytes);
}

void quoted(std::ostream& out, const std::string& value)
{
    const char hex[] = "0123456789abcdef";
    out << '"';
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32 || c >= 127) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << c;
    }
    out << '"';
}

void itemState(std::ostream& out, Unit* item)
{
    ItemsBIN* definition = D2GetItemsBIN(item->nTxtFileNo);
    DWORD code = definition ? definition->ItemCode : 0;
    char text[4];
    for (int i = 0; i < 4; ++i) text[i] = (char)(code >> (i * 8));
    out << "{\"gid\":" << item->nItemNum << ",\"classid\":" << item->nTxtFileNo
        << ",\"itemcode\":" << code << ",\"code\":";
    quoted(out, std::string(text, 4));
    out << ",\"x\":" << (item->path ? item->path->x : 0)
        << ",\"y\":" << (item->path ? item->path->y : 0)
        << ",\"page\":" << (unsigned)D2ItemGetPage(item)
        << ",\"mode\":" << item->mode
        << ",\"quantity\":" << D2GetPlayerStat(item, 70, 0)
        << ",\"width\":" << (definition ? (unsigned)definition->InvWidth : 0)
        << ",\"height\":" << (definition ? (unsigned)definition->InvHeight : 0)
        << ",\"sockets\":" << D2GetPlayerStat(item, STATS_ITEM_NUMSOCKETS, 0);
    Inventory* socketInventory = *(Inventory**)((DWORD)item + shifting.ptInventory);
    if (socketInventory && socketInventory->tag != 0x01020304) socketInventory = NULL;
    out << ",\"socketCounter\":" << (socketInventory ? socketInventory->currentUsedSocket : 0)
        << ",\"socketChildren\":[";
    std::set<Unit*> seen;
    DWORD used = 0;
    for (Unit* child = socketInventory ? D2InventoryGetFirstItem(socketInventory) : NULL;
        child && seen.size() < 16 && seen.insert(child).second; child = D2UnitGetNextItem(child)) {
        if (child->nUnitType != UNIT_ITEM || !child->ptItemData) continue;
        if (used) out << ',';
        ItemsBIN* childDefinition = D2GetItemsBIN(child->nTxtFileNo);
        DWORD childCode = childDefinition ? childDefinition->ItemCode : 0;
        char childText[4];
        for (int i = 0; i < 4; ++i) childText[i] = (char)(childCode >> (i * 8));
        out << "{\"gid\":" << child->nItemNum << ",\"classid\":" << child->nTxtFileNo
            << ",\"itemcode\":" << childCode << ",\"code\":";
        quoted(out, std::string(childText, 4));
        out << ",\"mode\":" << child->mode << '}';
        ++used;
    }
    out << "],\"usedSockets\":" << used << '}';
}

void listItems(std::ostream& out, Unit* first, Inventory* owned, int page)
{
    out << '[';
    std::set<Unit*> seen;
    bool comma = false;
    for (Unit* item = first; item && seen.size() < 8192 && seen.insert(item).second; item = D2UnitGetNextItem(item)) {
        if (item->nUnitType != UNIT_ITEM || !item->ptItemData) continue;
        if (owned && item->ptItemData->ptInventory != owned) continue;
        if (page >= 0 && D2ItemGetPage(item) != page) continue;
        if (comma) out << ',';
        itemState(out, item);
        comma = true;
    }
    out << ']';
}

void pageStates(std::ostream& out, Unit* ptChar, Stash* first)
{
    out << '[';
    std::set<Stash*> seen;
    bool comma = false;
    for (Stash* page = first; page && seen.size() < 8192 && seen.insert(page).second; page = page->nextStash) {
        if (comma) out << ',';
        out << "{\"id\":" << page->id << ",\"flags\":" << page->flags
            << ",\"shown\":" << (page == PCPY->currentStash ? "true" : "false") << ",\"name\":";
        quoted(out, page->name ? page->name : "");
        out << ",\"items\":";
        bool shown = page == PCPY->currentStash;
        listItems(out, shown ? D2InventoryGetFirstItem(PCInventory) : page->ptListItem, shown ? PCInventory : NULL, 4);
        out << '}';
        comma = true;
    }
    out << ']';
}

void nearbyObjects(std::ostream& out, Unit* ptChar)
{
    out << '[';
    Room* room = D2GetRoom(ptChar);
    std::set<Room*> rooms;
    std::set<Unit*> units;
    if (room) rooms.insert(room);
    if (room && room->ptNearRooms && room->nbNearRooms > 0 && room->nbNearRooms <= 64)
        for (int i = 0; i < room->nbNearRooms; ++i)
            if (room->ptNearRooms[i]) rooms.insert(room->ptNearRooms[i]);
    bool comma = false;
    for (std::set<Room*>::iterator r = rooms.begin(); r != rooms.end(); ++r) {
        std::set<Unit*> seen;
        for (Unit* unit = (*r)->ptFirstUnit; unit && seen.size() < 8192 && seen.insert(unit).second; unit = unit->ptNextUnitInRoom) {
            if (unit->nUnitType != UNIT_OBJECT || !units.insert(unit).second) continue;
            Position position = {};
            D2GetPosition(unit, &position);
            if (comma) out << ',';
            out << "{\"gid\":" << unit->nItemNum << ",\"classid\":" << unit->nTxtFileNo
                << ",\"x\":" << position.x << ",\"y\":" << position.y << '}';
            comma = true;
        }
    }
    out << ']';
}

void dumpState(Unit* ptChar, const char* base, const char* suffix)
{
    std::ostringstream out;
    out << "{\"pid\":" << GetCurrentProcessId() << ",\"tick\":" << GetTickCount()
        << ",\"gameMode\":" << ClientGameMode << ",\"ready\":" << (ptChar ? "true" : "false");
    if (ptChar && ptChar->nUnitType == UNIT_PLAYER && PCPlayerData && PCInventory) {
        Position position = {};
        D2GetPosition(ptChar, &position);
        out << ",\"playerGid\":" << ptChar->nItemNum << ",\"name\":";
        quoted(out, std::string(PCPlayerData->name, strnlen(PCPlayerData->name, 16)));
        out << ",\"carriedGold\":" << D2GetPlayerStat(ptChar, STATS_GOLD, 0)
            << ",\"bankGold\":" << D2GetPlayerStat(ptChar, STATS_GOLDBANK, 0);
        out << ",\"x\":" << position.x << ",\"y\":" << position.y << ",\"objects\":";
        nearbyObjects(out, ptChar);
        out << ",\"inventory\":";
        listItems(out, D2InventoryGetFirstItem(PCInventory), PCInventory, -1);
        out << ",\"cube\":";
        listItems(out, D2InventoryGetFirstItem(PCInventory), PCInventory, 3);
        // Native getter 0x63C1E0 checks the inventory tag and reads +0x20.
        Unit* cursor = PCInventory->tag == 0x01020304 ? PCInventory->inventory1 : NULL;
        out << ",\"cursor\":";
        if (cursor && cursor->nUnitType == UNIT_ITEM && cursor->ptItemData) itemState(out, cursor);
        else out << "null";
        if (active_PlayerCustomData) {
            out << ",\"clientProto\":" << (unsigned)PCPY->clientProto
                << ",\"serverProto\":" << (unsigned)PCPY->serverProto
                << ",\"sharedGold\":" << PCPY->sharedGold << ",\"self\":";
            pageStates(out, ptChar, PCPY->selfStash);
            out << ",\"shared\":";
            pageStates(out, ptChar, PCPY->sharedStash);
        }
    }
    out << "}\n";
    writeResultAt(base, suffix, out.str());
}

int hexDigit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void command(Unit* ptChar)
{
    char path[MAX_PATH], claimed[MAX_PATH];
    if (!testPath(path, "request") || !testPath(claimed, "processing")) return;
    // Claim before reading. A killed process never replays a .processing file.
    if (!MoveFileExA(path, claimed, MOVEFILE_REPLACE_EXISTING)) return;
    std::string token, result = "invalid";
    std::vector<BYTE> packet;
    std::vector<std::vector<BYTE> > packets;
    size_t byteCount = 0;
    HANDLE file = CreateFileA(claimed, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        char bytes[16384];
        DWORD size = GetFileSize(file, NULL), got = 0;
        if (size < sizeof(bytes) && ReadFile(file, bytes, size, &got, NULL) && got == size) {
            std::string input(bytes, got);
            size_t newline = input.find('\n');
            if (newline != std::string::npos && newline > 0 && newline <= 128) {
                token = input.substr(0, newline);
                if (!token.empty() && token[token.size() - 1] == '\r') token.resize(token.size() - 1);
                bool valid = !token.empty();
                int high = -1;
                for (size_t i = newline + 1; valid && i < input.size(); ++i) {
                    unsigned char c = (unsigned char)input[i];
                    if (isspace(c)) continue;
                    if (c == '|') {
                        if (high >= 0 || packet.empty() || packets.size() >= 15) valid = false;
                        else {
                            byteCount += packet.size();
                            packets.push_back(packet);
                            packet.clear();
                        }
                        continue;
                    }
                    int n = hexDigit(c);
                    if (n < 0) valid = false;
                    else if (high < 0) high = n;
                    else { packet.push_back((BYTE)((high << 4) | n)); high = -1; }
                    if (packet.size() > 256) valid = false;
                }
                valid = valid && high < 0 && !packet.empty();
                if (valid) {
                    byteCount += packet.size();
                    packets.push_back(packet);
                    if (!ptChar || ClientGameMode != 0) result = "not_ready";
                    else if (!consumedTokens.insert(token).second) result = "duplicate";
                    else {
                        // Parse the whole batch before dispatch. Separate native sends
                        // give every request its own revision stamp and counter.
                        for (size_t i = 0; i < packets.size(); ++i)
                            D2SendToServer((DWORD)packets[i].size(), 1, &packets[i][0]);
                        result = "sent";
                    }
                }
            }
        }
        CloseHandle(file);
    }
    DeleteFileA(claimed);
    std::ostringstream response;
    response << "{\"token\":";
    quoted(response, token);
    response << ",\"result\":";
    quoted(response, result);
    response << ",\"bytes\":" << byteCount << ",\"packets\":" << packets.size() << "}\n";
    writeResult("response", response.str());
}
}

void SharedStashTestClientFrame()
{
    try {
        if (!configured) {
            configured = true;
            if (!configureDirectory(directory)) return;
        }
        if (!directory[0] || version_D2Client != V114d) return;
        DWORD now = GetTickCount();
        if (now - lastFrame < 100) return;
        lastFrame = now;
        Unit* ptChar = D2GetClientPlayer();
        command(ptChar);
        dumpState(ptChar, directory, "state.json");
    } catch (...) {
        // Test instrumentation must not unwind through a native game frame.
        log_msg("Shared stash test client frame failed.\n");
    }
}

void SharedStashTestServerFrame(Unit* character)
{
    // Separate storage from the client frame: neither thread reads the other's
    // character, configuration, timestamps, request tokens, or output buffer.
    static char serverDirectory[MAX_PATH] = {};
    static bool serverConfigured = false;
    static DWORD lastServerFrame = 0;
    try {
        if (!serverConfigured) {
            serverConfigured = true;
            if (!configureDirectory(serverDirectory)) return;
        }
        if (!serverDirectory[0] || version_D2Game != V114d) return;
        installFixtureHandler();
        DWORD now = GetTickCount();
        if (now - lastServerFrame < 100) return;
        lastServerFrame = now;
        dumpState(character, serverDirectory, "server.json");
    } catch (...) {
        log_msg("Shared stash test server frame failed.\n");
    }
}
#endif
