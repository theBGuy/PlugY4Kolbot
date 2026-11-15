/*=================================================================
	Unit tests for src/common/SharedStashFormat: the classic .sss
	builder against hand-written bytes and the synthetic fixture,
	the marker, the folder state table, FNV-1a and the lock names.
	Built and run by scripts/build.ps1 -Test; exits 1 on any failure.

	SharedStashFormatTests.exe --fixtures DIR
=================================================================*/

#include "../src/common/SharedStashFormat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace SharedStash;

static int nbPassed = 0;
static int nbFailed = 0;

static void check(bool ok, const char* text, int line)
{
	if (ok)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d: %s\n", line, text);
}

static void checkInt(int actual, int expected, const char* text, int line)
{
	if (actual == expected)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d: %s is %d, expected %d\n", line, text, actual, expected);
}

#define CHECK(cond) check((cond) ? true : false, #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkInt((int)(actual), (int)(expected), #actual, __LINE__)

// A loop's checks name the item, since one source line runs for every item.
static void checkItem(bool ok, int item, const char* text, int line)
{
	if (ok)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d [%d]: %s\n", line, item, text);
}

#define CHECK_ITEM(cond) checkItem((cond) ? true : false, item, #cond, __LINE__)

static char fixturesDir[MAX_PATH] = "";

// "53 53 00" -> bytes.
static Bytes hex(const char* text)
{
	Bytes out;
	while (*text)
	{
		if (*text == ' ')
		{
			text++;
			continue;
		}
		out.push_back((unsigned char)strtoul(std::string(text, 2).c_str(), NULL, 16));
		text += 2;
	}
	return out;
}

static Bytes text(const char* s)
{
	return Bytes(s, s + strlen(s));
}

static Meta metaOf(DWORD gold)
{
	Meta meta;
	memset(&meta, 0, sizeof(meta));
	meta.mode = MODE_LIVE;
	meta.gridW = 10;
	meta.gridH = 10;
	meta.gold = gold;
	return meta;
}

static PageRow pageOf(Id id, int ordinal, DWORD flags, const Bytes& name)
{
	PageRow page;
	page.id = id;
	page.ordinal = ordinal;
	page.flags = flags;
	page.name = name;
	page.version = 1;
	return page;
}

static ItemRow itemOf(Id id, Id page, const Bytes& data, Id pendingOp = 0, int pendingKind = PENDING_NONE)
{
	ItemRow item;
	item.id = id;
	item.page = page;
	item.x = 0;
	item.y = 0;
	item.w = 1;
	item.h = 1;
	item.data = data;
	item.version = 1;
	item.pendingOp = pendingOp;
	item.pendingKind = pendingKind;
	return item;
}

// A refused build leaves both outputs empty, whatever they held before.
static bool refused(const Meta& meta, const std::vector<PageRow>& pages, const std::vector<ItemRow>& items,
	BuildMode mode = BUILD_CONVERT, const PendingResolution* resolution = NULL)
{
	Bytes out(3, 0x55);
	std::vector<Id> ids(2, 7);
	bool built = BuildClassicStash(meta, pages, items, mode, resolution, &out, &ids);
	return !built && out.empty() && ids.empty();
}

static bool readFile(const std::string& path, Bytes* out)
{
	out->clear();
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	unsigned char buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
		out->insert(out->end(), buffer, buffer + n);
	fclose(f);
	return true;
}


/*============================= T-F1 ==========================*/

static void testBuilderBytes()
{
	printf("T-F1 builder bytes\n");
	std::vector<PageRow> pages;
	std::vector<ItemRow> items;
	Bytes out;
	std::vector<Id> ids;

	// The worked example: the smallest valid file.
	pages.push_back(pageOf(1, 0, 0, Bytes()));
	CHECK(BuildClassicStash(metaOf(0), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	CHECK(out == hex("53 53 53 00 30 31 01 00 00 00 53 54 00 00 00 00 00 4A 4D 00 00"));
	CHECK_EQ(out.size(), 21);
	CHECK(ids.empty());

	CHECK(BuildClassicStash(metaOf(4294967295UL), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	CHECK(out == hex("53 53 53 00 30 32 FF FF FF FF 01 00 00 00 53 54 00 00 00 00 00 4A 4D 00 00"));

	// A 20-byte name, written whole, then its NUL.
	pages[0] = pageOf(1, 0, 0x00000105, text("ABCDEFGHIJKLMNOPQRST"));
	CHECK(BuildClassicStash(metaOf(0), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	CHECK(out == hex("53 53 53 00 30 31 01 00 00 00 53 54 05 01 00 00 "
		"41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50 51 52 53 54 00 4A 4D 00 00"));

	// Pages and items given out of order come out by ordinal, then by ascending id.
	pages.clear();
	pages.push_back(pageOf(3, 1, 0, text("B")));
	pages.push_back(pageOf(7, 0, 2, Bytes()));
	items.push_back(itemOf(9, 3, hex("4A 4D 09")));
	items.push_back(itemOf(5, 7, hex("4A 4D 05")));
	items.push_back(itemOf(1, 3, hex("4A 4D 01 01")));
	items.push_back(itemOf(2, 7, hex("4A 4D 02")));
	CHECK(BuildClassicStash(metaOf(0x01020304), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	CHECK(out == hex("53 53 53 00 30 32 04 03 02 01 02 00 00 00 "
		"53 54 02 00 00 00 00 4A 4D 02 00 4A 4D 02 4A 4D 05 "
		"53 54 00 00 00 00 42 00 4A 4D 02 00 4A 4D 01 01 4A 4D 09"));
	CHECK_EQ(ids.size(), 4);
	CHECK(ids.size() == 4 && ids[0] == 2 && ids[1] == 5 && ids[2] == 1 && ids[3] == 9);

	// One page at the largest root count.
	pages.clear();
	items.clear();
	pages.push_back(pageOf(1, 0, 0, Bytes()));
	for (Id id = 1; id <= 65535; id++)
		items.push_back(itemOf(id, 1, hex("4A")));
	CHECK(BuildClassicStash(metaOf(0), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	CHECK_EQ(out.size(), 21 + 65535);
	CHECK(out.size() > 20 && out[19] == 0xFF && out[20] == 0xFF);
	CHECK_EQ(ids.size(), 65535);
	items.push_back(itemOf(65536, 1, hex("4A")));
	CHECK(refused(metaOf(0), pages, items));

	// Controls, each refused with both outputs empty.
	items.clear();
	pages[0].name = text("ABCDEFGHIJKLMNOPQRSTU");
	CHECK(refused(metaOf(0), pages, items));
	pages[0].name = hex("41 00 42");
	CHECK(refused(metaOf(0), pages, items));
	pages[0].name.clear();
	pages.push_back(pageOf(2, 2, 0, Bytes()));
	CHECK(refused(metaOf(0), pages, items));
	pages[1].ordinal = 0;
	CHECK(refused(metaOf(0), pages, items));
	pages[1].ordinal = 1;
	CHECK(BuildClassicStash(metaOf(0), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	pages[1].id = 1;
	CHECK(refused(metaOf(0), pages, items));
	pages[1].id = 2;
	items.push_back(itemOf(4, 1, hex("4A 4D")));
	items.push_back(itemOf(4, 2, hex("4A 4D")));
	CHECK(refused(metaOf(0), pages, items));
	items[1].id = 5;
	CHECK(BuildClassicStash(metaOf(0), pages, items, BUILD_CONVERT, NULL, &out, &ids));
	items[1].data.clear();
	CHECK(refused(metaOf(0), pages, items));
	items[1].data = hex("4A 4D");
	items[1].page = 3;
	CHECK(refused(metaOf(0), pages, items));
}


/*============================= T-F2 ==========================*/

struct SplitPage
{
	DWORD start;      // offset of "ST"
	DWORD itemsStart; // offset of the first blob
	DWORD end;        // offset after the last blob
	DWORD flags;
	Bytes name;
	DWORD rootCount;
	std::vector<Bytes> blobs;
};

struct Split
{
	ClassicHeader header;
	std::vector<SplitPage> pages;
};

static DWORD word(const Bytes& b, DWORD at)
{
	return b[at] | (b[at + 1] << 8);
}

static DWORD dword(const Bytes& b, DWORD at)
{
	return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | ((DWORD)b[at + 3] << 24);
}

// Walks the file as the builder writes it, taking each blob by its recorded size, never by scanning for tags.
// False is the named result "split failed".
static bool splitFile(const Bytes& file, const std::vector<std::vector<DWORD> >& sizes, Split* out)
{
	out->pages.clear();
	if (file.empty() || !ParseClassicHeader(&file[0], (DWORD)file.size(), &out->header))
		return false;
	DWORD size = (DWORD)file.size();
	DWORD pos = out->header.pagesOffset;
	if (sizes.size() != out->header.pageCount)
		return false;
	for (DWORD p = 0; p < out->header.pageCount; p++)
	{
		SplitPage page;
		page.start = pos;
		if (size - pos < 6 || word(file, pos) != 0x5453)
			return false;
		page.flags = dword(file, pos + 2);
		pos += 6;
		const unsigned char* nul = (const unsigned char*)memchr(&file[pos], 0, size - pos);
		if (!nul)
			return false;
		DWORD nameLen = (DWORD)(nul - &file[pos]);
		page.name.assign(file.begin() + pos, file.begin() + pos + nameLen);
		pos += nameLen + 1;
		if (size - pos < 4 || word(file, pos) != 0x4D4A)
			return false;
		page.rootCount = word(file, pos + 2);
		pos += 4;
		page.itemsStart = pos;
		if (sizes[p].size() != page.rootCount)
			return false;
		for (size_t i = 0; i < sizes[p].size(); i++)
		{
			if (sizes[p][i] > size - pos)
				return false;
			page.blobs.push_back(Bytes(file.begin() + pos, file.begin() + pos + sizes[p][i]));
			pos += sizes[p][i];
		}
		page.end = pos;
		bool last = p + 1 == out->header.pageCount;
		if (last ? pos != size : (size - pos < 2 || word(file, pos) != 0x5453))
			return false;
		out->pages.push_back(page);
	}
	return true;
}

static bool readSizes(const std::string& path, std::vector<std::vector<DWORD> >* out)
{
	Bytes raw;
	if (!readFile(path, &raw))
		return false;
	std::string all(raw.begin(), raw.end());
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= all.size())
	{
		size_t nl = all.find('\n', start);
		if (nl == std::string::npos)
		{
			if (start < all.size())
				lines.push_back(all.substr(start));
			break;
		}
		lines.push_back(all.substr(start, nl - start));
		start = nl + 1;
	}
	out->clear();
	for (size_t i = 0; i < lines.size(); i++)
	{
		std::string line = lines[i];
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		std::vector<DWORD> sizes;
		const char* p = line.c_str();
		while (*p)
		{
			char* end;
			unsigned long v = strtoul(p, &end, 10);
			if (end == p)
				return false;
			sizes.push_back((DWORD)v);
			p = end;
			while (*p == ' ')
				p++;
		}
		out->push_back(sizes);
	}
	return true;
}

static bool blobsStartJm(const Split& split)
{
	for (size_t p = 0; p < split.pages.size(); p++)
		for (size_t i = 0; i < split.pages[p].blobs.size(); i++)
		{
			const Bytes& b = split.pages[p].blobs[i];
			if (b.size() < 2 || b[0] != 0x4A || b[1] != 0x4D)
				return false;
		}
	return true;
}

// Rows from the split: page ids 1..n by file order, item ids ascending in file order; on page `reversed` (or none
// for -1) the items take ids in reverse file order instead.
static void rowsOf(const Split& split, int reversed, std::vector<PageRow>* pages, std::vector<ItemRow>* items)
{
	pages->clear();
	items->clear();
	Id nextId = 1;
	for (size_t p = 0; p < split.pages.size(); p++)
	{
		const SplitPage& page = split.pages[p];
		pages->push_back(pageOf((Id)p + 1, (int)p, page.flags, page.name));
		for (size_t i = 0; i < page.blobs.size(); i++)
		{
			Id id = (int)p == reversed ? 1000 + (Id)(page.blobs.size() - i) : nextId++;
			items->push_back(itemOf(id, (Id)p + 1, page.blobs[i]));
		}
	}
}

// The file with page `p`'s blobs in reverse order, assembled from the file's own bytes and the split's blobs.
static Bytes reversedPage(const Bytes& file, const Split& split, size_t p)
{
	const SplitPage& page = split.pages[p];
	Bytes out(file.begin(), file.begin() + page.itemsStart);
	for (size_t i = page.blobs.size(); i > 0; i--)
		out.insert(out.end(), page.blobs[i - 1].begin(), page.blobs[i - 1].end());
	out.insert(out.end(), file.begin() + page.end, file.end());
	return out;
}

// The flags are read only when the two bytes after the first 0x00 at or after the flags offset are not "JM".
static bool loaderReadsFlags(const Bytes& file, const SplitPage& page)
{
	DWORD flagsAt = page.start + 2;
	bool zeroInFlags = false;
	for (DWORD i = 0; i < 4; i++)
		if (file[flagsAt + i] == 0)
			zeroInFlags = true;
	const unsigned char* nul = (const unsigned char*)memchr(&file[flagsAt], 0, file.size() - flagsAt);
	if (!zeroInFlags || !nul)
		return false;
	DWORD after = (DWORD)(nul - &file[0]) + 1;
	return after + 2 <= file.size() && !(file[after] == 0x4A && file[after + 1] == 0x4D);
}

static void testFixtureRoundTrip()
{
	printf("T-F2 fixture round trip\n");
	std::string dir = std::string(fixturesDir) + "\\live-stash\\";
	Bytes file;
	std::vector<std::vector<DWORD> > sizes;
	CHECK(readFile(dir + "synthetic.sss", &file));
	CHECK(readSizes(dir + "synthetic.sizes.txt", &sizes));
	Split split;
	bool splitOk = splitFile(file, sizes, &split);
	CHECK(splitOk);
	if (!splitOk)
	{
		printf("  split failed\n");
		return;
	}
	CHECK_EQ(split.pages.size(), 3);
	CHECK(split.header.version == 0x3230 && split.header.gold != 0);
	for (size_t p = 0; p < split.pages.size(); p++)
	{
		int item = (int)p;
		CHECK_ITEM(sizes[p].size() == split.pages[p].rootCount);
		CHECK_ITEM(loaderReadsFlags(file, split.pages[p]));
	}
	CHECK(blobsStartJm(split));

	Meta meta = metaOf(split.header.gold);
	std::vector<PageRow> pages;
	std::vector<ItemRow> items;
	Bytes out;
	rowsOf(split, -1, &pages, &items);
	CHECK(BuildClassicStash(meta, pages, items, BUILD_CONVERT, NULL, &out, NULL));
	CHECK(out == file);

	// The page with the most items, its ids reversed.
	size_t busiest = 0;
	for (size_t p = 1; p < split.pages.size(); p++)
		if (split.pages[p].blobs.size() > split.pages[busiest].blobs.size())
			busiest = p;
	CHECK(split.pages[busiest].blobs.size() >= 2);
	rowsOf(split, (int)busiest, &pages, &items);
	CHECK(BuildClassicStash(meta, pages, items, BUILD_CONVERT, NULL, &out, NULL));
	CHECK(out == reversedPage(file, split, busiest));
	CHECK(out != file);

	// Controls on a copy of the sizes. A compensated pair still splits but cuts a blob that does not start "JM";
	// an uncompensated +1 cannot split. Only the "JM" start check catches the pair: the permuted build cuts its rows
	// and its expected bytes from the same sizes.
	std::vector<std::vector<DWORD> > shifted = sizes;
	size_t pairPage = busiest;
	shifted[pairPage][0] += 1;
	shifted[pairPage][1] -= 1;
	Split wrong;
	CHECK(splitFile(file, shifted, &wrong));
	CHECK(!blobsStartJm(wrong));
	shifted = sizes;
	shifted[pairPage][1] += 1;
	bool uncompensated = splitFile(file, shifted, &wrong);
	CHECK(!uncompensated);
	if (uncompensated)
		printf("  uncompensated +1 still split\n");
}


/*============================= T-F3 ==========================*/

static const unsigned char Guid[16] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};
static const char* DbName = "_LOD_SharedStashSave.live.sqlite3";

static bool zeroGuid(const unsigned char* guid)
{
	for (int i = 0; i < 16; i++)
		if (guid[i])
			return false;
	return true;
}

static bool markerRefused(const char* name)
{
	Bytes out(4, 0x55);
	return !BuildMarker(Guid, name, &out) && out.empty();
}

static void testMarker()
{
	printf("T-F3 marker\n");
	Bytes marker;
	unsigned char guid[16];
	CHECK(BuildMarker(Guid, DbName, &marker));
	CHECK_EQ(marker.size(), 224);
	CHECK(ParseMarker(&marker[0], (DWORD)marker.size(), DbName, guid) == MARKER_VALID);
	CHECK(!memcmp(guid, Guid, 16));
	// The exact bytes players see, written out by hand rather than through the builder's own text function.
	static const char* expectedText =
		"This shared stash is stored in _LOD_SharedStashSave.live.sqlite3 (PlugY4Kolbot live shared stash).\r\n"
		"To get a classic shared stash back, run PlugYLiveStash.exe disable (PlugY4Kolbot releases page).\r\n";
	Bytes expected = hex("53 53 53 00 4C 56 01 00 00 00");
	expected.insert(expected.end(), Guid, Guid + 16);
	expected.insert(expected.end(), expectedText, expectedText + strlen(expectedText));
	CHECK_EQ(strlen(expectedText), 198);
	CHECK(marker == expected);

	for (int item = 0; item < (int)marker.size(); item++)
	{
		Bytes changed = marker;
		changed[item] ^= 0x01;
		MarkerKind kind = ParseMarker(&changed[0], (DWORD)changed.size(), DbName, guid);
		if (item < 6)
			CHECK_ITEM(kind == MARKER_NONE);
		else if (item < 10)
			CHECK_ITEM(kind == MARKER_DAMAGED);
		else if (item < 26)
			CHECK_ITEM(kind == MARKER_VALID && memcmp(guid, Guid, 16) != 0);
		else
			CHECK_ITEM(kind == MARKER_DAMAGED);
	}
	Bytes cut(marker.begin(), marker.end() - 1);
	memset(guid, 0xCC, 16);
	CHECK(ParseMarker(&cut[0], (DWORD)cut.size(), DbName, guid) == MARKER_DAMAGED);
	CHECK(zeroGuid(guid));
	Bytes longer = marker;
	longer.push_back(0x0A);
	CHECK(ParseMarker(&longer[0], (DWORD)longer.size(), DbName, guid) == MARKER_DAMAGED);
	CHECK(ParseMarker(&marker[0], (DWORD)marker.size(), "_LOD_Other.live.sqlite3", guid) == MARKER_DAMAGED);
	CHECK(ParseMarker(&marker[0], 6, DbName, guid) == MARKER_DAMAGED);
	CHECK(ClassifySssBytes(&marker[0], 6, DbName, guid) == SSS_MARKER_DAMAGED);

	std::string name63 = std::string(50, 'n') + ".live.sqlite3";
	CHECK_EQ(name63.size(), 63);
	Bytes out;
	CHECK(BuildMarker(Guid, name63.c_str(), &out));
	CHECK_EQ(out.size(), 254);
	std::string name64 = "n" + name63;
	CHECK(markerRefused(name64.c_str()));
	CHECK(markerRefused(""));
	CHECK(markerRefused(NULL));
	CHECK(markerRefused("a\\b.live.sqlite3"));
	CHECK(markerRefused("a/b.live.sqlite3"));
	CHECK(markerRefused("a:b.live.sqlite3"));
	CHECK(markerRefused("a\rb.live.sqlite3"));
	CHECK(markerRefused("caf\xE9.live.sqlite3"));
	CHECK(markerRefused("x.sqlite3"));

	// A name BuildMarker refuses can never match: DAMAGED, not a text compare.
	CHECK(ParseMarker(&marker[0], (DWORD)marker.size(), "x.sqlite3", guid) == MARKER_DAMAGED);
	CHECK(ParseMarker(&marker[0], (DWORD)marker.size(), NULL, guid) == MARKER_DAMAGED);

	// guidOut holds the database guid only for a valid marker; every other answer zeroes it.
	Bytes classic = hex("53 53 53 00 30 31 01 00 00 00 53 54 00 00 00 00 00 4A 4D 00 00");
	Bytes other = hex("53 53 53 00 30 33 01 00 00 00");
	Bytes tiny = hex("53 53 53");
	CHECK(ClassifySssBytes(&marker[0], (DWORD)marker.size(), DbName, guid) == SSS_MARKER);
	CHECK(!memcmp(guid, Guid, 16));
	CHECK(ClassifySssBytes(&classic[0], (DWORD)classic.size(), DbName, guid) == SSS_CLASSIC);
	CHECK(zeroGuid(guid));
	memset(guid, 0xCC, 16);
	CHECK(ClassifySssBytes(&other[0], (DWORD)other.size(), DbName, guid) == SSS_FOREIGN);
	CHECK(zeroGuid(guid));
	memset(guid, 0xCC, 16);
	CHECK(ClassifySssBytes(&tiny[0], (DWORD)tiny.size(), DbName, guid) == SSS_FOREIGN);
	CHECK(zeroGuid(guid));
	memset(guid, 0xCC, 16);
	CHECK(ClassifySssBytes(NULL, 0, DbName, guid) == SSS_ABSENT);
	CHECK(zeroGuid(guid));
	memset(guid, 0xCC, 16);
	CHECK(ClassifySssBytes(&marker[0], 100, DbName, guid) == SSS_MARKER_DAMAGED);
	CHECK(zeroGuid(guid));
	memset(guid, 0xCC, 16);
	CHECK(ParseMarker(&classic[0], (DWORD)classic.size(), DbName, guid) == MARKER_NONE);
	CHECK(zeroGuid(guid));

	// Shorter than master's 10-byte minimum: master's reader refuses it as truncated, so it is never CLASSIC.
	for (int item = 6; item < 10; item++)
	{
		CHECK_ITEM(ClassifySssBytes(&classic[0], item, DbName, guid) == SSS_MARKER_DAMAGED);
		Bytes gold = hex("53 53 53 00 30 32 01 00 00 00");
		CHECK_ITEM(ClassifySssBytes(&gold[0], item, DbName, guid) == SSS_MARKER_DAMAGED);
	}
	CHECK(ClassifySssBytes(&classic[0], 10, DbName, guid) == SSS_CLASSIC);

	ClassicHeader header;
	CHECK(ParseClassicHeader(&classic[0], (DWORD)classic.size(), &header));
	CHECK(header.version == 0x3130 && header.gold == 0 && header.pageCount == 1 && header.pagesOffset == 10);
	CHECK(!ParseClassicHeader(&classic[0], 9, &header));
	CHECK(!ParseClassicHeader(&other[0], (DWORD)other.size(), &header));
	Bytes withGold = hex("53 53 53 00 30 32 04 03 02 01 02 00 00 00");
	CHECK(ParseClassicHeader(&withGold[0], (DWORD)withGold.size(), &header));
	CHECK(header.version == 0x3230 && header.gold == 0x01020304 && header.pageCount == 2 && header.pagesOffset == 14);
	CHECK(!ParseClassicHeader(&withGold[0], 13, &header));
}


/*============================= T-F4 ==========================*/

enum Condition { COND_NONE, COND_NO_GUID, COND_NOT_USABLE, COND_ORPHANS, COND_REQUEST };

// One line of the oracle: masks over SssKind and DbState, a condition, and the expected answer.
struct OracleLine
{
	unsigned sss;
	unsigned db;
	Condition cond;
	int row;
	FolderVerdict verdict;
	FolderAction action;
};

#define S(k) (1u << (k))
#define ANY 0xFFFFFFFFu
#define ALIVE (S(DB_IMPORTING) | S(DB_LIVE) | S(DB_CONVERTING_BACK))

static const OracleLine Oracle[] = {
	{S(SSS_FOREIGN), ANY, COND_NONE, 10, FOLDER_CLASSIC, ACTION_NONE},
	{S(SSS_UNREADABLE), ANY, COND_NONE, 11, FOLDER_CLASSIC, ACTION_NONE},
	{S(SSS_MARKER_DAMAGED), ANY, COND_NONE, 8, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_MARKER), S(DB_ABSENT) | S(DB_EMPTY) | S(DB_FOREIGN) | S(DB_UNREADABLE) | S(DB_BACKUP_COPY), COND_NONE, 8,
		FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_MARKER), S(DB_NEWER), COND_NONE, 14, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_MARKER), ALIVE, COND_NO_GUID, 8, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_MARKER), ALIVE, COND_NOT_USABLE, 9, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_MARKER), S(DB_LIVE), COND_NONE, 2, FOLDER_LIVE, ACTION_NONE},
	{S(SSS_MARKER), S(DB_IMPORTING), COND_NONE, 4, FOLDER_LIVE, ACTION_SET_MODE_LIVE},
	{S(SSS_MARKER), S(DB_CONVERTING_BACK), COND_NONE, 7, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_CLASSIC) | S(SSS_ABSENT), S(DB_ABSENT), COND_ORPHANS, 15, FOLDER_CLASSIC, ACTION_RENAME_ORPHANS},
	{S(SSS_CLASSIC) | S(SSS_ABSENT), S(DB_ABSENT), COND_REQUEST, 1, FOLDER_IMPORT, ACTION_NONE},
	{S(SSS_CLASSIC) | S(SSS_ABSENT), S(DB_ABSENT), COND_NONE, 1, FOLDER_CLASSIC, ACTION_NONE},
	{S(SSS_CLASSIC) | S(SSS_ABSENT), S(DB_IMPORTING) | S(DB_EMPTY), COND_NONE, 3, FOLDER_CLASSIC,
		ACTION_RETIRE_FAILED},
	{S(SSS_CLASSIC), S(DB_LIVE), COND_NONE, 5, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_ABSENT), S(DB_LIVE), COND_NONE, 12, FOLDER_REFUSE, ACTION_NONE},
	{S(SSS_CLASSIC), S(DB_CONVERTING_BACK), COND_NONE, 6, FOLDER_CLASSIC, ACTION_RETIRE_RETIRED},
	{S(SSS_ABSENT), S(DB_CONVERTING_BACK), COND_NONE, 13, FOLDER_REFUSE, ACTION_NONE},
	{ANY, ANY, COND_NONE, 0, FOLDER_REFUSE, ACTION_NONE},
};
#define ORACLE_LINES ((int)(sizeof(Oracle) / sizeof(Oracle[0])))

static bool conditionHolds(Condition cond, const FolderInputs& in)
{
	switch (cond)
	{
	case COND_NO_GUID:
		return !in.guidMatches;
	case COND_NOT_USABLE:
		return !in.folderUsable;
	case COND_ORPHANS:
		return in.orphanWalShm;
	case COND_REQUEST:
		return in.requestExists;
	default:
		return true;
	}
}

static const OracleLine* oracleOf(const OracleLine* table, int lines, const FolderInputs& in)
{
	for (int i = 0; i < lines; i++)
		if ((table[i].sss & S(in.sss)) && (table[i].db & S(in.db)) && conditionHolds(table[i].cond, in))
			return &table[i];
	return NULL;
}

static FolderInputs inputsOf(int combo)
{
	FolderInputs in;
	in.sss = (SssKind)(combo % 6);
	in.db = (DbState)((combo / 6) % 9);
	int bits = combo / 54;
	in.guidMatches = (bits & 1) != 0;
	in.orphanWalShm = (bits & 2) != 0;
	in.requestExists = (bits & 4) != 0;
	in.folderUsable = (bits & 8) != 0;
	return in;
}

// Every combination against the table: the number of mismatches; rowCounts (16 entries) and importCount when given.
static int sweep(const OracleLine* table, int lines, int* rowCounts, int* importCount, int* refuseWithAction)
{
	int mismatches = 0;
	for (int combo = 0; combo < 864; combo++)
	{
		FolderInputs in = inputsOf(combo);
		FolderDecision d = LiveFolderDecide(in);
		const OracleLine* expected = oracleOf(table, lines, in);
		if (!expected || d.row != expected->row || d.verdict != expected->verdict || d.action != expected->action)
			mismatches++;
		if (rowCounts && d.row >= 0 && d.row < 16)
			rowCounts[d.row]++;
		if (importCount && d.verdict == FOLDER_IMPORT)
			(*importCount)++;
		if (refuseWithAction && d.verdict == FOLDER_REFUSE && d.action != ACTION_NONE)
			(*refuseWithAction)++;
	}
	return mismatches;
}

static FolderDecision decide(SssKind sss, DbState db, bool guidMatches = true, bool orphans = false,
	bool request = false, bool usable = true)
{
	FolderInputs in = {sss, db, guidMatches, orphans, request, usable};
	return LiveFolderDecide(in);
}

static bool is(FolderDecision d, FolderVerdict verdict, int row, FolderAction action)
{
	return d.verdict == verdict && d.row == row && d.action == action;
}

static void testFolderDecide()
{
	printf("T-F4 LiveFolderDecide\n");
	CHECK(is(decide(SSS_CLASSIC, DB_ABSENT), FOLDER_CLASSIC, 1, ACTION_NONE));
	CHECK(is(decide(SSS_MARKER, DB_LIVE), FOLDER_LIVE, 2, ACTION_NONE));
	CHECK(is(decide(SSS_ABSENT, DB_IMPORTING), FOLDER_CLASSIC, 3, ACTION_RETIRE_FAILED));
	CHECK(is(decide(SSS_MARKER, DB_IMPORTING), FOLDER_LIVE, 4, ACTION_SET_MODE_LIVE));
	CHECK(is(decide(SSS_CLASSIC, DB_LIVE), FOLDER_REFUSE, 5, ACTION_NONE));
	CHECK(is(decide(SSS_CLASSIC, DB_CONVERTING_BACK), FOLDER_CLASSIC, 6, ACTION_RETIRE_RETIRED));
	CHECK(is(decide(SSS_MARKER, DB_CONVERTING_BACK), FOLDER_REFUSE, 7, ACTION_NONE));
	CHECK(is(decide(SSS_MARKER, DB_BACKUP_COPY), FOLDER_REFUSE, 8, ACTION_NONE));
	CHECK(is(decide(SSS_MARKER, DB_LIVE, true, false, false, false), FOLDER_REFUSE, 9, ACTION_NONE));
	CHECK(is(decide(SSS_FOREIGN, DB_LIVE), FOLDER_CLASSIC, 10, ACTION_NONE));
	CHECK(is(decide(SSS_UNREADABLE, DB_ABSENT), FOLDER_CLASSIC, 11, ACTION_NONE));
	CHECK(is(decide(SSS_ABSENT, DB_LIVE), FOLDER_REFUSE, 12, ACTION_NONE));
	CHECK(is(decide(SSS_ABSENT, DB_CONVERTING_BACK), FOLDER_REFUSE, 13, ACTION_NONE));
	CHECK(is(decide(SSS_MARKER, DB_NEWER), FOLDER_REFUSE, 14, ACTION_NONE));
	CHECK(is(decide(SSS_ABSENT, DB_ABSENT, true, true), FOLDER_CLASSIC, 15, ACTION_RENAME_ORPHANS));

	CHECK(is(decide(SSS_CLASSIC, DB_ABSENT, true, false, true, true), FOLDER_IMPORT, 1, ACTION_NONE));
	CHECK(is(decide(SSS_CLASSIC, DB_ABSENT, true, false, true, false), FOLDER_IMPORT, 1, ACTION_NONE));
	CHECK(is(decide(SSS_CLASSIC, DB_ABSENT, true, true, true, true), FOLDER_CLASSIC, 15, ACTION_RENAME_ORPHANS));
	CHECK(is(decide(SSS_MARKER, DB_LIVE, false, false, false, false), FOLDER_REFUSE, 8, ACTION_NONE));
	CHECK(is(decide(SSS_CLASSIC, DB_FOREIGN), FOLDER_REFUSE, 0, ACTION_NONE));
	CHECK(is(decide(SSS_CLASSIC, DB_EMPTY), FOLDER_CLASSIC, 3, ACTION_RETIRE_FAILED));
	CHECK(is(decide(SSS_ABSENT, DB_EMPTY), FOLDER_CLASSIC, 3, ACTION_RETIRE_FAILED));
	CHECK(is(decide(SSS_MARKER, DB_EMPTY), FOLDER_REFUSE, 8, ACTION_NONE));
	CHECK(is(decide(ClassifySssBytes(NULL, 0, DbName, NULL), DB_LIVE), FOLDER_REFUSE, 12, ACTION_NONE));
	// A classic header cut below master's minimum refuses whatever the database holds: never kept, never imported.
	Bytes truncated = hex("53 53 53 00 30 31 01 00 00");
	SssKind truncatedKind = ClassifySssBytes(&truncated[0], (DWORD)truncated.size(), DbName, NULL);
	for (int item = DB_ABSENT; item <= DB_UNREADABLE; item++)
	{
		FolderDecision d = decide(truncatedKind, (DbState)item, true, false, true, true);
		CHECK_ITEM(d.verdict == FOLDER_REFUSE && d.action == ACTION_NONE);
	}
	// A stale decision, then the fresh one after the import wrote its marker and set the mode.
	CHECK(is(decide(SSS_CLASSIC, DB_IMPORTING), FOLDER_CLASSIC, 3, ACTION_RETIRE_FAILED));
	CHECK(is(decide(SSS_MARKER, DB_LIVE, true, false, false, true), FOLDER_LIVE, 2, ACTION_NONE));

	int rowCounts[16] = {0};
	int importCount = 0;
	int refuseWithAction = 0;
	CHECK_EQ(sweep(Oracle, ORACLE_LINES, rowCounts, &importCount, &refuseWithAction), 0);
	CHECK_EQ(refuseWithAction, 0);
	CHECK_EQ(importCount, 8);
	static const int expectedCounts[16] = {128, 16, 4, 64, 4, 16, 16, 4, 248, 12, 144, 144, 16, 16, 16, 16};
	int total = 0;
	for (int item = 0; item < 16; item++)
	{
		CHECK_ITEM(rowCounts[item] == expectedCounts[item]);
		total += rowCounts[item];
	}
	CHECK_EQ(total, 864);

	// Each oracle line, renumbered in a copy, must make the sweep report a mismatch.
	int rejected = 0;
	for (int item = 0; item < ORACLE_LINES; item++)
	{
		OracleLine mutated[ORACLE_LINES];
		memcpy(mutated, Oracle, sizeof(Oracle));
		mutated[item].row = 99;
		bool caught = sweep(mutated, ORACLE_LINES, NULL, NULL, NULL) > 0;
		CHECK_ITEM(caught);
		if (caught)
			rejected++;
	}
	printf("T-F4 control: %d of %d mutated oracles rejected\n", rejected, ORACLE_LINES);
}


/*============================= T-F5 ==========================*/

// Copied verbatim from src/plugy/Common.cpp:24 (the plugy side's fnv1a, which this target cannot link).
static DWORD commonFnv1a(const BYTE* data, DWORD size, DWORD hash = 0x811C9DC5)
{
	for (DWORD i = 0; i < size; i++)
	{
		hash ^= data[i];
		hash *= 0x01000193;
	}
	return hash;
}

static void testFnv()
{
	printf("T-F5 Fnv1a\n");
	CHECK(Fnv1a(NULL, 0) == 0x811C9DC5);
	CHECK(Fnv1a((const BYTE*)"a", 1) == 0xE40C292C);
	CHECK(Fnv1a((const BYTE*)"foobar", 6) == 0xBF9CF968);
	CHECK(Fnv1a((const BYTE*)"bar", 3, Fnv1a((const BYTE*)"foo", 3)) == 0xBF9CF968);
	BYTE all[256];
	for (int i = 0; i < 256; i++)
		all[i] = (BYTE)i;
	CHECK(Fnv1a(all, sizeof(all)) == commonFnv1a(all, sizeof(all)));
	CHECK(Fnv1a(all, 100, 12345) == commonFnv1a(all, 100, 12345));
}


/*============================= T-F6 ==========================*/

static void testLockNames()
{
	printf("T-F6 lock names\n");
	char name[SHARED_STASH_MUTEX_NAME_SIZE];
	char other[SHARED_STASH_MUTEX_NAME_SIZE];
	SharedStashMutexName("C:\\Saves\\_LOD_SharedStashSave.sss", name);
	CHECK(!strcmp(name, "Local\\PlugY_SharedStash_172FB8C3"));
	SharedStashMutexName("C:\\SAVES\\_lod_sharedstashsave.SSS", other);
	CHECK(!strcmp(name, other));
	SharedStashMutexName("C:\\Saves\\_LOD_HC_SharedStashSave.sss", other);
	CHECK(strcmp(name, other) != 0);
	LiveStashLockName("c:\\saves\\_lod_sharedstashsave.live.sqlite3", name);
	CHECK(!strcmp(name, "Global\\PlugY_LiveStash_E307483D"));
}


/*============================= T-F7 ==========================*/

static void testBuilderModes()
{
	printf("T-F7 builder modes\n");
	std::vector<PageRow> pages;
	std::vector<ItemRow> items;
	pages.push_back(pageOf(1, 0, 0, Bytes()));
	items.push_back(itemOf(1, 1, hex("4A 4D 01")));
	items.push_back(itemOf(2, 1, hex("4A 4D 02"), 10, PENDING_DEPOSITED));
	items.push_back(itemOf(3, 1, hex("4A 4D 03"), 11, PENDING_WITHDRAWN));
	items.push_back(itemOf(4, 1, hex("4A 4D 04")));
	Meta meta = metaOf(0);
	Bytes out;
	std::vector<Id> ids;
	const char* head = "53 53 53 00 30 31 01 00 00 00 53 54 00 00 00 00 00 4A 4D ";

	CHECK(BuildClassicStash(meta, pages, items, BUILD_LIVE_LOAD, NULL, &out, &ids));
	CHECK(out == hex((std::string(head) + "02 00 4A 4D 01 4A 4D 04").c_str()));
	CHECK(ids.size() == 2 && ids[0] == 1 && ids[1] == 4);
	CHECK(refused(meta, pages, items, BUILD_CONVERT));

	// Ops 10 (the deposit) and 11 (the withdrawal) under one view, then under different views.
	PendingResolution resolution;
	PendingResolutionEntry deposit = {10, VIEW_SETTLED};
	PendingResolutionEntry withdrawal = {11, VIEW_SETTLED};
	resolution.push_back(deposit);
	resolution.push_back(withdrawal);
	CHECK(BuildClassicStash(meta, pages, items, BUILD_EXPORT, &resolution, &out, &ids));
	CHECK(out == hex((std::string(head) + "03 00 4A 4D 01 4A 4D 02 4A 4D 04").c_str()));
	CHECK(ids.size() == 3 && ids[0] == 1 && ids[1] == 2 && ids[2] == 4);
	resolution[0].view = VIEW_REVERT;
	resolution[1].view = VIEW_REVERT;
	CHECK(BuildClassicStash(meta, pages, items, BUILD_EXPORT, &resolution, &out, &ids));
	CHECK(out == hex((std::string(head) + "03 00 4A 4D 01 4A 4D 03 4A 4D 04").c_str()));
	CHECK(ids.size() == 3 && ids[0] == 1 && ids[1] == 3 && ids[2] == 4);
	resolution[0].view = VIEW_SETTLED;
	resolution[1].view = VIEW_REVERT;
	CHECK(BuildClassicStash(meta, pages, items, BUILD_EXPORT, &resolution, &out, &ids));
	CHECK(out == hex((std::string(head) + "04 00 4A 4D 01 4A 4D 02 4A 4D 03 4A 4D 04").c_str()));
	resolution[0].view = VIEW_REVERT;
	resolution[1].view = VIEW_SETTLED;
	CHECK(BuildClassicStash(meta, pages, items, BUILD_EXPORT, &resolution, &out, &ids));
	CHECK(out == hex((std::string(head) + "02 00 4A 4D 01 4A 4D 04").c_str()));

	// Controls: an op with no entry, no resolution at all, one op under both views.
	PendingResolution partial(1, deposit);
	CHECK(refused(meta, pages, items, BUILD_EXPORT, &partial));
	CHECK(refused(meta, pages, items, BUILD_EXPORT, NULL));
	PendingResolution both = resolution;
	PendingResolutionEntry again = {10, VIEW_SETTLED};
	both.push_back(again);
	CHECK(refused(meta, pages, items, BUILD_EXPORT, &both));
}


int main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--fixtures") && i + 1 < argc)
			strncpy(fixturesDir, argv[++i], MAX_PATH - 1);
		else
		{
			printf("usage: SharedStashFormatTests --fixtures DIR\n");
			return 2;
		}
	}
	if (!fixturesDir[0])
	{
		printf("usage: SharedStashFormatTests --fixtures DIR\n");
		return 2;
	}

	testBuilderBytes();
	testFixtureRoundTrip();
	testMarker();
	testFolderDecide();
	testFnv();
	testLockNames();
	testBuilderModes();

	printf("%d passed, %d failed\n", nbPassed, nbFailed);
	return nbFailed ? 1 : 0;
}
