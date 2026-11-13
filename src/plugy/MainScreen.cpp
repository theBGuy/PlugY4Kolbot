/*=================================================================
	File created by Yohann NICOLAS.
	Add support 1.13d by L'Autour.
	Add support 1.14d by haxifix.

    Print PlugY version on screen.

=================================================================*/

#include "extraOptions.h"
#include "windowed.h"
#include "common.h"
#include "../common/UpdateCheck.h"
#include "../common/SharedStashCleanup.h"
#include "infinityStash.h"
#include "updateNoticeRules.h"
#include <stdio.h>
#include <string.h>

char* versionText = "";
bool active_VersionTextChange = 0;
BYTE modVersionColor = 0;
bool active_PrintPlugYVersion = 1;
BYTE colorOfPlugYVersion = 4;

// Two centered lines (name, then version) in upstream's 200 px box, grown one line upward (y is the
// box's bottom edge): one "PlugY4Kolbot <version>" line wraps and the box shows only its last line,
// and a wider box would run into the Exit button. y is 6 px below upstream's so the version line keeps
// the "v 1.14d" baseline (measured on 1.14d at 800x600: rows 558-567 for both).
DWORD newTextBoxData[]={4,0x237,0x249,0xC8,0x28,0,0,0,0,0,0,2};//type,x,y,l,h,?,?,?,?,?,?,?(0,257,C8,28)

// Two lines ("Update available", then the version) with a gap above the name, so the notice reads as
// its own message. Lines start at y - h and sit 13 px apart (measured on 1.14d at 800x600: rows 506-514
// and 519-528, the name on 545-553).
DWORD updateTextBoxData[]={4,0x237,0x222,0xC8,0x28,0,0,0,0,0,0,2};//type,x,y,l,h,?,?,?,?,?,?,?

// The version line holds the version alone. 13 characters fit this 200 px box: the 21-character line
// "Update: 14.10.0-rc.10" drew on one line at 161 px (measured on 1.14d at 800x600), and valid release
// versions never exceed 13.
#define UPDATE_NOTICE_VERSION_MAX 13

// Main thread only (the menu build and the in-game draw step), except updateThreadHint: the check thread
// writes it, then sets updateThreadDone, and never touches it again, so it is read only after the flag.
static bool updateCheckStarted = false;
static bool updateThreadRunning = false;
static char updateNoticeVersion[UPDATE_TAG_MAX] = "";	// "" while no newer release is known
static char updateGameVersion[UPDATE_TAG_MAX] = "";		// the same release for the in-game line, which has no width cap
static UpdateHint updateThreadHint;
static volatile LONG updateThreadDone = 0;

static void setUpdateNotice(const char* version)
{
	strcpy(updateGameVersion, version);
	if (strlen(version) <= UPDATE_NOTICE_VERSION_MAX)
		strcpy(updateNoticeVersion, version);
	else
		log_msg("Update check: version %s is not shown on the main menu: longer than %d characters\n",
			version, UPDATE_NOTICE_VERSION_MAX);
}

static void logNoUsableCache()
{
	log_msg("Update check: skipped, no usable update cache in %s\n", "%LOCALAPPDATA%\\PlugY4Kolbot");
}

// module is a reference on PlugY.dll taken for this thread: PlugY.exe's exit stub calls Release() and
// then FreeLibrary(PlugY.dll), which must not unmap the code this thread is still running.
static DWORD WINAPI updateCheckThread(LPVOID module)
{
	UpdateCheckForNewer(updateChannel, PLUGY_VERSION, &updateThreadHint);
	InterlockedExchange(&updateThreadDone, 1);
	FreeLibraryAndExitThread((HMODULE)module, 0);
	return 0;
}

/* Only an answer from GitHub or a fresh cache clears the notice; a failed or skipped refresh keeps
   it, since a release never stops being newer than this build. */
static void applyUpdateHint(const UpdateHint* hint)
{
	const char* known = hint->newer ? "; last known release: " : "";
	const char* knownVersion = hint->newer ? hint->version : "";
	if (hint->result == UPDATE_BUSY)
		log_msg("Update check: skipped, another game is checking%s%s\n", known, knownVersion);
	else if (hint->result == UPDATE_CACHE_UNAVAILABLE)
		logNoUsableCache();
	else if (hint->result != UPDATE_OK && hint->result != UPDATE_NO_RELEASE)
		log_msg("Update check failed: %s (HTTP status %u, error %u)%s%s\n", UpdateResultName(hint->result),
			hint->info.httpStatus, hint->info.winError, known, knownVersion);
	else
	{
		if (hint->newer)
			log_msg("Update check: version %s is available at %s\n", hint->version, hint->pageUrl);
		else
			log_msg("Update check: up to date\n");
		updateNoticeVersion[0] = 0;
		updateGameVersion[0] = 0;
	}
	if (hint->newer)
		setUpdateNotice(hint->version);
}

// The cached release for this channel, without its 'v', when it is newer than this build (the comparison UpdateCheckForNewer makes).
static const char* newerCachedVersion(const UpdateCache* cache)
{
	UpdateVersion current, latest;
	if (cache->channel != updateChannel || !UpdateParseTag(PLUGY_VERSION, &current)
		|| !UpdateParseTag(cache->latestTag, &latest) || UpdateCompareVersions(&latest, &current) <= 0)
		return NULL;
	return cache->latestTag[0] == 'v' ? cache->latestTag + 1 : cache->latestTag;
}

/* Called by each menu build and, on 1.14d, by each in-game frame until the in-game line has printed.
   The first call decides from the cache file alone; only a stale or missing cache starts the one check
   thread, whose answer shows at the next of those calls, because a request can take minutes and the
   main thread must never wait on the network. A stale cache's newer release shows at once, whatever its
   last check's status, so a bot that never returns to the main menu still sees it. */
static void pollUpdateCheck()
{
	if (updateThreadRunning)
	{
		if (InterlockedCompareExchange(&updateThreadDone, 0, 0))
		{
			updateThreadRunning = false;
			applyUpdateHint(&updateThreadHint);
		}
		return;
	}
	if (updateCheckStarted)
		return;
	updateCheckStarted = true;

	wchar_t cachePath[MAX_PATH];
	UpdateCache cache;
	// Without a usable cache the core never asks GitHub, so no thread is started for it.
	if (!UpdateCacheChannelPath(updateChannel, cachePath, MAX_PATH))
	{
		logNoUsableCache();
		return;
	}
	bool haveCache = UpdateCacheRead(cachePath, &cache);
	const char* version = haveCache ? newerCachedVersion(&cache) : NULL;
	if (UpdateShouldCheck(haveCache ? &cache : NULL, updateChannel, UpdateUnixTimeNow()))
	{
		if (version)
		{
			log_msg("Update check: version %s is available at %s (last check; refreshing)\n", version, cache.pageUrl);
			setUpdateNotice(version);
		}
		HMODULE module;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&updateCheckThread, &module))
		{
			log_msg("Update check failed: no module reference (error %u)\n", GetLastError());
			return;
		}
		// Reserved, not committed: each of a farm's instances commits only the pages the check touches.
		HANDLE hThread = CreateThread(NULL, 256 * 1024, updateCheckThread, module, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
		if (hThread)
		{
			updateThreadRunning = true;
			CloseHandle(hThread);
		}
		else
		{
			log_msg("Update check failed: no thread (error %u)\n", GetLastError());
			FreeLibrary(module);
		}
		return;
	}

	// Fresh: a failed check within the hour keeps the last good release, which still counts.
	if (version)
	{
		log_msg("Update check: version %s is available at %s\n", version, cache.pageUrl);
		setUpdateNotice(version);
	}
	else if (!cache.ok)
		log_msg("Update check: skipped, the last check failed less than an hour ago\n");
	else
		log_msg("Update check: up to date\n");
}

/* The in-game half of the notice, called from the in-game draw step on the main menu's thread. A bot
   leaves the menu at once, and a check can answer after the menu was built, so the first game that
   knows a newer release prints one local chat line, once per process. 1.14d only: the one client
   whose chat-print address is known. */
void updateNoticeFrame()
{
	static bool shown = false;
	static UpdateGameNoticeTimer timer = {};
	if (shown || version_D2Client != V114d)
		return;
	pollUpdateCheck();
	// The player lookup runs only once a newer release is known.
	if (!updateGameNoticeStep(&timer, GetTickCount(), updateGameVersion[0] && D2GetClientPlayer()))
		return;
	WCHAR line[128];
	_snwprintf(line, 127, L"PlugY4Kolbot %hs is available. Run PlugY4KolbotUpdater.exe to update.", updateGameVersion);
	line[127] = 0;
	D2PrintGameString(line, GREEN);
	shown = true;
	log_msg("Update check: version %s announced in game\n", updateGameVersion);
}

void STDCALL printPlugYVersion(void** childrens, DWORD* sgnNumChildren)
{
	if (version_D2Game == V114d && active_multiPageStash && active_SharedStashInMultiPlayer > 0)
		if (!SharedStash::startCleanupTimer())
			log_msg("Shared stash cleanup timer could not be started (error %u).\n", GetLastError());
	if (active_Windowed)
		SetWindowedOptions();
	if (active_PrintPlugYVersion)
	{
		char name[] = "PlugY4Kolbot";
		char version[] = PLUGY_VERSION;
		// The version line must fit the 200 px box on its own (measured on 1.14d: 13 characters do).
		static_assert(sizeof(version) - 1 <= 13, "PLUGY_VERSION is longer than 13 characters");
		void* textbox = D2CreateTextBox(newTextBoxData);
		childrens[*sgnNumChildren] = textbox;
		d2_assert((*sgnNumChildren)++ >= 40, "sgnNumChildren < MAX_CHILDREN", __FILE__, __LINE__);
		D2PrintLineOnTextBox(textbox, name, colorOfPlugYVersion);
		D2PrintLineOnTextBox(textbox, version, colorOfPlugYVersion);

		if (active_CheckForUpdates)
			pollUpdateCheck();
		// The notice is optional, so a full child list (the bound d2_assert enforces above) skips it.
		if (updateNoticeVersion[0] && *sgnNumChildren < 40)
		{
			void* noticeBox = D2CreateTextBox(updateTextBoxData);
			if (noticeBox)
			{
				childrens[(*sgnNumChildren)++] = noticeBox;
				char heading[] = "Update available";
				D2PrintLineOnTextBox(noticeBox, heading, GREEN);
				D2PrintLineOnTextBox(noticeBox, updateNoticeVersion, WHITE);
			}
		}
	}
}

void** childrens;
DWORD* sgnNumChildren;
FCT_ASM ( caller_printPlugYVersion114 )
	PUSH sgnNumChildren
	PUSH childrens
	CALL printPlugYVersion
	MOV ECX,0x115
	RETN
}}

FCT_ASM ( caller_printPlugYVersion )
	POP ESI
	PUSH DWORD PTR [ESI+2]
	PUSH DWORD PTR [ESI+9]
	CALL printPlugYVersion
	CALL D2CreateTextBox
	JMP ESI
}}


void Install_PrintPlugYVersion()
{
	static int isInstalled = false;
	if (isInstalled) return;

	log_msg("Patch D2Launch to print PlugY version. (PrintPlugYVersion)\n");

	// Print PlugY version.
	mem_seek R8(D2Launch, 7F5D, 7F7D, 9639, 117C7, 178A7, 16AF7, 18061, 10A11, 33798);
	if (version_D2Launch == V114d)
	{
		memt_byte(0xB9, 0xE8);
		MEMT_REF4(0x00000115, caller_printPlugYVersion114);
		childrens = (void**)(offset_Game + 0x379350);
		sgnNumChildren = (DWORD*)(offset_Game + 0x379944);
		//00433798  |. B9 15010000    MOV ECX,115
	}
	else
	{
		MEMJ_REF4( D2CreateTextBox, caller_printPlugYVersion);
		//6FA19638  |. E8 1BED0000    CALL <JMP.&D2Win.#10017>
		//6FA517C6  |. E8 6F81FFFF    CALL <JMP.&D2Win.#10147>
		//6FA578A6  |. E8 D71FFFFF    CALL <JMP.&D2Win.#10113>
		//6FA56B1F  |. E8 1A2EFFFF    CALL <JMP.&D2Win.#10098>
		//6FA56AF6  |. E8 432EFFFF    CALL <JMP.&D2Win.#10098>
		//6FA58060  |. E8 ED18FFFF    CALL <JMP.&D2Win.#10098>
		//6FA50A10  |. E8 218FFFFF    CALL <JMP.&D2Win.#10164>
	}

	log_msg("\n");

	isInstalled = true;
}

/*************************************************************************************/

void FASTCALL versionChange(void* screen, char* text, DWORD color)
{
	D2PrintLineOnTextBox(screen,versionText,modVersionColor);
}

FCT_ASM ( caller_VersionChange_114 )
	MOV CL, BYTE PTR DS:[modVersionColor]
	MOV BYTE PTR SS:[ESP+4], CL
	MOV EDX,versionText
	MOV ECX,ESI
	RETN
}}

FCT_ASM ( caller_VersionChange_10 )
	MOV CL, BYTE PTR DS:[modVersionColor]
	MOV BYTE PTR SS:[ESP+4], CL
	MOV EDX,versionText
	MOV ECX,EDI
	RETN
}}

void Install_VersionChange()// BUG WITH 2MOD if D2Mod started before PlugY ????
{
	static int isInstalled = false;
	if (isInstalled) return;

	log_msg("Patch D2Launch to print Mod version. (VersionChange)\n");

//6FA18012  |. 8D5424 0C      LEA EDX,DWORD PTR SS:[ESP+C]
//6FA18016  |. 8BCE           MOV ECX,ESI
//6FA18018  |. 6A 00          PUSH 0


	// Print LoD/Mod version.
	if (version_D2Launch >= V114d)
	{
		mem_seek R8(D2Launch, 00000, 00000, 9723, 1189B, 1797B, 16BCB, 18134, 10AE4, 337EA);//6FA19721-6FA10000
		memt_byte(0x8D, 0xE8);	// CALL
		MEMT_REF4(0xCE8BC055, caller_VersionChange_114);
		//004337EA  |. 8D55 C0        LEA EDX,DWORD PTR SS:[EBP-40]            ; |
		//004337ED  |. 8BCE           MOV ECX,ESI                              ; |
	}
	else if (version_D2Launch >= V110)
	{
		mem_seek R8(D2Launch, 00000, 00000, 9723, 1189B, 1797B, 16BCB, 18134, 10AE4, 337EA);//6FA19721-6FA10000
		memt_byte( 0x8D, 0xE8 );	// CALL
		MEMT_REF4( 0x8B102454 , caller_VersionChange_10);
		memt_byte( 0xCF, 0x90 );	// NOP
		//6FA19723  |. 8D5424 10      LEA EDX,DWORD PTR SS:[ESP+10]
		//6FA19727  |. 8BCF           MOV ECX,EDI
		//6FA5189B  |. 8D5424 10      LEA EDX,DWORD PTR SS:[ESP+10]
		//6FA5189F  |. 8BCF           MOV ECX,EDI
		//6FA5797B  |. 8D5424 10      LEA EDX,DWORD PTR SS:[ESP+10]
		//6FA5797F  |. 8BCF           MOV ECX,EDI
		//6FA56BCB  |. 8D5424 10      LEA EDX,DWORD PTR SS:[ESP+10]
		//6FA56BCF  |. 8BCF           MOV ECX,EDI
		//6FA58134  |. 8D5424 10      LEA EDX,DWORD PTR SS:[ESP+10]
		//6FA58138  |. 8BCF           MOV ECX,EDI
		//6FA50AE4  |. 8D5424 10      LEA EDX,DWORD PTR SS:[ESP+10]
		//6FA50AE8  |. 8BCF           MOV ECX,EDI
	} else {
		mem_seek R8(D2Launch, 801B, 803B, 972A, 118A2, 17982, 16BD2, 1813B, 10AEB, 00000);
		MEMJ_REF4( D2PrintLineOnTextBox, versionChange);
		//6FA19729  |. E8 88EB0000    CALL <JMP.&D2Win.#10046>
		//6FA518A1  |. E8 267FFFFF    CALL <JMP.&D2Win.#10061>
		//6FA57981  |. E8 781EFFFF    CALL <JMP.&D2Win.#10075>
		//6FA56BD1  |. E8 EA2CFFFF    CALL <JMP.&D2Win.#10015>
		//6FA5813A  |. E8 8F17FFFF    CALL <JMP.&D2Win.#10022>
		//6FA50AEA  |. E8 DF8CFFFF    CALL <JMP.&D2Win.#10051>
	}
	log_msg("\n");

	isInstalled = true;
}


/*================================= END OF FILE =================================*/
