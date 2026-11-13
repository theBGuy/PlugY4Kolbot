#pragma once

#include <windows.h>
#include <string.h>

// The one-byte server command 0x06 sets the client's game-exit flag.
inline bool sharedStashExitPacket(const BYTE* packet, DWORD size)
{
	return packet && size == 1 && packet[0] == 0x06;
}

// Native inventory actions and NPC purchase/sale/repair/quest services can all
// change items owned by the player's inventory, including the shown shared page.
// Keep the client stamp filter and the server handler wrapper on this same set.
inline bool sharedStashActionPacket(BYTE id)
{
	return (id >= 0x16 && id <= 0x38) || id == 0x3A || id == 0x3E
		|| id == 0x44 || id == 0x4F || id == 0x55;
}

inline bool sharedStashItemPacket(const BYTE* packet, DWORD size, DWORD* itemGid)
{
	if (!packet || !itemGid || !size) return false;
	if ((packet[0] == 0x9C || packet[0] == 0x9D) && size >= 8 && packet[1] < 0x18) {
		if (packet[2] > size || packet[2] < (packet[0] == 0x9C ? 8 : 13)) return false;
		memcpy(itemGid, packet + 4, sizeof(*itemGid));
		return true;
	}
	if (packet[0] == 0x0A && size == 6 && packet[1] == 4) {
		memcpy(itemGid, packet + 2, sizeof(*itemGid));
		return true;
	}
	// 0x3F is an item-specific state packet; its client handler looks up unit type 4.
	if (packet[0] == 0x3F && size == 8) {
		memcpy(itemGid, packet + 2, sizeof(*itemGid));
		return true;
	}
	// 0x3E writes a compact item ID at payload bit zero: 0+8 bits, 10+16 bits, or 11+32 bits.
	if (packet[0] == 0x3E && size >= 4 && packet[1] >= 4 && packet[1] <= size) {
		DWORD prefix = (packet[2] & 1) ? 2 : 1;
		DWORD bits = prefix == 1 ? 8 : (packet[2] & 2) ? 32 : 16;
		if (prefix + bits > ((DWORD)packet[1] - 2) * 8) return false;
		DWORD gid = 0;
		for (DWORD bit = 0; bit < bits; ++bit) {
			DWORD position = prefix + bit;
			gid |= ((DWORD)((packet[2 + position / 8] >> (position % 8)) & 1)) << bit;
		}
		*itemGid = gid;
		return true;
	}
	return false;
}
