/*=================================================================
	Unit tests for src/common/CharacterCommit.
	Built and run by scripts/build.ps1 -Test; exits 1 on any failure.
=================================================================*/

#include "../src/common/CharacterCommit.h"
#include <stdio.h>
#include <string.h>
#include <vector>

typedef std::vector<BYTE> Bytes;

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

// A matrix cell's checks name the cell, since one source line runs for every cell.
static void checkCell(bool ok, const char* cell, const char* text, int line)
{
	if (ok)
	{
		nbPassed++;
		return;
	}
	nbFailed++;
	printf("  FAIL line %d [%s]: %s\n", line, cell, text);
}

#define CHECK_CELL(cond) checkCell((cond) ? true : false, cell, #cond, __LINE__)

static Bytes bytesOf(const char* text)
{
	return Bytes(text, text + strlen(text));
}

// A .d2x-like file: the "CSTM" tag, a version WORD, then the body text.
static Bytes d2xFile(WORD version, const char* body)
{
	Bytes b = bytesOf("CSTM");
	b.push_back((BYTE)version);
	b.push_back((BYTE)(version >> 8));
	Bytes rest = bytesOf(body);
	b.insert(b.end(), rest.begin(), rest.end());
	return b;
}

static const BYTE* ptr(const Bytes& b)
{
	return b.empty() ? NULL : b.data();
}

static DWORD fnv(const Bytes& b)
{
	return CommitFnv1a(ptr(b), (DWORD)b.size());
}

static void putDword(Bytes& b, size_t at, DWORD v)
{
	b[at] = (BYTE)v;
	b[at + 1] = (BYTE)(v >> 8);
	b[at + 2] = (BYTE)(v >> 16);
	b[at + 3] = (BYTE)(v >> 24);
}

static void appendDword(Bytes& b, DWORD v)
{
	b.resize(b.size() + 4);
	putDword(b, b.size() - 4, v);
}

static void append(Bytes& b, const Bytes& more)
{
	b.insert(b.end(), more.begin(), more.end());
}

// Recomputes the payload hash after a test edited the bytes behind it.
static void reseal(Bytes& journal)
{
	putDword(journal, 44, CommitFnv1a(journal.data() + COMMIT_HEADER_SIZE, (DWORD)journal.size() - COMMIT_HEADER_SIZE));
}

// --- Fixtures ------------------------------------------------------------------------------------------------------

static const Bytes D2S_PRE = bytesOf("d2s before this save: level 12");
static const Bytes D2S_POST = bytesOf("d2s after this save: level 13, more gold");
static const Bytes D2S_OTHER = bytesOf("d2s of another incarnation");
static const Bytes D2X_PRE = d2xFile(COMMIT_D2X_VERSION, "pre stash: 3 pages");
static const Bytes D2X_POST = d2xFile(COMMIT_D2X_VERSION, "post stash: 4 pages and a ring");
static const Bytes D2X_OTHER = d2xFile(COMMIT_D2X_VERSION, "stash of another incarnation");
static const Bytes D2X_OTHER_FENCED = d2xFile(COMMIT_D2X_VERSION_FENCED, "fenced stash of another commit");
static const Bytes SHORT_D2X = bytesOf("abc");

static Bytes stub()
{
	return d2xFile(COMMIT_D2X_VERSION_FENCED, "");
}

static Bytes fenced(const Bytes& pre)
{
	Bytes out(CommitFencedSize(ptr(pre), (DWORD)pre.size()));
	DWORD n = CommitFence(ptr(pre), (DWORD)pre.size(), out.data(), (DWORD)out.size());
	out.resize(n);
	return out;
}

struct File
{
	bool present;
	Bytes bytes;
};

static File absent()
{
	File f = {false, Bytes()};
	return f;
}

static File present(const Bytes& b)
{
	File f = {true, b};
	return f;
}

static CommitDiskFile describe(const File& f)
{
	return f.present ? CommitFilePresent(ptr(f.bytes), (DWORD)f.bytes.size()) : CommitFileAbsent();
}

static bool sameFile(const File& a, const File& b)
{
	return a.present == b.present && a.bytes == b.bytes;
}

// The input keeps pointers to d2xPre and the post buffers, so they must outlive it (no temporaries); NULL d2xPre
// is an absent .d2x.
static CommitBuildInput buildInput(const char* name, DWORD seq, const File& d2sPre, const Bytes& d2sPost,
	const Bytes* d2xPre, const Bytes& d2xPost, DWORD commitPointPart)
{
	CommitBuildInput in = {};
	in.name = name;
	in.seq = seq;
	in.d2sPre = describe(d2sPre);
	in.d2sPost = ptr(d2sPost);
	in.d2sPostSize = (DWORD)d2sPost.size();
	in.d2xPre = d2xPre ? ptr(*d2xPre) : NULL;
	in.d2xPreSize = d2xPre ? (DWORD)d2xPre->size() : 0;
	in.d2xPost = ptr(d2xPost);
	in.d2xPostSize = (DWORD)d2xPost.size();
	in.commitPointPart = commitPointPart;
	return in;
}

static Bytes encode(const CommitBuildInput& in)
{
	Bytes out(CommitJournalSize(&in));
	DWORD n = out.empty() ? 0 : CommitJournalEncode(&in, out.data(), (DWORD)out.size());
	out.resize(n);
	return out;
}

static Bytes journalOf(BYTE state, const File& d2sPre, const File& d2xPre, DWORD commitPointPart = 0)
{
	CommitBuildInput in = buildInput("Sorceress", 7, d2sPre, D2S_POST, d2xPre.present ? &d2xPre.bytes : NULL,
		D2X_POST, commitPointPart);
	Bytes j = encode(in);
	if (!j.empty() && state != COMMIT_PREPARED)
		CommitJournalSetState(j.data(), (DWORD)j.size(), state);
	return j;
}

// The character's files as recovery sees and changes them.
struct Disk
{
	File d2s;
	File d2x;
	File journal;
	File archivedJournal;
	File archivedD2x;
};

static void decide(const Disk& disk, bool loadedHere, CommitRecovery* r)
{
	CommitDiskFile s = describe(disk.d2s);
	CommitDiskFile x = describe(disk.d2x);
	CommitDecideRecovery(disk.journal.present ? COMMIT_FILE_PRESENT : COMMIT_FILE_ABSENT, ptr(disk.journal.bytes),
		(DWORD)disk.journal.bytes.size(), loadedHere, &s, &x, r);
}

// Runs the plan in order like the plugy side; failAt makes that action fail, and nothing after it runs.
static int apply(Disk& disk, const CommitRecovery& r, int failAt = -1)
{
	const CommitJournal& j = r.journal;
	int done = 0;
	for (int i = 0; i < r.actionCount; i++, done++)
	{
		if (i == failAt)
			break;
		switch (r.actions[i])
		{
		case COMMIT_ACTION_WRITE_D2S_POST:
			disk.d2s = present(Bytes(j.d2s.postBytes, j.d2s.postBytes + j.d2s.postSize));
			break;
		case COMMIT_ACTION_WRITE_D2X_POST:
			disk.d2x = present(Bytes(j.d2x.postBytes, j.d2x.postBytes + j.d2x.postSize));
			break;
		case COMMIT_ACTION_WRITE_D2X_PRE:
			disk.d2x = present(Bytes(j.d2x.preBytes, j.d2x.preBytes + j.d2x.preSize));
			break;
		case COMMIT_ACTION_DELETE_D2X:
			disk.d2x = absent();
			break;
		case COMMIT_ACTION_ARCHIVE_D2X:
			disk.archivedD2x = disk.d2x;
			disk.d2x = absent();
			break;
		case COMMIT_ACTION_ARCHIVE_JOURNAL:
			disk.archivedJournal = disk.journal;
			disk.journal = absent();
			break;
		case COMMIT_ACTION_DELETE_JOURNAL:
			disk.journal = absent();
			break;
		}
	}
	return done;
}

static int indexOf(const CommitRecovery& r, CommitAction action)
{
	for (int i = 0; i < r.actionCount; i++)
		if (r.actions[i] == action)
			return i;
	return -1;
}

// --- FNV-1a ---------------------------------------------------------------------------------------------------------

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
	printf("CommitFnv1a\n");
	CHECK(CommitFnv1a(NULL, 0) == 0x811C9DC5);
	CHECK(CommitFnv1a((const BYTE*)"a", 1) == 0xE40C292C);
	CHECK(CommitFnv1a((const BYTE*)"foobar", 6) == 0xBF9CF968);
	CHECK(CommitFnv1a((const BYTE*)"bar", 3, CommitFnv1a((const BYTE*)"foo", 3)) == 0xBF9CF968);
	CHECK(commonFnv1a((const BYTE*)"foobar", 6) == 0xBF9CF968);
	BYTE all[256];
	for (int i = 0; i < 256; i++)
		all[i] = (BYTE)i;
	CHECK(CommitFnv1a(all, sizeof(all)) == commonFnv1a(all, sizeof(all)));
	CHECK(CommitFnv1a(all, 100, 12345) == commonFnv1a(all, 100, 12345));
}

// --- Fence ----------------------------------------------------------------------------------------------------------

static void testFence()
{
	printf("Fence\n");
	Bytes s = stub();
	CHECK_EQ(s.size(), COMMIT_FENCE_STUB_SIZE);
	CHECK(s[4] == '0' && s[5] == 'J');
	CHECK(CommitIsFenced(s.data(), 6));
	CHECK(CommitIsFenceStub(s.data(), 6));
	CHECK(!CommitIsFenced(s.data(), 5));
	CHECK(!CommitIsFenced(NULL, 0));
	CHECK(!CommitIsFenced(D2X_PRE.data(), (DWORD)D2X_PRE.size()));
	CHECK(CommitIsFenced(D2X_OTHER_FENCED.data(), (DWORD)D2X_OTHER_FENCED.size()));
	CHECK(!CommitIsFenceStub(D2X_OTHER_FENCED.data(), (DWORD)D2X_OTHER_FENCED.size()));

	// A tagged pre file keeps every byte but the version.
	Bytes f = fenced(D2X_PRE);
	CHECK_EQ(f.size(), D2X_PRE.size());
	CHECK(CommitIsFenced(f.data(), (DWORD)f.size()));
	Bytes unfenced = f;
	unfenced[4] = D2X_PRE[4];
	unfenced[5] = D2X_PRE[5];
	CHECK(unfenced == D2X_PRE);

	// Absent, shorter than 6 bytes, or untagged: the stub.
	CHECK(fenced(Bytes()) == s);
	CHECK(fenced(SHORT_D2X) == s);
	CHECK(fenced(bytesOf("CSTM0")) == s);
	CHECK(fenced(bytesOf("garbage file")) == s);
	CHECK(fenced(d2xFile(COMMIT_D2X_VERSION, "")) == s);
	CHECK_EQ(CommitFencedSize(SHORT_D2X.data(), 3), COMMIT_FENCE_STUB_SIZE);

	BYTE fiveBytes[5];
	CHECK_EQ(CommitFence(NULL, 0, fiveBytes, sizeof(fiveBytes)), 0);
	CHECK_EQ(CommitFence(D2X_PRE.data(), (DWORD)D2X_PRE.size(), NULL, 100), 0);

	CommitDiskFile d = CommitFilePresent(s.data(), 6);
	CHECK(d.state == COMMIT_FILE_PRESENT && d.size == 6 && d.fenced && d.hash == fnv(s));
	d = CommitFilePresent(NULL, 0);
	CHECK(d.state == COMMIT_FILE_PRESENT && d.size == 0 && !d.fenced && d.hash == COMMIT_FNV_INIT);
	CHECK(CommitFileAbsent().state == COMMIT_FILE_ABSENT);
	CHECK(CommitFileUnreadable().state == COMMIT_FILE_UNREADABLE);
}

// --- Encode / parse -------------------------------------------------------------------------------------------------

static void checkPart(const CommitPart& p, const Bytes* pre, DWORD preSize, DWORD preHash, const Bytes& post, int line)
{
	check(p.preSize == preSize && p.preHash == preHash, "pre size and hash", line);
	check(p.postSize == post.size() && p.postHash == fnv(post), "post size and hash", line);
	check(p.postBytes && Bytes(p.postBytes, p.postBytes + p.postSize) == post, "post bytes", line);
	if (pre)
		check(pre->empty() ? !p.preBytes : (p.preBytes && Bytes(p.preBytes, p.preBytes + p.preSize) == *pre),
			"pre bytes", line);
}

static void testRoundTrip()
{
	printf("Journal round trip\n");
	CommitBuildInput in = buildInput("Sorceress", 41, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, 0);
	Bytes j = encode(in);
	CHECK_EQ(j.size(), COMMIT_HEADER_SIZE + 2 * (COMMIT_PART_HEADER_SIZE + 16) + D2S_POST.size() + D2X_PRE.size()
		+ D2X_POST.size());
	CHECK(j[0] == 'P' && j[1] == 'Y' && j[2] == 'J' && j[3] == '1');
	CommitJournal p;
	CHECK_EQ(CommitJournalParse(j.data(), (DWORD)j.size(), &p), COMMIT_PARSE_OK);
	CHECK_EQ(p.state, COMMIT_PREPARED);
	CHECK_EQ(p.seq, 41);
	CHECK(!strcmp(p.name, "Sorceress"));
	BYTE zero[COMMIT_GUID_SIZE] = {};
	CHECK(!memcmp(p.charGuid, zero, COMMIT_GUID_SIZE));
	checkPart(p.d2s, NULL, (DWORD)D2S_PRE.size(), fnv(D2S_PRE), D2S_POST, __LINE__);
	CHECK(!p.d2s.preBytes);
	checkPart(p.d2x, &D2X_PRE, (DWORD)D2X_PRE.size(), fnv(D2X_PRE), D2X_POST, __LINE__);
	CHECK_EQ(p.d2s.flags, 0);
	CHECK_EQ(p.d2x.flags, 0);
	Bytes f = fenced(D2X_PRE);
	CHECK(p.d2x.fencedSize == f.size() && p.d2x.fencedHash == fnv(f));

	// Step 4 flips the state in place and the payload hash still holds.
	Bytes c = j;
	CHECK(CommitJournalSetState(c.data(), (DWORD)c.size(), COMMIT_COMMITTED));
	CHECK_EQ(CommitJournalParse(c.data(), (DWORD)c.size(), &p), COMMIT_PARSE_OK);
	CHECK_EQ(p.state, COMMIT_COMMITTED);
	CHECK(!memcmp(c.data() + 44, j.data() + 44, 4));
	CHECK(!CommitJournalSetState(c.data(), (DWORD)c.size(), 3));
	CHECK(!CommitJournalSetState(c.data(), 10, COMMIT_COMMITTED));

	printf("Journal round trip: absent and short files\n");
	// No .d2s yet (a new character) and no .d2x yet: preSize 0 and the empty hash.
	in = buildInput("Amazon", 1, absent(), D2S_POST, NULL, D2X_POST, 0);
	j = encode(in);
	CHECK_EQ(CommitJournalParse(j.data(), (DWORD)j.size(), &p), COMMIT_PARSE_OK);
	checkPart(p.d2s, NULL, 0, COMMIT_FNV_INIT, D2S_POST, __LINE__);
	Bytes none;
	checkPart(p.d2x, &none, 0, COMMIT_FNV_INIT, D2X_POST, __LINE__);
	CHECK(p.d2x.fencedSize == COMMIT_FENCE_STUB_SIZE && p.d2x.fencedHash == fnv(stub()));

	// A 0-byte .d2s is recorded like an absent one; a short .d2x keeps its exact bytes behind the stub.
	in = buildInput("Amazon", 2, present(Bytes()), D2S_POST, &SHORT_D2X, D2X_POST, 0);
	j = encode(in);
	CHECK_EQ(CommitJournalParse(j.data(), (DWORD)j.size(), &p), COMMIT_PARSE_OK);
	checkPart(p.d2s, NULL, 0, COMMIT_FNV_INIT, D2S_POST, __LINE__);
	checkPart(p.d2x, &SHORT_D2X, 3, fnv(SHORT_D2X), D2X_POST, __LINE__);
	CHECK(p.d2x.fencedSize == COMMIT_FENCE_STUB_SIZE && p.d2x.fencedHash == fnv(stub()));

	// The commit-point flag lands on the named part only.
	in = buildInput("Amazon", 3, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, COMMIT_PART_D2X);
	j = encode(in);
	CHECK_EQ(CommitJournalParse(j.data(), (DWORD)j.size(), &p), COMMIT_PARSE_OK);
	CHECK_EQ(p.d2x.flags, COMMIT_PART_FLAG_COMMIT_POINT);
	CHECK_EQ(p.d2s.flags, 0);
	in.commitPointPart = COMMIT_PART_D2S;
	j = encode(in);
	CHECK_EQ(CommitJournalParse(j.data(), (DWORD)j.size(), &p), COMMIT_PARSE_OK);
	CHECK_EQ(p.d2s.flags, COMMIT_PART_FLAG_COMMIT_POINT);

	printf("Journal build refusals\n");
	const Bytes fencedPre = fenced(D2X_PRE);
	const Bytes stubFile = stub();
	in = buildInput("Fifteen_chars_x", 1, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, 0);
	j = encode(in);
	CHECK_EQ(CommitJournalParse(j.data(), (DWORD)j.size(), &p), COMMIT_PARSE_OK);
	CHECK(!strcmp(p.name, "Fifteen_chars_x"));
	in.name = "Sixteen_chars_xx";
	CHECK_EQ(CommitJournalSize(&in), 0);
	in.name = "";
	CHECK_EQ(CommitJournalSize(&in), 0);
	in.name = NULL;
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, 0);
	in.d2sPre = CommitFileUnreadable();
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), Bytes(), &D2X_PRE, D2X_POST, 0);
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &D2X_PRE, Bytes(), 0);
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &fencedPre, D2X_POST, 0);
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &stubFile, D2X_POST, 0);
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, COMMIT_PART_SSS);
	CHECK_EQ(CommitJournalSize(&in), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, 0);
	in.d2xPre = NULL;
	CHECK_EQ(CommitJournalSize(&in), 0);
	CHECK_EQ(CommitJournalSize(NULL), 0);
	in = buildInput("Amazon", 1, present(D2S_PRE), D2S_POST, &D2X_PRE, D2X_POST, 0);
	Bytes tooSmall(CommitJournalSize(&in) - 1);
	CHECK_EQ(CommitJournalEncode(&in, tooSmall.data(), (DWORD)tooSmall.size()), 0);
	in.d2xPostSize += COMMIT_MAX_FILE_SIZE - CommitJournalSize(&in);
	CHECK_EQ(CommitJournalSize(&in), COMMIT_MAX_FILE_SIZE);
	in.d2xPostSize++;
	CHECK_EQ(CommitJournalSize(&in), 0);
}

// A journal assembled field by field, for the shapes the encoder never writes.
static Bytes rawHeader(BYTE state, BYTE partCount, const char* name)
{
	Bytes h(COMMIT_HEADER_SIZE, 0);
	putDword(h, 0, COMMIT_JOURNAL_MAGIC);
	h[4] = COMMIT_JOURNAL_VERSION;
	h[6] = state;
	h[7] = partCount;
	putDword(h, 8, 1);
	memcpy(&h[28], name, strlen(name));
	return h;
}

static void rawPart(Bytes& j, DWORD type, DWORD flags, DWORD preHash, DWORD preSize, const Bytes& pre, const Bytes& post)
{
	appendDword(j, type);
	appendDword(j, (DWORD)(16 + pre.size() + post.size()));
	appendDword(j, flags);
	appendDword(j, preHash);
	appendDword(j, preSize);
	appendDword(j, fnv(post));
	appendDword(j, (DWORD)post.size());
	append(j, pre);
	append(j, post);
}

static Bytes rawJournal(DWORD d2sFlags, DWORD d2xFlags)
{
	Bytes j = rawHeader(COMMIT_PREPARED, 2, "Necro");
	rawPart(j, COMMIT_PART_D2S, d2sFlags, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), D2S_POST);
	rawPart(j, COMMIT_PART_D2X, d2xFlags, fnv(D2X_PRE), (DWORD)D2X_PRE.size(), D2X_PRE, D2X_POST);
	reseal(j);
	return j;
}

static CommitParseStatus parseOf(const Bytes& j)
{
	CommitJournal p;
	return CommitJournalParse(ptr(j), (DWORD)j.size(), &p);
}

// Every parse failure quarantines the journal and plans nothing, so the journal is kept.
static void checkQuarantined(const Bytes& j, CommitParseStatus expected, int line)
{
	checkInt(parseOf(j), expected, "parse status", line);
	Disk disk = {present(D2S_PRE), present(fenced(D2X_PRE)), present(j), absent(), absent()};
	CommitRecovery r;
	decide(disk, false, &r);
	check(r.verdict == COMMIT_VERDICT_QUARANTINE && r.cause == COMMIT_QUARANTINE_PARSE && r.parse == expected,
		"quarantine verdict", line);
	check(r.actionCount == 0, "nothing planned", line);
}

static void testParseFailures()
{
	printf("Parse failures\n");
	Bytes good = journalOf(COMMIT_PREPARED, present(D2S_PRE), present(D2X_PRE));
	CHECK_EQ(parseOf(good), COMMIT_PARSE_OK);
	CHECK_EQ(parseOf(rawJournal(0, 0)), COMMIT_PARSE_OK);
	const size_t part2 = COMMIT_HEADER_SIZE + COMMIT_PART_HEADER_SIZE + 16 + D2S_POST.size();

	checkQuarantined(Bytes(), COMMIT_PARSE_TRUNCATED, __LINE__);
	checkQuarantined(Bytes(good.begin(), good.begin() + 3), COMMIT_PARSE_TRUNCATED, __LINE__);
	checkQuarantined(Bytes(good.begin(), good.begin() + 5), COMMIT_PARSE_TRUNCATED, __LINE__);
	checkQuarantined(Bytes(good.begin(), good.begin() + COMMIT_HEADER_SIZE - 1), COMMIT_PARSE_TRUNCATED, __LINE__);

	Bytes j = good;
	j[0] = 'X';
	checkQuarantined(j, COMMIT_PARSE_BAD_MAGIC, __LINE__);
	checkQuarantined(bytesOf("not a journal at all, just text"), COMMIT_PARSE_BAD_MAGIC, __LINE__);
	j = good;
	j[4] = 2;
	checkQuarantined(j, COMMIT_PARSE_BAD_VERSION, __LINE__);

	j = good;
	j[j.size() - 1] ^= 1;
	checkQuarantined(j, COMMIT_PARSE_BAD_HASH, __LINE__);
	checkQuarantined(Bytes(good.begin(), good.end() - 1), COMMIT_PARSE_BAD_HASH, __LINE__);
	checkQuarantined(Bytes(good.begin(), good.begin() + COMMIT_HEADER_SIZE), COMMIT_PARSE_BAD_HASH, __LINE__);

	j = good;
	j[6] = 3;
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j[6] = 0;
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	j[28 + 12] = 'x';
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	memset(&j[28], 'x', COMMIT_NAME_SIZE);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	memset(&j[28], 0, COMMIT_NAME_SIZE);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);

	printf("Parse failures: parts\n");
	j = good;
	j.push_back(0);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	j[7] = 3;
	checkQuarantined(j, COMMIT_PARSE_TRUNCATED, __LINE__);
	j = good;
	j[7] = 1;
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j.resize(part2);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	j[7] = 0;
	j.resize(COMMIT_HEADER_SIZE);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	putDword(j, part2 + 4, (DWORD)(j.size() - part2 - COMMIT_PART_HEADER_SIZE + 1));
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_TRUNCATED, __LINE__);
	j = good;
	j.resize(j.size() - 1);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_TRUNCATED, __LINE__);
	j = good;
	putDword(j, COMMIT_HEADER_SIZE + 4, (DWORD)(16 + D2S_POST.size() - 1));
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	j[COMMIT_HEADER_SIZE + COMMIT_PART_HEADER_SIZE + 16] ^= 1;
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	j[part2 + COMMIT_PART_HEADER_SIZE + 16 + 7] ^= 1;
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = good;
	j[part2 + 8] = 2;
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	checkQuarantined(rawJournal(COMMIT_PART_FLAG_COMMIT_POINT, COMMIT_PART_FLAG_COMMIT_POINT), COMMIT_PARSE_MALFORMED,
		__LINE__);

	// Two .d2s parts, an empty post, a .d2s pre of size 0 with a non-empty hash, a fenced pre .d2x.
	j = rawHeader(COMMIT_PREPARED, 2, "Necro");
	rawPart(j, COMMIT_PART_D2S, 0, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), D2S_POST);
	rawPart(j, COMMIT_PART_D2S, 0, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), D2S_POST);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = rawHeader(COMMIT_PREPARED, 2, "Necro");
	rawPart(j, COMMIT_PART_D2S, 0, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), Bytes());
	rawPart(j, COMMIT_PART_D2X, 0, fnv(D2X_PRE), (DWORD)D2X_PRE.size(), D2X_PRE, D2X_POST);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	j = rawHeader(COMMIT_PREPARED, 2, "Necro");
	rawPart(j, COMMIT_PART_D2S, 0, 1234, 0, Bytes(), D2S_POST);
	rawPart(j, COMMIT_PART_D2X, 0, fnv(D2X_PRE), (DWORD)D2X_PRE.size(), D2X_PRE, D2X_POST);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);
	Bytes f = fenced(D2X_PRE);
	j = rawHeader(COMMIT_PREPARED, 2, "Necro");
	rawPart(j, COMMIT_PART_D2S, 0, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), D2S_POST);
	rawPart(j, COMMIT_PART_D2X, 0, fnv(f), (DWORD)f.size(), f, D2X_POST);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_MALFORMED, __LINE__);

	// Parts in the other order are fine.
	j = rawHeader(COMMIT_PREPARED, 2, "Necro");
	rawPart(j, COMMIT_PART_D2X, 0, fnv(D2X_PRE), (DWORD)D2X_PRE.size(), D2X_PRE, D2X_POST);
	rawPart(j, COMMIT_PART_D2S, 0, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), D2S_POST);
	reseal(j);
	CHECK_EQ(parseOf(j), COMMIT_PARSE_OK);

	printf("Parse failures: unknown and reserved parts\n");
	for (int type = 0; type < 3; type++)
	{
		DWORD id = type == 0 ? 0 : type == 1 ? 7 : 0xFFFFFFFF;
		j = good;
		j[7] = 3;
		appendDword(j, id);
		appendDword(j, 0);
		appendDword(j, 0);
		reseal(j);
		checkQuarantined(j, COMMIT_PARSE_UNKNOWN_PART, __LINE__);
	}
	// A newer build's shared stash part is refused, wherever it sits and whatever it holds.
	j = good;
	j[7] = 3;
	rawPart(j, COMMIT_PART_SSS, COMMIT_PART_FLAG_COMMIT_POINT, 0, 0, Bytes(), bytesOf("shared stash post"));
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_RESERVED_PART, __LINE__);
	j = rawHeader(COMMIT_COMMITTED, 3, "Necro");
	rawPart(j, COMMIT_PART_SSS, 0, 0, 0, Bytes(), bytesOf("shared stash post"));
	rawPart(j, COMMIT_PART_D2S, 0, fnv(D2S_PRE), (DWORD)D2S_PRE.size(), Bytes(), D2S_POST);
	rawPart(j, COMMIT_PART_D2X, 0, fnv(D2X_PRE), (DWORD)D2X_PRE.size(), D2X_PRE, D2X_POST);
	reseal(j);
	checkQuarantined(j, COMMIT_PARSE_RESERVED_PART, __LINE__);

	for (int s = COMMIT_PARSE_OK; s <= COMMIT_PARSE_RESERVED_PART; s++)
		CHECK(strcmp(CommitParseStatusName((CommitParseStatus)s), "?") != 0);
}

// An open or read error is not a parse failure: no quarantine, no box, nothing planned.
static void testReadErrors()
{
	printf("Read errors\n");
	Bytes j = journalOf(COMMIT_COMMITTED, present(D2S_PRE), present(D2X_PRE));
	CommitDiskFile s = describe(present(D2S_PRE));
	CommitDiskFile x = describe(present(fenced(D2X_PRE)));
	CommitDiskFile bad = CommitFileUnreadable();
	CommitRecovery r;
	CommitDecideRecovery(COMMIT_FILE_UNREADABLE, NULL, 0, false, &s, &x, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_READ_ERROR);
	CHECK_EQ(r.actionCount, 0);
	CHECK(!CommitVerdictProceeds(r.verdict));
	CommitDecideRecovery(COMMIT_FILE_PRESENT, j.data(), (DWORD)j.size(), false, &bad, &x, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_READ_ERROR);
	CHECK_EQ(r.actionCount, 0);
	CommitDecideRecovery(COMMIT_FILE_PRESENT, j.data(), (DWORD)j.size(), true, &s, &bad, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_READ_ERROR);
	// A parse failure outranks an unreadable target: it never heals on a retry.
	j[j.size() - 1] ^= 1;
	CommitDecideRecovery(COMMIT_FILE_PRESENT, j.data(), (DWORD)j.size(), false, &bad, &bad, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_QUARANTINE);
	// With no journal an unreadable .d2x is left to the save and load code, as before.
	CommitDecideRecovery(COMMIT_FILE_ABSENT, NULL, 0, false, &s, &bad, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_NONE);
}

static void testNoJournal()
{
	printf("No journal\n");
	File cases[] = {absent(), present(Bytes()), present(D2X_PRE), present(SHORT_D2X), present(fenced(D2X_PRE)),
		present(stub())};
	const bool quarantines[] = {false, false, false, false, true, true};
	for (int i = 0; i < 6; i++)
	{
		Disk disk = {present(D2S_PRE), cases[i], absent(), absent(), absent()};
		CommitRecovery r;
		decide(disk, true, &r);
		char cell[32];
		sprintf(cell, "d2x case %d", i);
		CHECK_CELL(r.verdict == (quarantines[i] ? COMMIT_VERDICT_QUARANTINE : COMMIT_VERDICT_NONE));
		CHECK_CELL(r.cause == (quarantines[i] ? COMMIT_QUARANTINE_FENCE_WITHOUT_JOURNAL : COMMIT_QUARANTINE_NONE));
		CHECK_CELL(r.actionCount == 0);
	}
	CHECK(CommitVerdictProceeds(COMMIT_VERDICT_NONE));
	CHECK(!CommitVerdictProceeds(COMMIT_VERDICT_QUARANTINE));
}

// --- Commit test ---------------------------------------------------------------------------------------------------

static void testCommitTest()
{
	printf("Commit test\n");
	CommitJournal p;
	CommitDiskFile d2sPre = describe(present(D2S_PRE));
	CommitDiskFile d2sPost = describe(present(D2S_POST));
	CommitDiskFile d2xFenced = describe(present(fenced(D2X_PRE)));
	CommitDiskFile d2xPost = describe(present(D2X_POST));
	CommitDiskFile none = CommitFileAbsent();

	Bytes j = journalOf(COMMIT_COMMITTED, present(D2S_PRE), present(D2X_PRE));
	CommitJournalParse(j.data(), (DWORD)j.size(), &p);
	CHECK(CommitIsCommitted(&p, &none, &none));
	CHECK(CommitIsCommitted(&p, &d2sPre, &d2xFenced));

	// PREPARED without a commit-point part: never committed, whatever the disk holds.
	j = journalOf(COMMIT_PREPARED, present(D2S_PRE), present(D2X_PRE));
	CommitJournalParse(j.data(), (DWORD)j.size(), &p);
	CHECK(!CommitIsCommitted(&p, &d2sPost, &d2xPost));

	// A synthetic commit point on the .d2x: its target holding post bytes proves the commit.
	j = journalOf(COMMIT_PREPARED, present(D2S_PRE), present(D2X_PRE), COMMIT_PART_D2X);
	CommitJournalParse(j.data(), (DWORD)j.size(), &p);
	CHECK(CommitIsCommitted(&p, &d2sPre, &d2xPost));
	CHECK(!CommitIsCommitted(&p, &d2sPost, &d2xFenced));
	CHECK(!CommitIsCommitted(&p, &d2sPost, &none));
	Disk disk = {present(D2S_PRE), present(D2X_POST), present(j), absent(), absent()};
	CommitRecovery r;
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_FORWARD);
	CHECK(r.committed);
	CHECK_EQ(r.actionCount, 2);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_WRITE_D2S_POST);
	CHECK_EQ(r.actions[1], COMMIT_ACTION_DELETE_JOURNAL);
	disk.d2x = present(fenced(D2X_PRE));
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_BACK);
	CHECK(!r.committed);

	// On the .d2s.
	j = journalOf(COMMIT_PREPARED, present(D2S_PRE), present(D2X_PRE), COMMIT_PART_D2S);
	CommitJournalParse(j.data(), (DWORD)j.size(), &p);
	CHECK(CommitIsCommitted(&p, &d2sPost, &d2xFenced));
	CHECK(!CommitIsCommitted(&p, &d2sPre, &d2xPost));
	Disk disk2 = {present(D2S_POST), present(fenced(D2X_PRE)), present(j), absent(), absent()};
	decide(disk2, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_FORWARD);
	CHECK_EQ(r.actionCount, 2);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_WRITE_D2X_POST);
	CHECK(!r.rewritesD2s);
}

// --- Verdict matrix ------------------------------------------------------------------------------------------------

struct Case
{
	const char* label;
	File file;
};

// The rules restated per named case, one letter per .d2x column: N none, R read error, Q quarantine, S skip,
// D orphan (deleted), O orphan (superseded), F roll forward, B roll back.
static char verdictLetter(CommitVerdict v)
{
	switch (v)
	{
	case COMMIT_VERDICT_NONE: return 'N';
	case COMMIT_VERDICT_READ_ERROR: return 'R';
	case COMMIT_VERDICT_QUARANTINE: return 'Q';
	case COMMIT_VERDICT_SKIP: return 'S';
	case COMMIT_VERDICT_ORPHAN_DELETED: return 'D';
	case COMMIT_VERDICT_ORPHAN_SUPERSEDED: return 'O';
	case COMMIT_VERDICT_ROLL_FORWARD: return 'F';
	case COMMIT_VERDICT_ROLL_BACK: return 'B';
	}
	return '?';
}

static int matrixCells = 0;

// Runs every (state x .d2s x .d2x x loaded) cell against the expected letters, then checks what the plan does to
// the files and that the next recovery has nothing left to do (or refuses the same way again).
static void runMatrix(const char* name, const File& d2sPre, const File& d2xPre, const Case* d2sCases, int nbD2s,
	const Case* d2xCases, int nbD2x, const char* const* expectPrepared, const char* const* expectCommitted)
{
	printf("Verdict matrix: %s\n", name);
	for (int st = 0; st < 2; st++)
	{
		BYTE state = st ? COMMIT_COMMITTED : COMMIT_PREPARED;
		Bytes journal = journalOf(state, d2sPre, d2xPre);
		for (int loaded = 0; loaded < 2; loaded++)
		for (int si = 0; si < nbD2s; si++)
		for (int xi = 0; xi < nbD2x; xi++)
		{
			char cell[160];
			sprintf(cell, "%s %s d2s=%s d2x=%s %s", name, st ? "COMMITTED" : "PREPARED", d2sCases[si].label,
				d2xCases[xi].label, loaded ? "loaded" : "never loaded");
			matrixCells++;
			const Disk before = {d2sCases[si].file, d2xCases[xi].file, present(journal), absent(), absent()};
			CommitRecovery r;
			decide(before, loaded != 0, &r);

			char expected = (st ? expectCommitted : expectPrepared)[si][xi];
			if (expected == 'D' && loaded)
				expected = 'S';
			char got = verdictLetter(r.verdict);
			if (got != expected)
				printf("    got %c, expected %c\n", got, expected);
			CHECK_CELL(got == expected);
			CHECK_CELL(r.parse == COMMIT_PARSE_OK);
			CHECK_CELL(r.committed == (st == 1));

			Disk after = before;
			int done = apply(after, r);
			CHECK_CELL(done == r.actionCount);
			if (!CommitVerdictProceeds(r.verdict))
			{
				CHECK_CELL(r.actionCount == 0);
				CommitRecovery again;
				decide(after, loaded != 0, &again);
				CHECK_CELL(again.verdict == r.verdict);
				continue;
			}
			CHECK_CELL(r.actionCount >= 1);
			CHECK_CELL(!after.journal.present);
			CommitRecovery next;
			decide(after, loaded != 0, &next);
			CHECK_CELL(next.verdict == COMMIT_VERDICT_NONE);

			const CommitJournal& j = r.journal;
			File post2s = present(D2S_POST);
			File post2x = present(D2X_POST);
			switch (r.verdict)
			{
			case COMMIT_VERDICT_ROLL_FORWARD:
				CHECK_CELL(sameFile(after.d2s, post2s) && sameFile(after.d2x, post2x));
				CHECK_CELL(r.rewritesD2s == !sameFile(before.d2s, post2s));
				CHECK_CELL(r.actions[r.actionCount - 1] == COMMIT_ACTION_DELETE_JOURNAL);
				CHECK_CELL(indexOf(r, COMMIT_ACTION_WRITE_D2X_POST) < 0
					|| indexOf(r, COMMIT_ACTION_WRITE_D2S_POST) < indexOf(r, COMMIT_ACTION_WRITE_D2X_POST));
				break;
			case COMMIT_VERDICT_ROLL_BACK:
				CHECK_CELL(sameFile(after.d2s, before.d2s));
				CHECK_CELL(!r.rewritesD2s);
				if (j.d2x.preSize)
					CHECK_CELL(after.d2x.present
						&& after.d2x.bytes == Bytes(j.d2x.preBytes, j.d2x.preBytes + j.d2x.preSize));
				else
					CHECK_CELL(!after.d2x.present || after.d2x.bytes.empty());
				CHECK_CELL(r.actions[r.actionCount - 1] == COMMIT_ACTION_DELETE_JOURNAL);
				break;
			case COMMIT_VERDICT_ORPHAN_DELETED:
				CHECK_CELL(sameFile(after.d2s, before.d2s));
				CHECK_CELL(!after.d2x.present);
				CHECK_CELL(sameFile(after.archivedD2x, before.d2x));
				CHECK_CELL(sameFile(after.archivedJournal, before.journal));
				break;
			case COMMIT_VERDICT_ORPHAN_SUPERSEDED:
				CHECK_CELL(sameFile(after.d2s, before.d2s) && sameFile(after.d2x, before.d2x));
				CHECK_CELL(!after.archivedD2x.present);
				CHECK_CELL(sameFile(after.archivedJournal, before.journal));
				break;
			default:
				CHECK_CELL(false);
				break;
			}
		}
	}
}

static void testMatrix()
{
	// Columns: absent, 0-byte, unfenced pre, fenced pre, post, another unfenced, another fenced.
	const Case d2xCanonical[] = {
		{"absent", absent()}, {"empty", present(Bytes())}, {"pre", present(D2X_PRE)},
		{"fenced-pre", present(fenced(D2X_PRE))}, {"post", present(D2X_POST)}, {"other", present(D2X_OTHER)},
		{"other-fenced", present(D2X_OTHER_FENCED)}};
	const Case d2sCanonical[] = {
		{"absent", absent()}, {"empty", present(Bytes())}, {"pre", present(D2S_PRE)}, {"post", present(D2S_POST)},
		{"other", present(D2S_OTHER)}};
	const char* const prepared[] = {"DDDDDDD", "OOQQOOQ", "QQBBQQQ", "QQQQQQQ", "OOQQOOQ"};
	const char* const committed[] = {"DDDDDDD", "OOQQOOQ", "QQFFFQQ", "QQFFFQQ", "OOQQOOQ"};
	runMatrix("canonical", present(D2S_PRE), present(D2X_PRE), d2sCanonical, 5, d2xCanonical, 7, prepared,
		committed);

	// No .d2x when the commit began: the fence is the stub, and an absent or 0-byte .d2x is the pre file.
	const Case d2xStub[] = {
		{"absent", absent()}, {"empty", present(Bytes())}, {"stub", present(stub())}, {"post", present(D2X_POST)},
		{"other", present(D2X_OTHER)}, {"other-fenced", present(D2X_OTHER_FENCED)}};
	const char* const stubPrepared[] = {"DDDDDD", "QQQOOQ", "BBBQQQ", "QQQQQQ", "QQQOOQ"};
	const char* const stubCommitted[] = {"DDDDDD", "QQQOOQ", "FFFFQQ", "FFFFQQ", "QQQOOQ"};
	runMatrix("absent .d2x", present(D2S_PRE), absent(), d2sCanonical, 5, d2xStub, 6, stubPrepared, stubCommitted);

	// A .d2x shorter than 6 bytes: fenced with the stub, restored byte for byte.
	const Case d2xShort[] = {
		{"absent", absent()}, {"empty", present(Bytes())}, {"pre", present(SHORT_D2X)},
		{"stub", present(stub())}, {"post", present(D2X_POST)}, {"other", present(D2X_OTHER)},
		{"other-fenced", present(D2X_OTHER_FENCED)}};
	runMatrix("short .d2x", present(D2S_PRE), present(SHORT_D2X), d2sCanonical, 5, d2xShort, 7, prepared, committed);

	// A brand-new character (no .d2s when the commit began): rule 2a never applies.
	const Case d2sNew[] = {
		{"absent", absent()}, {"empty", present(Bytes())}, {"post", present(D2S_POST)}, {"other", present(D2S_OTHER)}};
	const char* const newPrepared[] = {"QQBBQQQ", "QQBBQQQ", "QQQQQQQ", "OOQQOOQ"};
	const char* const newCommitted[] = {"QQFFFQQ", "QQFFFQQ", "QQFFFQQ", "OOQQOOQ"};
	runMatrix("new character", absent(), present(D2X_PRE), d2sNew, 4, d2xCanonical, 7, newPrepared, newCommitted);

	printf("Verdict matrix: %d cells\n", matrixCells);
	for (int v = COMMIT_VERDICT_NONE; v <= COMMIT_VERDICT_ROLL_BACK; v++)
		CHECK(strcmp(CommitVerdictName((CommitVerdict)v), "?") != 0);
	for (int s = COMMIT_STEP_PREPARE; s <= COMMIT_STEP_END; s++)
		CHECK(strcmp(CommitStepName((CommitStep)s), "?") != 0);
}

// --- Orphans and the 0-byte rule -----------------------------------------------------------------------------------

static void testOrphans()
{
	printf("Orphans\n");
	Bytes j = journalOf(COMMIT_COMMITTED, present(D2S_PRE), present(D2X_PRE));
	CommitRecovery r;

	// Deleted character, never loaded here: the journal and the leftover .d2x (fenced or not) go to the archive.
	Disk disk = {absent(), present(fenced(D2X_PRE)), present(j), absent(), absent()};
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_DELETED);
	CHECK_EQ(r.actionCount, 2);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_ARCHIVE_D2X);
	CHECK_EQ(r.actions[1], COMMIT_ACTION_ARCHIVE_JOURNAL);
	disk.d2x = present(D2X_POST);
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_DELETED);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_ARCHIVE_D2X);
	disk.d2x = absent();
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_DELETED);
	CHECK_EQ(r.actionCount, 1);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_ARCHIVE_JOURNAL);

	// The character this process plays lost its .d2s: nothing is archived, the save is skipped.
	disk.d2x = present(fenced(D2X_PRE));
	decide(disk, true, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_SKIP);
	CHECK_EQ(r.actionCount, 0);
	CHECK(!CommitVerdictProceeds(r.verdict));

	// A 0-byte .d2s is present: not a deleted character. With the fence still on it matches no rule.
	disk.d2s = present(Bytes());
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_QUARANTINE);
	CHECK_EQ(r.cause, COMMIT_QUARANTINE_NO_RULE);

	// Another incarnation wrote both files: only the journal is archived, its unfenced .d2x stays.
	disk.d2s = present(D2S_OTHER);
	disk.d2x = present(D2X_OTHER);
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_SUPERSEDED);
	CHECK_EQ(r.actionCount, 1);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_ARCHIVE_JOURNAL);
	decide(disk, true, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_SUPERSEDED);
	disk.d2s = present(Bytes());
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_SUPERSEDED);
	// ... but not while the .d2x is still unfenced pre: the .d2s alone changed, which no rule explains.
	disk.d2s = present(D2S_OTHER);
	disk.d2x = present(D2X_PRE);
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_QUARANTINE);

	printf("Orphan suffix\n");
	SYSTEMTIME t = {};
	t.wYear = 2026;
	t.wMonth = 10;
	t.wDay = 3;
	t.wHour = 4;
	t.wMinute = 25;
	t.wSecond = 9;
	char suffix[32];
	CHECK(CommitOrphanSuffix(&t, suffix, sizeof(suffix)));
	CHECK(!strcmp(suffix, ".orphan-20261003-042509"));
	char tooSmall[10] = "untouched";
	CHECK(!CommitOrphanSuffix(&t, tooSmall, sizeof(tooSmall)));
	CHECK(!strcmp(tooSmall, ""));
	CHECK(!CommitOrphanSuffix(&t, NULL, 32));
}

// --- Commit steps --------------------------------------------------------------------------------------------------

// The disk a journaled save leaves after its first `steps` steps succeeded.
static Disk diskAfterSteps(int steps, const Bytes& prepared, const Bytes& committed)
{
	Disk disk = {present(D2S_PRE), present(D2X_PRE), absent(), absent(), absent()};
	if (steps > COMMIT_STEP_PREPARE)
		disk.journal = present(prepared);
	if (steps > COMMIT_STEP_FENCE)
		disk.d2x = present(fenced(D2X_PRE));
	if (steps > COMMIT_STEP_COMMIT)
		disk.journal = present(committed);
	if (steps > COMMIT_STEP_D2S)
		disk.d2s = present(D2S_POST);
	if (steps > COMMIT_STEP_D2X)
		disk.d2x = present(D2X_POST);
	if (steps > COMMIT_STEP_DELETE)
		disk.journal = absent();
	return disk;
}

static void testTxn()
{
	printf("Commit steps: seq and success\n");
	Bytes prepared = journalOf(COMMIT_PREPARED, present(D2S_PRE), present(D2X_PRE));
	Bytes committed = prepared;
	CommitJournalSetState(committed.data(), (DWORD)committed.size(), COMMIT_COMMITTED);
	CommitJournal pj;
	CommitJournalParse(prepared.data(), (DWORD)prepared.size(), &pj);
	const DWORD base = pj.seq - 1;

	CommitTxn txn;
	DWORD seq = base;
	CommitTxnBegin(&txn, seq);
	CHECK_EQ(txn.seq, base + 1);
	CHECK_EQ(txn.next, COMMIT_STEP_PREPARE);
	CHECK_EQ(txn.result, COMMIT_RESULT_PENDING);
	CHECK(!txn.journalOnDisk);
	CommitTxnStepDone(&txn, &seq);
	CHECK(txn.journalOnDisk);
	CHECK_EQ(seq, base);
	CommitTxnStepDone(&txn, &seq);
	CHECK_EQ(seq, base);
	CHECK(!txn.committed);
	CommitTxnStepDone(&txn, &seq);
	CHECK_EQ(seq, base + 1);
	CHECK(txn.committed);
	CommitTxnStepDone(&txn, &seq);
	CommitTxnStepDone(&txn, &seq);
	CHECK_EQ(txn.result, COMMIT_RESULT_PENDING);
	CHECK(txn.journalOnDisk);
	CommitTxnStepDone(&txn, &seq);
	CHECK_EQ(txn.next, COMMIT_STEP_END);
	CHECK_EQ(txn.result, COMMIT_RESULT_DONE);
	CHECK(!txn.journalOnDisk);
	CHECK_EQ(seq, base + 1);
	CommitTxnStepDone(&txn, &seq);
	CHECK_EQ(txn.next, COMMIT_STEP_END);
	CHECK_EQ(seq, base + 1);
	CommitTxnBegin(&txn, 0xFFFFFFFF);
	CHECK_EQ(txn.seq, 0);

	printf("Commit steps: a failure at each step\n");
	for (int failed = COMMIT_STEP_PREPARE; failed <= COMMIT_STEP_DELETE; failed++)
	{
		char cell[48];
		sprintf(cell, "failed step %d (%s)", failed + 2, CommitStepName((CommitStep)failed));
		seq = base;
		CommitTxnBegin(&txn, seq);
		for (int s = 0; s < failed; s++)
			CommitTxnStepDone(&txn, &seq);
		Disk disk = diskAfterSteps(failed, prepared, committed);
		CommitFailureAction action = CommitTxnStepFailed(&txn);
		CHECK_CELL(action == (failed <= COMMIT_STEP_COMMIT ? COMMIT_RECOVER_NOW : COMMIT_RETRY_LATER));
		CHECK_CELL(txn.journalOnDisk);
		if (action == COMMIT_RETRY_LATER)
		{
			CHECK_CELL(txn.result == COMMIT_RESULT_COMMITTED_INCOMPLETE);
			CHECK_CELL(seq == base + 1);
			// The retry resumes at the first target not yet holding its post bytes, in commit order.
			CommitRecovery r;
			decide(disk, true, &r);
			CHECK_CELL(r.verdict == COMMIT_VERDICT_ROLL_FORWARD);
			CommitAction first = failed == COMMIT_STEP_D2S ? COMMIT_ACTION_WRITE_D2S_POST
				: failed == COMMIT_STEP_D2X ? COMMIT_ACTION_WRITE_D2X_POST : COMMIT_ACTION_DELETE_JOURNAL;
			CHECK_CELL(r.actions[0] == first);
			CHECK_CELL(r.actionCount == COMMIT_STEP_END - failed);
			CommitTxnRecovered(&txn, &r, apply(disk, r), &seq);
			CHECK_CELL(txn.result == COMMIT_RESULT_DONE && !txn.journalOnDisk && seq == base + 1);
			CHECK_CELL(sameFile(disk.d2s, present(D2S_POST)) && sameFile(disk.d2x, present(D2X_POST)));
			continue;
		}
		// Before the commit point, the failed replace did not land: recovery rolls back.
		CommitRecovery r;
		decide(disk, true, &r);
		CHECK_CELL(r.verdict == (failed == COMMIT_STEP_PREPARE ? COMMIT_VERDICT_NONE : COMMIT_VERDICT_ROLL_BACK));
		CommitTxnRecovered(&txn, &r, apply(disk, r), &seq);
		CHECK_CELL(txn.result == COMMIT_RESULT_ABORTED && !txn.journalOnDisk && !txn.committed);
		CHECK_CELL(seq == base);
		CHECK_CELL(sameFile(disk.d2s, present(D2S_PRE)) && sameFile(disk.d2x, present(D2X_PRE)));
		CHECK_CELL(!disk.journal.present);
	}

	printf("Commit steps: a step 4 failure re-reads the disk\n");
	// The rename landed although it reported a failure: the save goes forward and the seq advances.
	seq = base;
	CommitTxnBegin(&txn, seq);
	for (int s = 0; s < COMMIT_STEP_COMMIT; s++)
		CommitTxnStepDone(&txn, &seq);
	Disk disk = diskAfterSteps(COMMIT_STEP_COMMIT + 1, prepared, committed);
	CHECK_EQ(CommitTxnStepFailed(&txn), COMMIT_RECOVER_NOW);
	CommitRecovery r;
	decide(disk, true, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_FORWARD);
	CHECK_EQ(r.actionCount, 3);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_WRITE_D2S_POST);
	CHECK_EQ(r.actions[1], COMMIT_ACTION_WRITE_D2X_POST);
	CHECK_EQ(r.actions[2], COMMIT_ACTION_DELETE_JOURNAL);
	CommitTxnRecovered(&txn, &r, apply(disk, r), &seq);
	CHECK(txn.committed);
	CHECK_EQ(seq, base + 1);
	CHECK_EQ(txn.result, COMMIT_RESULT_DONE);
	CHECK(!txn.journalOnDisk);
	// ... and if its own .d2s write then fails, the commit stays incomplete with the journal kept.
	seq = base;
	CommitTxnBegin(&txn, seq);
	for (int s = 0; s < COMMIT_STEP_COMMIT; s++)
		CommitTxnStepDone(&txn, &seq);
	disk = diskAfterSteps(COMMIT_STEP_COMMIT + 1, prepared, committed);
	CommitTxnStepFailed(&txn);
	decide(disk, true, &r);
	CommitTxnRecovered(&txn, &r, apply(disk, r, 0), &seq);
	CHECK_EQ(seq, base + 1);
	CHECK_EQ(txn.result, COMMIT_RESULT_COMMITTED_INCOMPLETE);
	CHECK(txn.journalOnDisk);
	CHECK(disk.journal.present);

	printf("Commit steps: a failed rollback restore keeps the journal\n");
	seq = base;
	CommitTxnBegin(&txn, seq);
	for (int s = 0; s < COMMIT_STEP_COMMIT; s++)
		CommitTxnStepDone(&txn, &seq);
	disk = diskAfterSteps(COMMIT_STEP_COMMIT, prepared, committed);
	CommitTxnStepFailed(&txn);
	decide(disk, true, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_BACK);
	CHECK_EQ(r.actionCount, 2);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_WRITE_D2X_PRE);
	CHECK_EQ(r.actions[1], COMMIT_ACTION_DELETE_JOURNAL);
	CommitTxnRecovered(&txn, &r, apply(disk, r, 0), &seq);
	CHECK_EQ(txn.result, COMMIT_RESULT_ABORTED);
	CHECK(txn.journalOnDisk);
	CHECK(disk.journal.present);
	CHECK_EQ(seq, base);
	// The leave fallback stays blocked while that journal is on disk; the next recovery finishes the rollback, and then
	// it may run.
	CHECK(!CommitLeaveFallbackAllowed(true, false, txn.journalOnDisk, COMMIT_FAIL_OTHER));
	decide(disk, true, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_BACK);
	CommitTxnRecovered(&txn, &r, apply(disk, r), &seq);
	CHECK_EQ(txn.result, COMMIT_RESULT_ABORTED);
	CHECK(!txn.journalOnDisk);
	CHECK(sameFile(disk.d2x, present(D2X_PRE)) && !disk.journal.present);
	CHECK(CommitLeaveFallbackAllowed(true, false, txn.journalOnDisk, COMMIT_FAIL_OTHER));
	// A refusal verdict on a retry touches nothing and keeps the commit's state.
	seq = base;
	CommitTxnBegin(&txn, seq);
	for (int s = 0; s <= COMMIT_STEP_COMMIT; s++)
		CommitTxnStepDone(&txn, &seq);
	CommitTxnStepFailed(&txn);
	disk = diskAfterSteps(COMMIT_STEP_D2S, prepared, committed);
	disk.d2s = present(D2S_OTHER);
	disk.d2x = present(fenced(D2X_PRE));
	decide(disk, true, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_QUARANTINE);
	CommitTxnRecovered(&txn, &r, apply(disk, r), &seq);
	CHECK_EQ(txn.result, COMMIT_RESULT_COMMITTED_INCOMPLETE);
	CHECK(txn.journalOnDisk);

	printf("Commit steps: in-order resume after a failed retry\n");
	disk = diskAfterSteps(COMMIT_STEP_D2S, prepared, committed);
	seq = base;
	CommitTxnBegin(&txn, seq);
	for (int s = 0; s < COMMIT_STEP_D2S; s++)
		CommitTxnStepDone(&txn, &seq);
	CommitTxnStepFailed(&txn);
	decide(disk, true, &r);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_WRITE_D2S_POST);
	CommitTxnRecovered(&txn, &r, apply(disk, r, 1), &seq);
	CHECK_EQ(txn.result, COMMIT_RESULT_COMMITTED_INCOMPLETE);
	CHECK(sameFile(disk.d2s, present(D2S_POST)) && sameFile(disk.d2x, present(fenced(D2X_PRE))));
	decide(disk, true, &r);
	CHECK_EQ(r.actionCount, 2);
	CHECK_EQ(r.actions[0], COMMIT_ACTION_WRITE_D2X_POST);
	CHECK(!r.rewritesD2s);
	CommitTxnRecovered(&txn, &r, apply(disk, r), &seq);
	CHECK_EQ(txn.result, COMMIT_RESULT_DONE);
	CHECK_EQ(seq, base + 1);
}

// --- Leave fallback ------------------------------------------------------------------------------------------------

static void testLeaveFallback()
{
	printf("Leave fallback\n");
	CHECK(CommitLeaveFallbackAllowed(true, false, false, COMMIT_FAIL_OTHER));
	CHECK(!CommitLeaveFallbackAllowed(false, false, false, COMMIT_FAIL_OTHER));
	CHECK(!CommitLeaveFallbackAllowed(true, true, false, COMMIT_FAIL_OTHER));
	CHECK(!CommitLeaveFallbackAllowed(true, false, true, COMMIT_FAIL_OTHER));
	CHECK(!CommitLeaveFallbackAllowed(true, false, false, COMMIT_FAIL_D2S_PRECHECK));
	CHECK(!CommitLeaveFallbackAllowed(true, false, false, COMMIT_FAIL_SESSION_LOCK));
}

// --- Load of a committed journal -----------------------------------------------------------------------------------

static void testLoadCommitted()
{
	printf("Load of a committed journal\n");
	Bytes j = journalOf(COMMIT_COMMITTED, present(D2S_PRE), present(D2X_PRE));
	CommitRecovery r;

	// Rolled forward at load: the .d2s is rewritten only while it still holds the old save.
	Disk disk = {present(D2S_POST), present(fenced(D2X_PRE)), present(j), absent(), absent()};
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ROLL_FORWARD);
	CHECK(!r.rewritesD2s);
	disk.d2s = present(D2S_PRE);
	decide(disk, false, &r);
	CHECK(r.rewritesD2s);

	// A committed journal of an earlier incarnation is only archived: this character's files stay as they are.
	disk.d2s = present(D2S_OTHER);
	disk.d2x = present(D2X_OTHER);
	decide(disk, false, &r);
	CHECK_EQ(r.verdict, COMMIT_VERDICT_ORPHAN_SUPERSEDED);
	CHECK(r.committed);
	CHECK(!r.rewritesD2s);
}

int main()
{
	testFnv();
	testFence();
	testRoundTrip();
	testParseFailures();
	testReadErrors();
	testNoJournal();
	testCommitTest();
	testMatrix();
	testOrphans();
	testTxn();
	testLeaveFallback();
	testLoadCommitted();
	printf("%d passed, %d failed\n", nbPassed, nbFailed);
	return nbFailed ? 1 : 0;
}
