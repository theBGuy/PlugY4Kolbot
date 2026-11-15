/*=================================================================
	Live shared stash, the pure half: classic .sss builder and
	header parse, marker, folder state table, lock names. See
	SharedStashFormat.h.
=================================================================*/

#include "SharedStashFormat.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>

#define SSS_TAG 0x00535353
#define SSS_VERSION_01 0x3130
#define SSS_VERSION_02 0x3230
#define SSS_STASH_TAG 0x5453
#define SSS_JM_TAG 0x4D4A
#define SSS_MAX_NAME 20
#define SSS_MAX_ROOTS 65535
// FILE_MIN_SIZE of src/plugy/SharedSaveFile.cpp: tag, version "01" and the page count.
#define SSS_MIN_SIZE 10

#define MARKER_VERSION 0x564C
#define MARKER_FORMAT 1
#define MARKER_HEADER_SIZE 26
#define MARKER_MAX_DB_NAME 63
#define MARKER_DB_SUFFIX ".live.sqlite3"

namespace SharedStash
{

static DWORD getWord(const BYTE* p)
{
	return p[0] | (p[1] << 8);
}

static DWORD getDword(const BYTE* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24);
}

static void putWord(Bytes* out, DWORD v)
{
	out->push_back((unsigned char)v);
	out->push_back((unsigned char)(v >> 8));
}

static void putDword(Bytes* out, DWORD v)
{
	out->push_back((unsigned char)v);
	out->push_back((unsigned char)(v >> 8));
	out->push_back((unsigned char)(v >> 16));
	out->push_back((unsigned char)(v >> 24));
}

DWORD Fnv1a(const BYTE* data, DWORD size, DWORD hash)
{
	for (DWORD i = 0; i < size; i++)
	{
		hash ^= data[i];
		hash *= 0x01000193;
	}
	return hash;
}

void SharedStashMutexName(const char* sssPath, char* out)
{
	char fullPath[MAX_PATH];
	DWORD len = GetFullPathNameA(sssPath, MAX_PATH, fullPath, NULL);
	if (!len || len >= MAX_PATH)
	{
		strncpy(fullPath, sssPath, MAX_PATH - 1);
		fullPath[MAX_PATH - 1] = 0;
	}
	// 8.3 and long spellings of the same save folder must name the same mutex
	char longPath[MAX_PATH];
	len = GetLongPathNameA(fullPath, longPath, MAX_PATH);
	if (len && len < MAX_PATH)
		strcpy(fullPath, longPath);
	_strlwr(fullPath);
	sprintf(out, "Local\\PlugY_SharedStash_%08X", Fnv1a((const BYTE*)fullPath, (DWORD)strlen(fullPath)));
}

void LiveStashLockName(const char* canonicalLowerDbPath, char* out)
{
	sprintf(out, "Global\\PlugY_LiveStash_%08X",
		Fnv1a((const BYTE*)canonicalLowerDbPath, (DWORD)strlen(canonicalLowerDbPath)));
}

// --- Classic .sss builder -------------------------------------------------------------------------------------------

static bool isPending(const ItemRow& item)
{
	return item.pendingOp != 0 || item.pendingKind != PENDING_NONE;
}

// Whether a pending row is part of the output in mode EXPORT; false in *emit with a false return means refused.
static bool resolvePending(const ItemRow& item, const PendingResolution& resolution, bool* emit)
{
	if (item.pendingKind != PENDING_DEPOSITED && item.pendingKind != PENDING_WITHDRAWN)
		return false;
	for (size_t i = 0; i < resolution.size(); i++)
	{
		if (resolution[i].opId != item.pendingOp)
			continue;
		if (resolution[i].view == VIEW_SETTLED)
			*emit = item.pendingKind == PENDING_DEPOSITED;
		else
			*emit = item.pendingKind == PENDING_WITHDRAWN;
		return true;
	}
	return false;
}

static bool sameIdTwice(std::vector<Id> ids)
{
	std::sort(ids.begin(), ids.end());
	return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
}

static bool lowerId(const ItemRow* a, const ItemRow* b)
{
	return a->id < b->id;
}

static bool buildClassic(const Meta& meta, const std::vector<PageRow>& pages, const std::vector<ItemRow>& items,
	BuildMode mode, const PendingResolution* resolution, Bytes* out, std::vector<Id>* emittedIds)
{
	size_t pageCount = pages.size();
	std::vector<int> byOrdinal(pageCount, -1);
	std::map<Id, size_t> pageOfId;
	std::vector<Id> pageIds;
	for (size_t i = 0; i < pageCount; i++)
	{
		const PageRow& page = pages[i];
		if (page.ordinal < 0 || (size_t)page.ordinal >= pageCount || byOrdinal[page.ordinal] != -1)
			return false;
		byOrdinal[page.ordinal] = (int)i;
		if (page.name.size() > SSS_MAX_NAME || std::find(page.name.begin(), page.name.end(), 0) != page.name.end())
			return false;
		pageOfId[page.id] = i;
		pageIds.push_back(page.id);
	}
	if (sameIdTwice(pageIds))
		return false;

	if (mode == BUILD_EXPORT && resolution)
	{
		for (size_t i = 0; i < resolution->size(); i++)
			for (size_t j = i + 1; j < resolution->size(); j++)
				if ((*resolution)[i].opId == (*resolution)[j].opId && (*resolution)[i].view != (*resolution)[j].view)
					return false;
	}

	std::vector<std::vector<const ItemRow*> > emitted(pageCount);
	std::vector<Id> itemIds;
	for (size_t i = 0; i < items.size(); i++)
	{
		const ItemRow& item = items[i];
		itemIds.push_back(item.id);
		std::map<Id, size_t>::const_iterator page = pageOfId.find(item.page);
		if (page == pageOfId.end() || item.data.empty())
			return false;
		bool emit = true;
		if (isPending(item))
		{
			if (mode == BUILD_CONVERT)
				return false;
			if (mode == BUILD_LIVE_LOAD)
				emit = false;
			else if (!resolution || !resolvePending(item, *resolution, &emit))
				return false;
		}
		if (emit)
			emitted[page->second].push_back(&item);
	}
	if (sameIdTwice(itemIds))
		return false;

	Bytes result;
	std::vector<Id> ids;
	putDword(&result, SSS_TAG);
	if (meta.gold)
	{
		putWord(&result, SSS_VERSION_02);
		putDword(&result, meta.gold);
	}
	else
		putWord(&result, SSS_VERSION_01);
	putDword(&result, (DWORD)pageCount);
	for (size_t ordinal = 0; ordinal < pageCount; ordinal++)
	{
		const PageRow& page = pages[byOrdinal[ordinal]];
		std::vector<const ItemRow*>& list = emitted[byOrdinal[ordinal]];
		if (list.size() > SSS_MAX_ROOTS)
			return false;
		std::sort(list.begin(), list.end(), lowerId);
		// Four flag bytes always, as saveStash writes them. Master's loader reads a page flag-less when "JM" follows
		// the first 0x00 at or after the flags (InfinityStash.cpp loadStash); that pre-existing quirk is kept here.
		putWord(&result, SSS_STASH_TAG);
		putDword(&result, page.flags);
		result.insert(result.end(), page.name.begin(), page.name.end());
		result.push_back(0);
		putWord(&result, SSS_JM_TAG);
		putWord(&result, (DWORD)list.size());
		for (size_t i = 0; i < list.size(); i++)
		{
			result.insert(result.end(), list[i]->data.begin(), list[i]->data.end());
			ids.push_back(list[i]->id);
		}
	}
	out->swap(result);
	if (emittedIds)
		emittedIds->swap(ids);
	return true;
}

// The game calls this: an allocation failure is a refusal, never an exception.
bool BuildClassicStash(const Meta& meta, const std::vector<PageRow>& pages, const std::vector<ItemRow>& items,
	BuildMode mode, const PendingResolution* resolution, Bytes* out, std::vector<Id>* emittedIds)
{
	out->clear();
	if (emittedIds)
		emittedIds->clear();
	try
	{
		if (buildClassic(meta, pages, items, mode, resolution, out, emittedIds))
			return true;
	}
	catch (...)
	{
	}
	out->clear();
	if (emittedIds)
		emittedIds->clear();
	return false;
}

bool ParseClassicHeader(const BYTE* data, DWORD size, ClassicHeader* out)
{
	if (!data || size < 6 || getDword(data) != SSS_TAG)
		return false;
	DWORD version = getWord(data + 4);
	if (version == SSS_VERSION_01)
	{
		if (size < 10)
			return false;
		out->version = (WORD)version;
		out->gold = 0;
		out->pageCount = getDword(data + 6);
		out->pagesOffset = 10;
		return true;
	}
	if (version == SSS_VERSION_02)
	{
		if (size < 14)
			return false;
		out->version = (WORD)version;
		out->gold = getDword(data + 6);
		out->pageCount = getDword(data + 10);
		out->pagesOffset = 14;
		return true;
	}
	return false;
}

// --- Marker ---------------------------------------------------------------------------------------------------------

// ASCII text with no path separator, ending in the live database suffix, so the marker's text stays one plain line.
static bool validDbFileName(const char* name)
{
	if (!name)
		return false;
	size_t len = strlen(name);
	size_t suffixLen = strlen(MARKER_DB_SUFFIX);
	if (!len || len > MARKER_MAX_DB_NAME || len < suffixLen)
		return false;
	for (size_t i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)name[i];
		if (c < 0x20 || c > 0x7E || c == '\\' || c == '/' || c == ':')
			return false;
	}
	return memcmp(name + len - suffixLen, MARKER_DB_SUFFIX, suffixLen) == 0;
}

// The marker's text for a name validDbFileName accepted; returns its length. text holds MARKER_TEXT_SIZE bytes.
#define MARKER_TEXT_SIZE 256
static DWORD markerText(const char* dbFileName, char* text)
{
	strcpy(text, "This shared stash is stored in ");
	strcat(text, dbFileName);
	strcat(text, " (PlugY4Kolbot live shared stash).\r\n");
	strcat(text, "To get a classic shared stash back, run PlugYLiveStash.exe disable (PlugY4Kolbot releases page).\r\n");
	return (DWORD)strlen(text);
}

bool BuildMarker(const unsigned char dbGuid[16], const char* dbFileName, Bytes* out)
{
	out->clear();
	if (!dbGuid || !validDbFileName(dbFileName))
		return false;
	char text[MARKER_TEXT_SIZE];
	DWORD textLen = markerText(dbFileName, text);
	try
	{
		Bytes result;
		putDword(&result, SSS_TAG);
		putWord(&result, MARKER_VERSION);
		putDword(&result, MARKER_FORMAT);
		result.insert(result.end(), dbGuid, dbGuid + 16);
		result.insert(result.end(), text, text + textLen);
		out->swap(result);
		return true;
	}
	catch (...)
	{
		out->clear();
		return false;
	}
}

MarkerKind ParseMarker(const BYTE* data, DWORD size, const char* dbFileName, unsigned char guidOut[16])
{
	if (guidOut)
		memset(guidOut, 0, 16);
	if (!data || size < 6 || getDword(data) != SSS_TAG || getWord(data + 4) != MARKER_VERSION)
		return MARKER_NONE;
	if (size < MARKER_HEADER_SIZE || getDword(data + 6) != MARKER_FORMAT || !validDbFileName(dbFileName))
		return MARKER_DAMAGED;
	char text[MARKER_TEXT_SIZE];
	DWORD textLen = markerText(dbFileName, text);
	if (size != MARKER_HEADER_SIZE + textLen || memcmp(data + MARKER_HEADER_SIZE, text, textLen))
		return MARKER_DAMAGED;
	if (guidOut)
		memcpy(guidOut, data + 10, 16);
	return MARKER_VALID;
}

SssKind ClassifySssBytes(const BYTE* data, DWORD size, const char* dbFileName, unsigned char guidOut[16])
{
	if (guidOut)
		memset(guidOut, 0, 16);
	if (!size)
		return SSS_ABSENT;
	MarkerKind marker = ParseMarker(data, size, dbFileName, guidOut);
	if (marker == MARKER_VALID)
		return SSS_MARKER;
	if (marker == MARKER_DAMAGED)
		return SSS_MARKER_DAMAGED;
	if (data && size >= 6 && getDword(data) == SSS_TAG)
	{
		DWORD version = getWord(data + 4);
		// Master's reader refuses a non-empty .sss shorter than its minimum as truncated: every folder state must
		// refuse it, never keep or import it as a classic stash.
		if ((version == SSS_VERSION_01 || version == SSS_VERSION_02) && size < SSS_MIN_SIZE)
			return SSS_MARKER_DAMAGED;
		if (version == SSS_VERSION_01 || version == SSS_VERSION_02)
			return SSS_CLASSIC;
	}
	return SSS_FOREIGN;
}

// --- Folder state table ---------------------------------------------------------------------------------------------

static FolderDecision decision(FolderVerdict verdict, int row, FolderAction action)
{
	FolderDecision d = {verdict, row, action};
	return d;
}

FolderDecision LiveFolderDecide(const FolderInputs& in)
{
	if (in.sss == SSS_FOREIGN)
		return decision(FOLDER_CLASSIC, 10, ACTION_NONE);
	if (in.sss == SSS_UNREADABLE)
		return decision(FOLDER_CLASSIC, 11, ACTION_NONE);
	if (in.sss == SSS_MARKER_DAMAGED)
		return decision(FOLDER_REFUSE, 8, ACTION_NONE);
	if (in.sss == SSS_MARKER)
	{
		switch (in.db)
		{
		case DB_ABSENT:
		case DB_EMPTY:
		case DB_FOREIGN:
		case DB_UNREADABLE:
		case DB_BACKUP_COPY:
			return decision(FOLDER_REFUSE, 8, ACTION_NONE);
		case DB_NEWER:
			return decision(FOLDER_REFUSE, 14, ACTION_NONE);
		default:
			break;
		}
		if (!in.guidMatches)
			return decision(FOLDER_REFUSE, 8, ACTION_NONE);
		if (!in.folderUsable)
			return decision(FOLDER_REFUSE, 9, ACTION_NONE);
		if (in.db == DB_LIVE)
			return decision(FOLDER_LIVE, 2, ACTION_NONE);
		if (in.db == DB_IMPORTING)
			return decision(FOLDER_LIVE, 4, ACTION_SET_MODE_LIVE);
		return decision(FOLDER_REFUSE, 7, ACTION_NONE);
	}
	// CLASSIC or ABSENT. A request answers IMPORT whatever folderUsable says: the import owns every skip reason
	// and reports it.
	switch (in.db)
	{
	case DB_ABSENT:
		if (in.orphanWalShm)
			return decision(FOLDER_CLASSIC, 15, ACTION_RENAME_ORPHANS);
		if (in.requestExists)
			return decision(FOLDER_IMPORT, 1, ACTION_NONE);
		return decision(FOLDER_CLASSIC, 1, ACTION_NONE);
	case DB_IMPORTING:
	case DB_EMPTY:
		return decision(FOLDER_CLASSIC, 3, ACTION_RETIRE_FAILED);
	case DB_LIVE:
		if (in.sss == SSS_CLASSIC)
			return decision(FOLDER_REFUSE, 5, ACTION_NONE);
		return decision(FOLDER_REFUSE, 12, ACTION_NONE);
	case DB_CONVERTING_BACK:
		if (in.sss == SSS_CLASSIC)
			return decision(FOLDER_CLASSIC, 6, ACTION_RETIRE_RETIRED);
		return decision(FOLDER_REFUSE, 13, ACTION_NONE);
	default:
		return decision(FOLDER_REFUSE, 0, ACTION_NONE);
	}
}

}
