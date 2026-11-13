#include "../src/plugy/sharedStashItemPacketRules.h"
#include <stdio.h>

static int failed = 0;
#define CHECK(value) do { if (!(value)) { printf("FAIL line %d: %s\n", __LINE__, #value); ++failed; } } while (0)

static void compact(DWORD value, DWORD bits)
{
	BYTE packet[34] = {0x3E};
	DWORD prefix = bits == 8 ? 1 : 2;
	if (bits != 8) packet[2] = bits == 16 ? 1 : 3;
	for (DWORD bit = 0; bit < bits; ++bit)
		packet[2 + (prefix + bit) / 8] |= (BYTE)(((value >> bit) & 1) << ((prefix + bit) % 8));
	packet[1] = (BYTE)(2 + (prefix + bits + 7) / 8);
	DWORD gid = 0;
	CHECK(sharedStashItemPacket(packet, sizeof(packet), &gid));
	CHECK(gid == value);
	CHECK(!sharedStashItemPacket(packet, packet[1] - 1, &gid));
	--packet[1];
	CHECK(!sharedStashItemPacket(packet, sizeof(packet), &gid));
}

int main()
{
	BYTE exit[2] = {0x06, 0};
	CHECK(sharedStashExitPacket(exit, 1));
	CHECK(!sharedStashExitPacket(exit, 0));
	CHECK(!sharedStashExitPacket(exit, 2));
	CHECK(!sharedStashExitPacket(NULL, 1));
	for (unsigned id = 0; id <= 0xFF; ++id) {
		exit[0] = (BYTE)id;
		CHECK(sharedStashExitPacket(exit, 1) == (id == 0x06));
	}
	for (BYTE id = 0x16; id <= 0x38; ++id) CHECK(sharedStashActionPacket(id));
	CHECK(sharedStashActionPacket(0x3A));
	CHECK(sharedStashActionPacket(0x3E));
	CHECK(sharedStashActionPacket(0x44));
	CHECK(sharedStashActionPacket(0x4F));
	CHECK(sharedStashActionPacket(0x55));
	CHECK(!sharedStashActionPacket(0x15));
	CHECK(!sharedStashActionPacket(0x39));
	CHECK(!sharedStashActionPacket(0x3B));
	CHECK(!sharedStashActionPacket(0x50));
	CHECK(!sharedStashActionPacket(0x56));
	compact(0, 8);
	compact(255, 8);
	compact(256, 16);
	compact(65535, 16);
	compact(65536, 32);
	compact(0x89ABCDEF, 32);
	compact(0xFFFFFFFF, 32);
	DWORD value = 0x12345678, gid = 0;
	BYTE item[13] = {0x9C, 4, 8};
	memcpy(item + 4, &value, 4);
	CHECK(sharedStashItemPacket(item, 8, &gid) && gid == value);
	CHECK(!sharedStashItemPacket(item, 7, &gid));
	item[0] = 0x9D; item[1] = 0x13; item[2] = 13;
	CHECK(sharedStashItemPacket(item, sizeof(item), &gid) && gid == value);
	item[1] = 0x25;
	CHECK(!sharedStashItemPacket(item, sizeof(item), &gid));
	item[1] = 0x28;
	CHECK(!sharedStashItemPacket(item, sizeof(item), &gid));
	item[1] = 0x13; item[2] = 12;
	CHECK(!sharedStashItemPacket(item, sizeof(item), &gid));
	BYTE remove[6] = {0x0A, 4};
	memcpy(remove + 2, &value, 4);
	CHECK(sharedStashItemPacket(remove, sizeof(remove), &gid) && gid == value);
	remove[1] = 0;
	CHECK(!sharedStashItemPacket(remove, sizeof(remove), &gid));
	remove[0] = 0x42;
	CHECK(!sharedStashItemPacket(remove, sizeof(remove), &gid));
	BYTE state[8] = {0x3F};
	memcpy(state + 2, &value, 4);
	CHECK(sharedStashItemPacket(state, sizeof(state), &gid) && gid == value);
	CHECK(!sharedStashItemPacket(state, 7, &gid));
	CHECK(!sharedStashItemPacket(NULL, 8, &gid));
	CHECK(!sharedStashItemPacket(state, sizeof(state), NULL));
	printf("Shared stash item packet rules: %s\n", failed ? "FAILED" : "passed");
	return failed ? 1 : 0;
}
