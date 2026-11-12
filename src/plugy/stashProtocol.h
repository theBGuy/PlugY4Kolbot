/*=================================================================
	Stash protocol: requests a script sends to its own game server (packet PY_STASH_PACKET and the 0x3A
	US_PY_* handshake) and the server's 0x9D answers (UC_PROTO, UC_ITEM_MOVED, UC_STASH_PAGE, UC_ACK, UC_PAGE_OP). 1.14d only.

=================================================================*/
#pragma once

#include "common.h"

extern bool active_stashProtocol;

// UC_ACK +0B feature bits
#define PY_FEATURE_MULTIPAGE		0x01
#define PY_FEATURE_SHARED			0x02	// ActiveSharedStash
#define PY_FEATURE_SHARED_OPENED	0x04	// this game loaded the shared stash
#define PY_FEATURE_SELF_OPENED		0x08
#define PY_FEATURE_PACKET			0x10	// PY_STASH_PACKET reaches handleStashPacket; never send it otherwise
#define PY_FEATURE_PINNING			0x20	// swaps keep pinned items on their page: PinnedItems set, protocol on, client confirmed
#define PY_FEATURE_SHARED_CONFLICT	0x40
#define PY_FEATURE_SERVER_PAGE_OPS	0x80	// page insert/delete (raw 0x3A packets too) keeps this client's pages in step: serverRunsPageOps

// UC_ACK +1C page flags; the low 3 bits are the page's own Stash flags
#define PY_PAGE_SHARED		0x001
#define PY_PAGE_INDEX		0x002
#define PY_PAGE_MAIN_INDEX	0x004
#define PY_PAGE_SHOWN		0x100	// the reported page is the page shown
#define PY_PAGE_UNCHANGED	0x200	// SELECT: the page was already shown, so nothing was sent or switched

#pragma pack(1)
// The one answer to each stash request and to US_PY_HELLO.
struct PYStashAck // size=0x24
{
	BYTE	mType;				//+00 0x9D
	BYTE	mFunc;				//+01 UC_ACK
	BYTE	mSize;				//+02 0x24
	BYTE	seq;				//+03 the request's seq
	DWORD	zero1;				//+04 D2BS reads an item gid here (itemaction event), so it stays 0
	BYTE	op;					//+08 the request's op, PY_OP_HELLO for US_PY_HELLO
	BYTE	status;				//+09 PY_STATUS_*
	BYTE	proto;				//+0A PY_STASH_PROTO
	BYTE	features;			//+0B PY_FEATURE_*
	BYTE	gridX;				//+0C server stash grid
	BYTE	gridY;				//+0D
	BYTE	clientProto;		//+0E what the client PlugY confirmed, 0 = nothing
	BYTE	clientGridMatch;	//+0F 1 when the client confirmed the server's grid
	DWORD	gid;				//+10 MOVE: the item; LIST: pages sent
	BYTE	x;					//+14
	BYTE	y;					//+15
	WORD	zero2;				//+16
	DWORD	page;				//+18 MOVE: the item's final page; others: the page shown
	DWORD	pageFlags;			//+1C PY_PAGE_*
	WORD	nbSelfPages;		//+20
	WORD	nbSharedPages;		//+22
};

// Sent at join and before each US_PY_HELLO answer; a client PlugY answers US_PY_CLIENT_HELLO and US_PY_CLIENT_GRID.
struct PYStashProto // size=0x24
{
	BYTE	mType;				//+00 0x9D
	BYTE	mFunc;				//+01 UC_PROTO
	BYTE	mSize;				//+02 0x24
	BYTE	zero1;				//+03
	DWORD	zero2;				//+04 kept 0 like UC_ACK +04
	BYTE	proto;				//+08 PY_STASH_PROTO
	BYTE	nbPinCodes;			//+09
	BYTE	gridX;				//+0A server stash grid
	BYTE	gridY;				//+0B
	DWORD	flags;				//+0C PY_PROTO_*
	DWORD	pinCodes[5];		//+10 ItemsBIN codes of the pinned items
};

// UC_PROTO +0C flags
#define PY_PROTO_SERVER_PAGE_OPS	0x01	// the server runs page insert/delete itself and tells the client with UC_PAGE_OP
#define PY_PROTO_SHARED_TRANSACTIONS 0x02 // shared actions use database ownership and revision checks

// UC_PAGE_OP +08 op
#define PY_PAGE_OP_INSERT	1	// a new empty page after the page shown
#define PY_PAGE_OP_DELETE	2	// the page shown, empty, is deleted: the next page's items come into the grid

// The server changed its page list; the client runs the same change on its own list, so a later message that names a
// page id means the same page on both sides. Sent only to a client that confirmed the protocol.
struct PYPageOp // size=0x24
{
	BYTE	mType;				//+00 0x9D
	BYTE	mFunc;				//+01 UC_PAGE_OP
	BYTE	mSize;				//+02 0x24
	BYTE	zero1;				//+03
	DWORD	zero2;				//+04 kept 0 like UC_ACK +04
	BYTE	op;					//+08 PY_PAGE_OP_*
	BYTE	shared;				//+09 1 when the page shown is on the shared list
	BYTE	zero3[14];			//+0A
	DWORD	page;				//+18 id of the page shown before the op
	DWORD	zero4[2];			//+1C
};

// UC_ITEM_MOVED +08 source flags and +09 target flags
#define PY_MOVED_SHARED			0x01	// the page is on the shared list
#define PY_MOVED_SRC_SHOWN		0x02	// source only: the item left the page shown
#define PY_MOVED_SRC_UNKNOWN	0x04	// source only: the server doesn't say where the item was

// UC_ITEM_MOVED +0A reason
#define PY_MOVED_REQUEST	0	// a MOVE request moved it
#define PY_MOVED_REPAIR		1	// a MOVE found it already there: where the server holds it
#define PY_MOVED_RELOCATED	2	// a page switch couldn't put it back at its saved cells

// Where the server put a stash item. The client moves its own copy there, sent before the request's UC_ACK.
struct PYItemMoved // size=0x24
{
	BYTE	mType;				//+00 0x9D
	BYTE	mFunc;				//+01 UC_ITEM_MOVED
	BYTE	mSize;				//+02 0x24
	BYTE	seq;				//+03 the request's seq, 0 when unsolicited
	DWORD	zero1;				//+04 kept 0 like UC_ACK +04
	BYTE	sourceFlags;		//+08 PY_MOVED_SHARED | PY_MOVED_SRC_*
	BYTE	targetFlags;		//+09 PY_MOVED_SHARED
	BYTE	reason;				//+0A PY_MOVED_REQUEST / REPAIR / RELOCATED
	BYTE	zero2[5];			//+0B
	DWORD	gid;				//+10 Unit nItemNum
	BYTE	x;					//+14
	BYTE	y;					//+15
	WORD	zero3;				//+16
	DWORD	targetPage;			//+18 page id
	DWORD	sourcePage;			//+1C page id, 0xFFFFFFFF when unknown
	DWORD	zero4;				//+20
};

// One stash item of a UC_STASH_PAGE.
struct PYStashPageItem // size=0x8
{
	DWORD	gid;				//+00 Unit nItemNum
	WORD	classid;			//+04 Unit nTxtFileNo
	BYTE	x;					//+06
	BYTE	y;					//+07
};

// The client's 0x9D handler copies at most 0x101 bytes and the size field is one byte, so a page's items go in chunks.
#define PY_PAGE_ITEMS_PER_CHUNK	26

// LIST (PY_OP_LIST) answer: one or more per listed page, then the request's UC_ACK. Sent mSize bytes long:
// the header and nbItems items.
struct PYStashPage // size=0xF8 at most
{
	BYTE	mType;				//+00 0x9D
	BYTE	mFunc;				//+01 UC_STASH_PAGE
	BYTE	mSize;				//+02 0x28 + 8 * nbItems
	BYTE	seq;				//+03 the request's seq
	DWORD	zero1;				//+04 kept 0 like UC_ACK +04
	DWORD	page;				//+08 page id
	DWORD	pageFlags;			//+0C PY_PAGE_* (PY_PAGE_SHOWN: the page shown)
	BYTE	chunkIndex;			//+10 0 .. chunkCount-1
	BYTE	chunkCount;			//+11 1 for an empty page
	BYTE	nbItems;			//+12
	BYTE	zero2;				//+13
	char	name[20];			//+14 NUL padded when shorter (a 20-character name has no NUL), empty when unnamed
	PYStashPageItem items[PY_PAGE_ITEMS_PER_CHUNK];	//+28
};
#pragma pack()
// the wire format: scripts read these offsets
C_ASSERT(sizeof(PYStashAck) == 0x24);
C_ASSERT(sizeof(PYStashProto) == 0x24);
C_ASSERT(sizeof(PYPageOp) == 0x24);
C_ASSERT(FIELD_OFFSET(PYPageOp, page) == 0x18);
C_ASSERT(sizeof(PYItemMoved) == 0x24);
C_ASSERT(sizeof(PYStashPageItem) == 0x8);
C_ASSERT(FIELD_OFFSET(PYStashPage, items) == 0x28);
C_ASSERT(sizeof(PYStashPage) == 0xF8);

void Install_StashProtocol();
// PYPlayerData exists only while PlayerCustomData is installed: PCPY is an offset past PlayerData.
bool hasStashData(Unit* ptChar);
DWORD FASTCALL handleStashPacket(Game* ptGame, Unit* ptChar, BYTE* packet, DWORD size);
void sendStashAck(Unit* ptChar, BYTE op, BYTE seq, BYTE status, DWORD gid, BYTE x, BYTE y, Stash* page, DWORD extraPageFlags = 0);
void sendProto(Unit* ptChar);
// One UC_ITEM_MOVED for the item now at (x,y) of dst. src NULL: the source is unknown.
void sendItemMoved(Unit* ptChar, DWORD gid, Stash* src, bool srcWasShown, Stash* dst, BYTE x, BYTE y, BYTE reason, BYTE seq);
// MOVE (PY_OP_MOVE): answers with exactly one UC_ACK, after the UC_ITEM_MOVED of a move; returns its status.
BYTE moveStashItem(Unit* ptChar, DWORD gid, DWORD page, BYTE flags, BYTE x, BYTE y, BYTE seq);

// server side of the 0x3A handshake
void answerStashHello(Unit* ptChar, BYTE seq);
void setStashClientProto(Unit* ptChar, BYTE proto);
void setStashClientGrid(Unit* ptChar, BYTE grid);

// Server player: page insert/delete is mirrored by UC_PAGE_OP, because its client confirmed the UC_PROTO that
// announced it (that client then changes no page itself). False for any other client, which still changes its own
// pages before asking.
bool serverRunsPageOps(Unit* ptChar);
void sendPageOp(Unit* ptChar, BYTE op, Stash* shown);	// shown: the page shown before the op

// client side
void applyProtoClient(const BYTE* msg);
void applyItemMovedClient(const BYTE* msg);
void applyPageOpClient(const BYTE* msg);
// Client player: the server announced it runs page insert/delete, so the client only asks.
bool clientAwaitsPageOps(Unit* ptChar);

/*================================= END OF FILE =================================*/
