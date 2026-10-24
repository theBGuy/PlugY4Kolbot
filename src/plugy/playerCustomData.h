/*=================================================================
	File created by Yohann NICOLAS.

	Data added to Player base-stucture

=================================================================*/
#pragma once

#include "common.h"

extern bool active_PlayerCustomData;
extern bool openSharedStashOnLoading;

struct Unit;

struct Stash
{
	DWORD id;
	union {
		DWORD flags;
		struct {
			DWORD isShared:1;	//
			DWORD isIndex:1;	//
			DWORD isMainIndex:1;	//
			DWORD isReserved:1;	// For swap items comand
		};
	};
	char* name;
	Unit* ptListItem;
	Stash* previousStash;
	Stash* nextStash;
};

// Where a page switch put an item it couldn't return to its saved cells (gid = Unit nItemNum).
struct PYPendingMove
{
	DWORD	gid;
	DWORD	page;
	BYTE	shared;
	BYTE	x;
	BYTE	y;
};


struct PYPlayerData
{
	union {
		DWORD flags;
		struct {
			DWORD selfStashIsOpened:1;	//
			DWORD sharedStashIsOpened:1;//
			DWORD showSharedStash:1;	//
//			DWORD notOnRealm:1;			//set to 1 when the player is load from client or in SP
		};
	};
	DWORD	sharedGold;
	DWORD	nbSelfPages;
	DWORD	nbSharedPages;
	Stash*	currentStash;
	Stash*	selfStash;
	Stash*	sharedStash;
	DWORD	sharedBaselineHash;	// FNV-1a of the shared stash as serialized right after loading
	DWORD	sharedDiskHash;		// FNV-1a of the shared stash file as last read or written by this process
	union {
		DWORD sharedStateFlags;
		struct {
			DWORD sharedBaselineValid:1;	//
			DWORD sharedDiskExisted:1;		//
			DWORD sharedInConflict:1;		// this game's shared stash goes to its conflict file; the player was told once
		};
	};
	char	sharedConflictStamp[16];	// "YYYYMMDD-HHMMSS" of this game's conflict file, empty if none
	union {
		DWORD saveStateFlags;
		struct {
			DWORD backupDoneThisGame:1;	// backupSaveFiles already ran for this game
			DWORD sharedMovedOnLoad:1;	// loading moved shared stash items that no save has stored yet
		};
	};
	DWORD	selfItemsOnNewPages;	// items loading had to put on new pages, reported once by LoadSPCustomData
	DWORD	sharedItemsOnNewPages;	//
	// Client player: stash items its page switches couldn't place this game. The server decides where they go; until
	// it says, each stays detached in the client unit table only (see placeUnplacedItems).
	DWORD	clientDesync;
	// Server player: relocations made by page switches, for the client once it has run the same switch. One switch
	// relocates at most one page of items, and the protocol describes grids up to 15x15.
	PYPendingMove pendingMoves[225];
	BYTE	nbPendingMoves;
	BYTE	clientReady;		// the join flush has built the client, so later relocations can be reported to it
	// Stash protocol handshake. Server player: what its client PlugY confirmed (0 = nothing yet, an old PlugY or
	// none). Client player: the protocol version the server announced in UC_PROTO.
	BYTE	clientProto;
	BYTE	clientGrid;			// gridX | gridY << 4
	BYTE	serverProto;
	// Client player: the ItemsBIN codes the server pins on page swaps, from its last UC_PROTO. The client never reads
	// its own PinnedItems, so its swaps keep exactly the items the server's do.
	BYTE	nbServerPinCodes;
	DWORD	serverPinCodes[5];
	// Client player: the PY_PROTO_* flags of the server's last UC_PROTO, 0 until one arrives (an older or protocol-off
	// host): with PY_PROTO_SERVER_PAGE_OPS this client asks for page insert/delete and the server's UC_PAGE_OP does it.
	DWORD	serverProtoFlags;
	// Server player: the page swap (US_SWAP3..0, a byte each) and page name (US_PAGENAME, a character each) its client
	// is sending. Per player, so another client's messages can't mix into them.
	DWORD	pageSwap;
	BYTE	pageNameLength;
	char	pageName[21];
};

// The free of every PlayerData, which ends the session lock of the character it loaded. Install_PlayerCustomData
// installs it, and so does Install_CharacterCommit on 1.14d whatever ActiveMultiPageStash says.
void Install_PlayerDataFree();
void Install_PlayerCustomData();

/*================================= END OF FILE =================================*/