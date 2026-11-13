/*=================================================================
	File created by Yohann NICOLAS.
	Add support 1.13d by L'Autour.
	Add support 1.14d by haxifix.

	Updating server.

=================================================================*/

#include "updateServer.h"
#include "statsPoints.h"
#include "skillsPoints.h"
#include "infinityStash.h"
#include "stashProtocol.h"
#include "commands.h"
#include "serverMessageRules.h"
#include "common.h"


void updateServer(WORD p)
{
	if (!onRealm)
		D2SendToServer3(0x3A, p);
}


// Only single player runs US_RENAME, so these hold its one client's name: a stream starts over when another unit pointer
// sends one, and keeps one character past a name's length so an over-long name is refused, not cut.
static Unit* renameUnit = NULL;
static int renameIndex = 0;
static char renameString[RENAME_NAME_MAX + 2];

// Any client can send these packets, so a message runs only when the feature that owns it is active, and a message
// no feature owns is refused.
static bool isServerUpdateAllowed(Unit* ptChar, int type)
{
	switch(type)
	{
		case US_UNASSIGN_STR_POINT :
		case US_UNASSIGN_ENE_POINT :
		case US_UNASSIGN_DEX_POINT :
		case US_UNASSIGN_VIT_POINT :
		case US_UNASSIGN_STR_POINTS :
		case US_UNASSIGN_ENE_POINTS :
		case US_UNASSIGN_DEX_POINTS :
		case US_UNASSIGN_VIT_POINTS :	return active_StatsPoints;
		case US_UNASSIGN_SKILLS :		return active_SkillsPoints;

		case US_SELECT_SELF :
		case US_SELECT_SHARED :
		case US_SWAP0_TOGGLE :			return active_sharedStash && active_multiPageStash && hasCurrentStash(ptChar);

		case US_SET_INDEX :
		case US_SET_MAIN_INDEX :
		case US_RESET_INDEX :
		case US_SELECT_PREVIOUS :
		case US_SELECT_NEXT :
		case US_SELECT_PREVIOUS_INDEX :
		case US_SELECT_NEXT_INDEX :
		case US_SELECT_PREVIOUS2 :
		case US_SELECT_NEXT2 :
		case US_SELECT_PREVIOUS_INDEX2 :
		case US_SELECT_NEXT_INDEX2 :
		case US_INSERT_PAGE :
		case US_DELETE_PAGE :
		case US_SWAP3 :
		case US_SWAP2 :
		case US_SWAP1 :
		case US_SWAP0 :
		case US_PAGENAME :				return active_multiPageStash && hasCurrentStash(ptChar);

#ifdef PLUGY_TEST_HOOKS
		case US_TEST1 :
		case US_TEST2 :					return active_multiPageStash && hasCurrentStash(ptChar);
#endif

		// The handshake only reports state, so scripts can probe for the stash protocol whatever is enabled.
		case US_PY_HELLO :
		case US_PY_CLIENT_HELLO :
		case US_PY_CLIENT_GRID :		return true;

		// Shared gold is saved with the shared stash, so it moves only while that stash is loaded.
		case US_PUTGOLD :
		case US_TAKEGOLD :				return active_sharedGold && hasStashData(ptChar) && PCPY->sharedStashIsOpened;

		case US_SAVE :					return ptChar && active_savegame;
		// The game indexes each multiplayer client by the name it joined with and finds it by its current name when it
		// leaves (1.14d disassembly), so a rename there ends the host or leaves a stale entry.
		case US_RENAME :				return ptChar && PCPlayerData && isSinglePlayerGameMode(ClientGameMode);

		default :						return false;
	}
}

#ifdef PLUGY_TEST_HOOKS
// Test builds only (live test T27): moves one item of a hidden personal page onto the cells of that page's first
// item, so the next switch to the page can't put it back. lastItem picks the list's last item, else its second.
static void testOverlapStashItem(Unit* ptChar, DWORD pageId, bool lastItem)
{
	Stash* page = PCPY->selfStash;
	while (page && page->id != pageId)
		page = page->nextStash;
	if (!page || page == PCPY->currentStash)
	{
		log_msg("TEST hook refused: personal page %u is missing or shown\n", pageId + 1);
		return;
	}
	Unit* first = page->ptListItem;
	Unit* target = first ? D2UnitGetNextItem(first) : NULL;
	if (!target)
	{
		log_msg("TEST hook refused: personal page %u holds fewer than 2 items\n", pageId + 1);
		return;
	}
	while (lastItem && D2UnitGetNextItem(target))
		target = D2UnitGetNextItem(target);
	log_msg("TEST hook: page %u %s item %u moved from (%u,%u) onto item %u at (%u,%u)\n", pageId + 1, lastItem ? "last" : "second",
		target->nItemNum, target->path->x, target->path->y, first->nItemNum, first->path->x, first->path->y);
	target->path->x = first->path->x;
	target->path->y = first->path->y;
}
#endif

int STDCALL handleServerUpdate(Unit* ptChar, WORD param)
{
	int type = param & 0xFF;
	DWORD arg = (param & 0xFF00) >> 8;
	if (isVanillaServerMessage(type))
		return 0;
	log_msg("Received custom message: type=%i, arg=%i\n", type, arg);
	if (!isServerUpdateAllowed(ptChar, type))
	{
		// A refused message ends the stream it belongs to, so the next one starts clean.
		if (type == US_RENAME)
			renameIndex = 0;
		else if (hasStashData(ptChar))
		{
			if (type >= US_SWAP3 && type <= US_SWAP0_TOGGLE)
				PCPY->pageSwap = 0;
			if (type == US_PAGENAME)
				PCPY->pageNameLength = 0;
		}
		log_msg("Custom message refused: type=%i (unknown, feature disabled, no current stash or not single player, game mode %u)\n", type, ClientGameMode);
		return 1;
	}
	switch(type)
	{
		case US_UNASSIGN_STR_POINT :	UnassignStrPoint( ptChar ); return 1;
		case US_UNASSIGN_ENE_POINT :	UnassignEnePoint( ptChar ); return 1;
		case US_UNASSIGN_DEX_POINT :	UnassignDexPoint( ptChar ); return 1;
		case US_UNASSIGN_VIT_POINT :	UnassignVitPoint( ptChar ); return 1;
		case US_UNASSIGN_STR_POINTS :	UnassignStrPoints( ptChar ); return 1;
		case US_UNASSIGN_ENE_POINTS :	UnassignEnePoints( ptChar ); return 1;
		case US_UNASSIGN_DEX_POINTS :	UnassignDexPoints( ptChar ); return 1;
		case US_UNASSIGN_VIT_POINTS :	UnassignVitPoints( ptChar ); return 1;
		case US_UNASSIGN_SKILLS :		UnassignAllSkillsPoints( ptChar ); return 1;

		case US_SET_INDEX:				setCurrentStashIndex(ptChar, 1); return 1;
		case US_SET_MAIN_INDEX:			setCurrentStashIndex(ptChar, 2); return 1;
		case US_RESET_INDEX:			setCurrentStashIndex(ptChar, 0); return 1;
		case US_SELECT_PREVIOUS :		selectPreviousStash( ptChar ); return 1;
		case US_SELECT_NEXT :			selectNextStash( ptChar ); return 1;
		case US_SELECT_SELF :			toggleToSelfStash( ptChar ); return 1;
		case US_SELECT_SHARED :			toggleToSharedStash( ptChar ); return 1;
		case US_SELECT_PREVIOUS_INDEX :	selectPreviousIndexStash( ptChar ); return 1;
		case US_SELECT_NEXT_INDEX :		selectNextIndexStash( ptChar ); return 1;
		case US_SELECT_PREVIOUS2 :		selectPrevious2Stash( ptChar ); return 1;
		case US_SELECT_NEXT2 :			selectNext2Stash( ptChar ); return 1;
		case US_SELECT_PREVIOUS_INDEX2:	selectPreviousIndex2Stash( ptChar ); return 1;
		case US_SELECT_NEXT_INDEX2 :	selectNextIndex2Stash( ptChar ); return 1;
		// The client changes its own pages on UC_PAGE_OP, before the select that shows the new page and before the
		// relocation reports that name page ids as they are after the delete's shift.
		case US_INSERT_PAGE:
			insertStash(ptChar);
			if (serverRunsPageOps(ptChar))
				sendPageOp(ptChar, PY_PAGE_OP_INSERT, PCPY->currentStash);
			selectNextStash(ptChar);
			return 1;
		case US_DELETE_PAGE:
			if (deleteStash(ptChar, false))
			{
				if (serverRunsPageOps(ptChar))
					sendPageOp(ptChar, PY_PAGE_OP_DELETE, PCPY->currentStash);
				flushPendingMoves(ptChar);
			}
			return 1;

		case US_SAVE :					savePlayers( ptChar ); return 1;
		case US_PY_HELLO :				answerStashHello(ptChar, (BYTE)arg); return 1;
		case US_PY_CLIENT_HELLO :		setStashClientProto(ptChar, (BYTE)arg); return 1;
		case US_PY_CLIENT_GRID :		setStashClientGrid(ptChar, (BYTE)arg); return 1;
#ifdef PLUGY_TEST_HOOKS
		case US_TEST1 :					testOverlapStashItem(ptChar, arg, true); return 1;
		case US_TEST2 :					testOverlapStashItem(ptChar, arg, false); return 1;
#endif

		case US_PUTGOLD :				putGold(ptChar, 0); return 1;
		case US_TAKEGOLD :				takeGold(ptChar, 0); return 1;
		case US_SWAP3 :					PCPY->pageSwap = arg << 24; return 1;
		case US_SWAP2 :					PCPY->pageSwap |= arg << 16; return 1;
		case US_SWAP1 :					PCPY->pageSwap |= arg << 8; return 1;
		case US_SWAP0:					swapStash(ptChar, PCPY->pageSwap | arg, false); PCPY->pageSwap = 0; return 1;
		case US_SWAP0_TOGGLE :			swapStash(ptChar, PCPY->pageSwap | arg, true); PCPY->pageSwap = 0; return 1;
		case US_RENAME :
			if (renameUnit != ptChar)
			{
				renameUnit = ptChar;
				renameIndex = 0;
			}
			if (renameIndex == 0)
				ZeroMemory(renameString, sizeof(renameString));
			if (arg != NULL && renameIndex < RENAME_NAME_MAX + 1)
				renameString[renameIndex++] = (char)arg;
			if (arg == NULL)
			{
				renameString[renameIndex] = NULL;
				renameIndex = 0;
				// The client checks the name the same way, so only a crafted message lands here.
				if (!isRenameCharName(renameString))
				{
					log_msg("Rename on Server refused : '%s' is not 2 to 15 letters or underscores\n", renameString);
					return 1;
				}
				log_msg("Rename on Server : %s -> %s\n", PCPlayerData->name, renameString);
				strcpy(PCPlayerData->name, renameString);
				strcpy(PCPlayerData->ptNetClient->name, renameString);
			}
			return 1;
		case US_PAGENAME:
			if (PCPY->pageNameLength == 0)
				ZeroMemory(PCPY->pageName, sizeof(PCPY->pageName));
			if (arg != NULL && PCPY->pageNameLength < sizeof(PCPY->pageName) - 1)
				PCPY->pageName[PCPY->pageNameLength++] = (char)arg;
			if (arg == NULL)
			{
				PCPY->pageName[PCPY->pageNameLength] = NULL;
				PCPY->pageNameLength = 0;
				log_msg("Rename current page on Server : %s -> %s\n", PCPY->currentStash->name, PCPY->pageName);
				renameCurrentStash(ptChar, PCPY->pageName);
			}
			return 1;
		default :
			return 0;
	}
}

FCT_ASM( caller_handleServerUpdate_114)
	PUSH ESI
	PUSH EBX
	CALL handleServerUpdate
	TEST EAX,EAX
	JNZ END_RCM
	MOV EAX,ESI
	AND EAX,0xFF
	SHR ESI,8
	MOV EDI,EAX
	RETN
END_RCM:
	// The 1.14d handler saves EBP, EBX, ESI, EDI. Drop only this hook's return address.
	ADD ESP,4
	POP EDI
	POP ESI
	XOR EAX,EAX
	POP EBX
	POP EBP
	RETN 8
}}

FCT_ASM( caller_handleServerUpdate)
	PUSH ESI
	PUSH EBX
	CALL handleServerUpdate
	TEST EAX,EAX
	JNZ END_RCM
	MOV EAX,ESI
	AND EAX,0xFF
	SHR ESI,8
	MOV EDI,EAX
	RETN
END_RCM:
	ADD ESP,4
	POP EDI
	POP ESI
	XOR EAX,EAX
	POP EBX
	RETN 8
}}


FCT_ASM( caller_handleServerUpdate_9)
	XOR EDX,EDX
	MOV DX,WORD PTR DS:[EAX+1]
	PUSH ECX
	PUSH EDX
	PUSH EDX
	PUSH ECX
	CALL handleServerUpdate
	POP EDX
	POP ECX
	TEST EAX,EAX
	JNZ END_RCM
	RETN
END_RCM:
	POP EAX
	XOR EAX,EAX
	RETN 8
}}


void Install_UpdateServer()
{
	static int isInstalled = false;
	if (isInstalled) return;

	log_msg("Patch D2Game for received button click message. (UpdateServer)\n");

	// manage button click message from Client.
	mem_seek R8(D2Game, 4A702, 4AAC2, 56EA2, 54AE3, 2C773, 975C3, CC983, 676C3, 14BD38);
	if (version_D2Game >= V111) {
		memt_byte( 0xC1, 0x57 );	// PUSH EDI
		memt_byte( 0xEE, 0xE8 );	// CALL caller_handleServerUpdate
		MEMT_REF4( 0xF88B5708, version_D2Game == V114d ? caller_handleServerUpdate_114 : caller_handleServerUpdate);
		//01FD4AE3   . C1EE 08        SHR ESI,8
		//01FD4AE6   . 57             PUSH EDI
		//01FD4AE7   . 8BF8           MOV EDI,EAX
		//01F9C773   . C1EE 08        SHR ESI,8
		//01F9C776   . 57             PUSH EDI
		//01F9C777   . 8BF8           MOV EDI,EAX
		//6FCB75C3   . C1EE 08        SHR ESI,8
		//6FCB75C6   . 57             PUSH EDI
		//6FCB75C7   . 8BF8           MOV EDI,EAX
		//6FCEC983   . C1EE 08        SHR ESI,8
		//6FCEC986   . 57             PUSH EDI
		//6FCEC987   . 8BF8           MOV EDI,EAX
		//066A76C3  |.  C1EE 08       SHR ESI,8
		//066A76C6  |.  57            PUSH EDI
		//066A76C7  |.  8BF8          MOV EDI,EAX
		//0054BD38  |. C1EE 08        SHR ESI,8
		//0054BD3B  |. 57             PUSH EDI
		//0054BD3C  |. 8BF8           MOV EDI,EAX
	} else if (version_D2Game == V110) {
		memt_byte( 0xC1, 0xE8 );	// CALL caller_handleServerUpdate
		MEMT_REF4( 0xF88B08EE, caller_handleServerUpdate);
		//6FC86EA2   . C1EE 08        SHR ESI,8
		//6FC86EA5   . 8BF8           MOV EDI,EAX
	} else {
		memt_byte( 0x33, 0xE8 );	// CALL caller_handleServerUpdate
		MEMT_REF4( 0x508B66D2, caller_handleServerUpdate_9);
		memt_byte( 0x01, 0x90 );	// NOP
		//6FC7A702   . 33D2           XOR EDX,EDX
		//6FC7A704   . 66:8B50 01     MOV DX,WORD PTR DS:[EAX+1]
	}

	log_msg("\n");

	isInstalled = true;
}

/*================================= END OF FILE =================================*/
