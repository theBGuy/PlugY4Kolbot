/*=================================================================
	File created by Yohann NICOLAS.

	Updating Client and Server constantes.

=================================================================*/

#pragma once

enum UpdateClientConst
{
	UC_SELECT_STASH=0x18,
	UC_SHARED_GOLD,
	UC_PAGE_NAME,
	UC_ADD_STAT,
	UC_ADD_SYNERGY,
	UC_TEST1,
	UC_TEST2,
	UC_TEST3,
	// Stash protocol messages: explicit values, a wire contract that must not move with the list above.
	UC_ITEM_MOVED=0x20,
	UC_ACK=0x21,
	UC_STASH_PAGE=0x22,
	UC_PROTO=0x23,
	UC_PAGE_OP=0x24
};

enum UpdateServerConst
{
	US_UNASSIGN_STR_POINT=0x10,//Don't change these first 8 value
	US_UNASSIGN_ENE_POINT,
	US_UNASSIGN_DEX_POINT,
	US_UNASSIGN_VIT_POINT,
	US_UNASSIGN_STR_POINTS,
	US_UNASSIGN_ENE_POINTS,
	US_UNASSIGN_DEX_POINTS,
	US_UNASSIGN_VIT_POINTS,
	US_UNASSIGN_SKILLS,
	US_SELECT_PREVIOUS,
	US_SELECT_NEXT,
	US_SELECT_SELF,
	US_SELECT_SHARED,
	US_SELECT_PREVIOUS_INDEX,
	US_SELECT_NEXT_INDEX,
	US_SELECT_PREVIOUS2,
	US_SELECT_NEXT2,
	US_SELECT_PREVIOUS_INDEX2,
	US_SELECT_NEXT_INDEX2,
	US_WORLDEVENT,
	US_SAVE,
	US_MAXGOLD,
	US_PUTGOLD,
	US_TAKEGOLD,
	US_RENAME,
	US_PAGENAME,
	US_SET_INDEX,
	US_SET_MAIN_INDEX,
	US_RESET_INDEX,
	US_INSERT_PAGE,
	US_DELETE_PAGE,
	US_SWAP3,
	US_SWAP2,
	US_SWAP1,
	US_SWAP0,
	US_SWAP0_TOGGLE,
	US_TOGGLE1,
	US_TOGGLE2,
	US_TOGGLE3,
	US_TEST1,	//For CB
	US_TEST2,	//For CB
	US_TEST3,	//For CB
	// Stash protocol handshake: explicit values, a wire contract that must not move with the list above.
	US_PY_HELLO=0x3A,			// arg = seq; the server answers UC_PROTO, then UC_ACK op PY_OP_HELLO
	US_PY_CLIENT_HELLO=0x3B,	// arg = the client's PY_STASH_PROTO
	US_PY_CLIENT_GRID=0x3C		// arg = the client's stash grid, gridX | gridY << 4
};

// Client->server stash request: PY_STASH_PACKET_SIZE bytes, little-endian: [0]=PY_STASH_PACKET [1]=op [2]=seq
// [3]=flags [4..7]=item gid (Unit nItemNum) [8..11]=page id (LIST: the first page) [12]=x [13]=y
// [14..15]=count (LIST: most pages listed, 0 = no limit), else 0.
#define PY_STASH_PACKET			0x55
#define PY_STASH_PACKET_SIZE	16
#define PY_STASH_PROTO			1

// request ops; UC_ACK echoes the op, and PY_OP_HELLO marks the answer to US_PY_HELLO
#define PY_OP_MOVE		0x01
#define PY_OP_SELECT	0x02
#define PY_OP_LIST		0x03
#define PY_OP_HELLO		0x80

// request flags (byte 3)
#define PY_FLAG_SHARED			0x01	// the target page (LIST: the listed pages) is on the shared list
#define PY_FLAG_AUTO			0x02	// MOVE: the first free spot of the target, x and y ignored
#define PY_FLAG_ALLOW_PINNED	0x04	// MOVE: a pinned item may move
#define PY_FLAG_CURRENT			0x08	// the target is the page shown (page id and PY_FLAG_SHARED ignored)
#define PY_FLAG_CROSS			0x10	// MOVE: the item may change list (personal <-> shared)
#define PY_FLAG_WITH_EMPTY		0x20	// LIST: pages without items are listed too
#define PY_FLAG_ALLOW_SHARED	0x40	// MOVE: a shared page may be the source or the target
#define PY_FLAG_RESERVED		0x80	// must be 0

// UC_ACK status (byte +09)
#define PY_STATUS_OK				0
#define PY_STATUS_NOOP				1	// already there
#define PY_STATUS_NOT_FOUND			2
#define PY_STATUS_NOT_IN_STASH		3
#define PY_STATUS_PINNED			4
#define PY_STATUS_PAGE_RANGE		5
#define PY_STATUS_NO_ROOM			6
#define PY_STATUS_FEATURE_OFF		7
#define PY_STATUS_BUSY				8	// retry after about 100 ms
#define PY_STATUS_BAD_REQUEST		9
#define PY_STATUS_CROSS_LIST		10
#define PY_STATUS_INTERNAL			11
#define PY_STATUS_CLIENT_MISMATCH	12
#define PY_STATUS_SHARED_CONFLICT	13
#define PY_STATUS_STALE				14	// refresh pages before retrying the action
#define PY_STATUS_STORE_ERROR		15	// database unavailable; no new shared action was run

/*================================= END OF FILE =================================*/
