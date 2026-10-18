/*=================================================================
	Crash-atomic single-player character saves: journal format,
	commit steps, recovery verdicts. See CharacterCommit.h.
=================================================================*/

#include "CharacterCommit.h"
#include <stdio.h>
#include <string.h>

// Header field offsets (COMMIT_HEADER_SIZE bytes in all).
#define OFS_MAGIC 0
#define OFS_VERSION 4
#define OFS_STATE 6
#define OFS_PART_COUNT 7
#define OFS_SEQ 8
#define OFS_GUID 12
#define OFS_NAME 28
#define OFS_PAYLOAD_HASH 44

// preHash, preSize, postHash, postSize: the fixed start of every part body.
#define PART_FIXED_SIZE 16

static DWORD getWord(const BYTE* p)
{
	return p[0] | (p[1] << 8);
}

static DWORD getDword(const BYTE* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24);
}

static void putWord(BYTE* p, DWORD v)
{
	p[0] = (BYTE)v;
	p[1] = (BYTE)(v >> 8);
}

static void putDword(BYTE* p, DWORD v)
{
	p[0] = (BYTE)v;
	p[1] = (BYTE)(v >> 8);
	p[2] = (BYTE)(v >> 16);
	p[3] = (BYTE)(v >> 24);
}

DWORD CommitFnv1a(const BYTE* data, DWORD size, DWORD hash)
{
	for (DWORD i = 0; i < size; i++)
	{
		hash ^= data[i];
		hash *= 0x01000193;
	}
	return hash;
}

CommitDiskFile CommitFileAbsent()
{
	CommitDiskFile file = {COMMIT_FILE_ABSENT, 0, COMMIT_FNV_INIT, false};
	return file;
}

CommitDiskFile CommitFileUnreadable()
{
	CommitDiskFile file = {COMMIT_FILE_UNREADABLE, 0, COMMIT_FNV_INIT, false};
	return file;
}

CommitDiskFile CommitFilePresent(const BYTE* data, DWORD size)
{
	CommitDiskFile file = {COMMIT_FILE_PRESENT, size, CommitFnv1a(data, size), CommitIsFenced(data, size)};
	return file;
}

static bool hasTag(const BYTE* data, DWORD size)
{
	return data && size >= COMMIT_FENCE_STUB_SIZE && getDword(data) == COMMIT_D2X_TAG;
}

bool CommitIsFenced(const BYTE* data, DWORD size)
{
	return hasTag(data, size) && getWord(data + 4) == COMMIT_D2X_VERSION_FENCED;
}

bool CommitIsFenceStub(const BYTE* data, DWORD size)
{
	return size == COMMIT_FENCE_STUB_SIZE && CommitIsFenced(data, size);
}

DWORD CommitFencedSize(const BYTE* pre, DWORD preSize)
{
	return hasTag(pre, preSize) ? preSize : COMMIT_FENCE_STUB_SIZE;
}

DWORD CommitFence(const BYTE* pre, DWORD preSize, BYTE* out, DWORD outSize)
{
	DWORD size = CommitFencedSize(pre, preSize);
	if (!out || outSize < size)
		return 0;
	if (hasTag(pre, preSize))
		memcpy(out, pre, preSize);
	else
		putDword(out, COMMIT_D2X_TAG);
	putWord(out + 4, COMMIT_D2X_VERSION_FENCED);
	return size;
}

// The hash of CommitFence's output, without building it.
static DWORD fencedHash(const BYTE* pre, DWORD preSize)
{
	BYTE head[COMMIT_FENCE_STUB_SIZE];
	putDword(head, COMMIT_D2X_TAG);
	putWord(head + 4, COMMIT_D2X_VERSION_FENCED);
	DWORD hash = CommitFnv1a(head, COMMIT_FENCE_STUB_SIZE);
	if (hasTag(pre, preSize))
		hash = CommitFnv1a(pre + COMMIT_FENCE_STUB_SIZE, preSize - COMMIT_FENCE_STUB_SIZE, hash);
	return hash;
}

static size_t nameLength(const char* name)
{
	if (!name)
		return 0;
	size_t len = 0;
	while (len < COMMIT_NAME_SIZE && name[len])
		len++;
	return len;
}

DWORD CommitJournalSize(const CommitBuildInput* in)
{
	if (!in)
		return 0;
	size_t len = nameLength(in->name);
	if (len == 0 || len >= COMMIT_NAME_SIZE)
		return 0;
	if (in->d2sPre.state != COMMIT_FILE_ABSENT && in->d2sPre.state != COMMIT_FILE_PRESENT)
		return 0;
	if (!in->d2sPost || !in->d2sPostSize || !in->d2xPost || !in->d2xPostSize)
		return 0;
	if (in->d2xPreSize && !in->d2xPre)
		return 0;
	if (CommitIsFenced(in->d2xPre, in->d2xPreSize))
		return 0;
	if (in->commitPointPart && in->commitPointPart != COMMIT_PART_D2S && in->commitPointPart != COMMIT_PART_D2X)
		return 0;
	unsigned __int64 size = (unsigned __int64)COMMIT_HEADER_SIZE
		+ COMMIT_PART_HEADER_SIZE + PART_FIXED_SIZE + in->d2sPostSize
		+ COMMIT_PART_HEADER_SIZE + PART_FIXED_SIZE + in->d2xPreSize + in->d2xPostSize;
	return size > COMMIT_MAX_FILE_SIZE ? 0 : (DWORD)size;
}

static BYTE* putPartHeader(BYTE* p, DWORD type, DWORD length, bool commitPoint)
{
	putDword(p, type);
	putDword(p + 4, length);
	putDword(p + 8, commitPoint ? COMMIT_PART_FLAG_COMMIT_POINT : 0);
	return p + COMMIT_PART_HEADER_SIZE;
}

DWORD CommitJournalEncode(const CommitBuildInput* in, BYTE* out, DWORD outSize)
{
	DWORD size = CommitJournalSize(in);
	if (!size || !out || outSize < size)
		return 0;
	memset(out, 0, COMMIT_HEADER_SIZE);
	putDword(out + OFS_MAGIC, COMMIT_JOURNAL_MAGIC);
	putWord(out + OFS_VERSION, COMMIT_JOURNAL_VERSION);
	out[OFS_STATE] = COMMIT_PREPARED;
	out[OFS_PART_COUNT] = 2;
	putDword(out + OFS_SEQ, in->seq);
	memcpy(out + OFS_NAME, in->name, nameLength(in->name));

	BYTE* p = out + COMMIT_HEADER_SIZE;
	bool d2sPresent = in->d2sPre.state == COMMIT_FILE_PRESENT;
	p = putPartHeader(p, COMMIT_PART_D2S, PART_FIXED_SIZE + in->d2sPostSize, in->commitPointPart == COMMIT_PART_D2S);
	putDword(p, d2sPresent ? in->d2sPre.hash : COMMIT_FNV_INIT);
	putDword(p + 4, d2sPresent ? in->d2sPre.size : 0);
	putDword(p + 8, CommitFnv1a(in->d2sPost, in->d2sPostSize));
	putDword(p + 12, in->d2sPostSize);
	p += PART_FIXED_SIZE;
	memcpy(p, in->d2sPost, in->d2sPostSize);
	p += in->d2sPostSize;

	p = putPartHeader(p, COMMIT_PART_D2X, PART_FIXED_SIZE + in->d2xPreSize + in->d2xPostSize,
		in->commitPointPart == COMMIT_PART_D2X);
	putDword(p, CommitFnv1a(in->d2xPre, in->d2xPreSize));
	putDword(p + 4, in->d2xPreSize);
	putDword(p + 8, CommitFnv1a(in->d2xPost, in->d2xPostSize));
	putDword(p + 12, in->d2xPostSize);
	p += PART_FIXED_SIZE;
	if (in->d2xPreSize)
		memcpy(p, in->d2xPre, in->d2xPreSize);
	p += in->d2xPreSize;
	memcpy(p, in->d2xPost, in->d2xPostSize);

	putDword(out + OFS_PAYLOAD_HASH, CommitFnv1a(out + COMMIT_HEADER_SIZE, size - COMMIT_HEADER_SIZE));
	return size;
}

bool CommitJournalSetState(BYTE* journal, DWORD size, BYTE state)
{
	if (!journal || size < COMMIT_HEADER_SIZE || getDword(journal + OFS_MAGIC) != COMMIT_JOURNAL_MAGIC)
		return false;
	if (state != COMMIT_PREPARED && state != COMMIT_COMMITTED)
		return false;
	journal[OFS_STATE] = state;
	return true;
}

// NUL-padded, 1 to 15 characters, nothing after the first NUL.
static bool validName(const BYTE* name)
{
	size_t len = 0;
	while (len < COMMIT_NAME_SIZE && name[len])
		len++;
	if (len == 0 || len == COMMIT_NAME_SIZE)
		return false;
	for (size_t i = len; i < COMMIT_NAME_SIZE; i++)
		if (name[i])
			return false;
	return true;
}

// Reads one part body into part; false when its sizes, hashes or flags are inconsistent.
static bool parsePart(DWORD type, const BYTE* body, DWORD length, DWORD flags, CommitPart* part)
{
	if (flags & ~(DWORD)COMMIT_PART_FLAG_COMMIT_POINT)
		return false;
	if (length < PART_FIXED_SIZE)
		return false;
	part->flags = flags;
	part->preHash = getDword(body);
	part->preSize = getDword(body + 4);
	part->postHash = getDword(body + 8);
	part->postSize = getDword(body + 12);
	unsigned __int64 expected = (unsigned __int64)PART_FIXED_SIZE + part->postSize
		+ (type == COMMIT_PART_D2X ? part->preSize : 0);
	if (expected != length || !part->postSize)
		return false;
	const BYTE* bytes = body + PART_FIXED_SIZE;
	if (type == COMMIT_PART_D2X)
	{
		part->preBytes = part->preSize ? bytes : NULL;
		if (CommitFnv1a(bytes, part->preSize) != part->preHash || CommitIsFenced(bytes, part->preSize))
			return false;
		part->fencedSize = CommitFencedSize(bytes, part->preSize);
		part->fencedHash = fencedHash(bytes, part->preSize);
		bytes += part->preSize;
	}
	else if (!part->preSize && part->preHash != COMMIT_FNV_INIT)
		return false;
	part->postBytes = bytes;
	return CommitFnv1a(bytes, part->postSize) == part->postHash;
}

CommitParseStatus CommitJournalParse(const BYTE* data, DWORD size, CommitJournal* out)
{
	memset(out, 0, sizeof(*out));
	if (!data || size < 4)
		return COMMIT_PARSE_TRUNCATED;
	if (getDword(data + OFS_MAGIC) != COMMIT_JOURNAL_MAGIC)
		return COMMIT_PARSE_BAD_MAGIC;
	if (size < OFS_VERSION + 2)
		return COMMIT_PARSE_TRUNCATED;
	if (getWord(data + OFS_VERSION) != COMMIT_JOURNAL_VERSION)
		return COMMIT_PARSE_BAD_VERSION;
	if (size < COMMIT_HEADER_SIZE)
		return COMMIT_PARSE_TRUNCATED;
	if (size > COMMIT_MAX_FILE_SIZE)
		return COMMIT_PARSE_MALFORMED;
	if (CommitFnv1a(data + COMMIT_HEADER_SIZE, size - COMMIT_HEADER_SIZE) != getDword(data + OFS_PAYLOAD_HASH))
		return COMMIT_PARSE_BAD_HASH;
	BYTE state = data[OFS_STATE];
	if ((state != COMMIT_PREPARED && state != COMMIT_COMMITTED) || !validName(data + OFS_NAME))
		return COMMIT_PARSE_MALFORMED;

	bool seenD2s = false;
	bool seenD2x = false;
	int commitPoints = 0;
	DWORD pos = COMMIT_HEADER_SIZE;
	for (int i = 0; i < data[OFS_PART_COUNT]; i++)
	{
		if (size - pos < COMMIT_PART_HEADER_SIZE)
			return COMMIT_PARSE_TRUNCATED;
		DWORD type = getDword(data + pos);
		DWORD length = getDword(data + pos + 4);
		DWORD flags = getDword(data + pos + 8);
		if (type == COMMIT_PART_SSS)
			return COMMIT_PARSE_RESERVED_PART;
		if (type != COMMIT_PART_D2S && type != COMMIT_PART_D2X)
			return COMMIT_PARSE_UNKNOWN_PART;
		pos += COMMIT_PART_HEADER_SIZE;
		if (length > size - pos)
			return COMMIT_PARSE_TRUNCATED;
		bool* seen = type == COMMIT_PART_D2S ? &seenD2s : &seenD2x;
		CommitPart* part = type == COMMIT_PART_D2S ? &out->d2s : &out->d2x;
		if (*seen || !parsePart(type, data + pos, length, flags, part))
			return COMMIT_PARSE_MALFORMED;
		*seen = true;
		if (flags & COMMIT_PART_FLAG_COMMIT_POINT)
			commitPoints++;
		pos += length;
	}
	if (pos != size || !seenD2s || !seenD2x || commitPoints > 1)
		return COMMIT_PARSE_MALFORMED;

	out->state = state;
	out->seq = getDword(data + OFS_SEQ);
	memcpy(out->charGuid, data + OFS_GUID, COMMIT_GUID_SIZE);
	memcpy(out->name, data + OFS_NAME, COMMIT_NAME_SIZE);
	return COMMIT_PARSE_OK;
}

const char* CommitParseStatusName(CommitParseStatus status)
{
	switch (status)
	{
	case COMMIT_PARSE_OK: return "ok";
	case COMMIT_PARSE_TRUNCATED: return "truncated";
	case COMMIT_PARSE_BAD_MAGIC: return "bad magic";
	case COMMIT_PARSE_BAD_VERSION: return "unknown format version";
	case COMMIT_PARSE_BAD_HASH: return "payload hash mismatch";
	case COMMIT_PARSE_MALFORMED: return "malformed";
	case COMMIT_PARSE_UNKNOWN_PART: return "unknown part";
	case COMMIT_PARSE_RESERVED_PART: return "shared stash part (newer build)";
	}
	return "?";
}

static bool holds(const CommitDiskFile* file, DWORD size, DWORD hash)
{
	return file->state == COMMIT_FILE_PRESENT && file->size == size && file->hash == hash;
}

static bool holdsPost(const CommitDiskFile* file, const CommitPart* part)
{
	return holds(file, part->postSize, part->postHash);
}

// The unfenced pre file; preSize 0 recorded an absent or empty file, and both match it.
static bool holdsPre(const CommitDiskFile* file, const CommitPart* part)
{
	if (!part->preSize)
		return file->state == COMMIT_FILE_ABSENT || (file->state == COMMIT_FILE_PRESENT && !file->size);
	return holds(file, part->preSize, part->preHash);
}

static bool holdsFencedPre(const CommitDiskFile* file, const CommitPart* part)
{
	return holds(file, part->fencedSize, part->fencedHash);
}

bool CommitIsCommitted(const CommitJournal* journal, const CommitDiskFile* d2s, const CommitDiskFile* d2x)
{
	if (journal->state == COMMIT_COMMITTED)
		return true;
	if (journal->d2s.flags & COMMIT_PART_FLAG_COMMIT_POINT)
		return holdsPost(d2s, &journal->d2s);
	if (journal->d2x.flags & COMMIT_PART_FLAG_COMMIT_POINT)
		return holdsPost(d2x, &journal->d2x);
	return false;
}

static void addAction(CommitRecovery* out, CommitAction action)
{
	if (out->actionCount < COMMIT_MAX_ACTIONS)
		out->actions[out->actionCount++] = action;
}

static void quarantine(CommitRecovery* out, CommitQuarantineCause cause)
{
	out->verdict = COMMIT_VERDICT_QUARANTINE;
	out->cause = cause;
	out->actionCount = 0;
	out->rewritesD2s = false;
}

void CommitDecideRecovery(CommitFileState journalState, const BYTE* journal, DWORD journalSize, bool loadedHere,
	const CommitDiskFile* d2s, const CommitDiskFile* d2x, CommitRecovery* out)
{
	memset(out, 0, sizeof(*out));
	out->verdict = COMMIT_VERDICT_NONE;
	out->parse = COMMIT_PARSE_OK;
	out->cause = COMMIT_QUARANTINE_NONE;

	// The fence lands only after a PREPARED journal is durable, so a fenced .d2x alone was left by hand or a
	// foreign writer: nothing says which character state it belongs to.
	if (journalState == COMMIT_FILE_ABSENT)
	{
		if (d2x->state == COMMIT_FILE_PRESENT && d2x->fenced)
			quarantine(out, COMMIT_QUARANTINE_FENCE_WITHOUT_JOURNAL);
		return;
	}
	if (journalState != COMMIT_FILE_PRESENT)
	{
		out->verdict = COMMIT_VERDICT_READ_ERROR;
		return;
	}

	// Rule 1: parse the journal and decide whether its commit point passed.
	out->parse = CommitJournalParse(journal, journalSize, &out->journal);
	if (out->parse != COMMIT_PARSE_OK)
	{
		quarantine(out, COMMIT_QUARANTINE_PARSE);
		return;
	}
	if (d2s->state == COMMIT_FILE_UNREADABLE || d2x->state == COMMIT_FILE_UNREADABLE)
	{
		out->verdict = COMMIT_VERDICT_READ_ERROR;
		return;
	}
	const CommitPart* s = &out->journal.d2s;
	const CommitPart* x = &out->journal.d2x;
	out->committed = CommitIsCommitted(&out->journal, d2s, d2x);

	// Rule 2a: the character was deleted. Its .d2x belongs to that incarnation, and a recreated character must
	// start without it; the files of a character this process plays are never archived.
	if (s->preSize && d2s->state == COMMIT_FILE_ABSENT)
	{
		if (loadedHere)
		{
			out->verdict = COMMIT_VERDICT_SKIP;
			return;
		}
		out->verdict = COMMIT_VERDICT_ORPHAN_DELETED;
		if (d2x->state == COMMIT_FILE_PRESENT)
			addAction(out, COMMIT_ACTION_ARCHIVE_D2X);
		addAction(out, COMMIT_ACTION_ARCHIVE_JOURNAL);
		return;
	}

	bool d2sPre = holdsPre(d2s, s);
	bool d2sPost = holdsPost(d2s, s);
	bool d2xUnfencedPre = holdsPre(d2x, x);
	bool d2xFenced = d2x->state == COMMIT_FILE_PRESENT && d2x->fenced;

	// Rule 2b: another incarnation already wrote both files; the unfenced .d2x is its own.
	if (!d2xFenced && !d2xUnfencedPre && !d2sPre && !d2sPost)
	{
		out->verdict = COMMIT_VERDICT_ORPHAN_SUPERSEDED;
		addAction(out, COMMIT_ACTION_ARCHIVE_JOURNAL);
		return;
	}

	// A rollback restore can leave the pre .d2x unfenced, which counts as pre like the fenced one or the stub.
	bool d2xPre = d2xUnfencedPre || holdsFencedPre(d2x, x);
	bool d2xPost = holdsPost(d2x, x);

	// Rule 3, in commit order: the .d2s is replaced before the .d2x, so no new .d2x ever sits beside an old .d2s.
	if (out->committed)
	{
		if (!(d2sPost || d2sPre) || !(d2xPost || d2xPre))
		{
			quarantine(out, COMMIT_QUARANTINE_NO_RULE);
			return;
		}
		out->verdict = COMMIT_VERDICT_ROLL_FORWARD;
		if (!d2sPost)
		{
			addAction(out, COMMIT_ACTION_WRITE_D2S_POST);
			out->rewritesD2s = true;
		}
		if (!d2xPost)
			addAction(out, COMMIT_ACTION_WRITE_D2X_POST);
		addAction(out, COMMIT_ACTION_DELETE_JOURNAL);
		return;
	}

	// Rule 4. The journal goes last, so a failed restore keeps it for the next recovery.
	if (d2sPre && d2xPre)
	{
		out->verdict = COMMIT_VERDICT_ROLL_BACK;
		if (!d2xUnfencedPre)
			addAction(out, x->preSize ? COMMIT_ACTION_WRITE_D2X_PRE : COMMIT_ACTION_DELETE_D2X);
		addAction(out, COMMIT_ACTION_DELETE_JOURNAL);
		return;
	}

	// Rule 5: anything else is refused rather than guessed.
	quarantine(out, COMMIT_QUARANTINE_NO_RULE);
}

bool CommitVerdictProceeds(CommitVerdict verdict)
{
	return verdict == COMMIT_VERDICT_NONE || verdict == COMMIT_VERDICT_ORPHAN_DELETED
		|| verdict == COMMIT_VERDICT_ORPHAN_SUPERSEDED || verdict == COMMIT_VERDICT_ROLL_FORWARD
		|| verdict == COMMIT_VERDICT_ROLL_BACK;
}

const char* CommitVerdictName(CommitVerdict verdict)
{
	switch (verdict)
	{
	case COMMIT_VERDICT_NONE: return "none";
	case COMMIT_VERDICT_READ_ERROR: return "read error";
	case COMMIT_VERDICT_QUARANTINE: return "quarantine";
	case COMMIT_VERDICT_SKIP: return "skip";
	case COMMIT_VERDICT_ORPHAN_DELETED: return "orphan (character deleted)";
	case COMMIT_VERDICT_ORPHAN_SUPERSEDED: return "orphan (files replaced)";
	case COMMIT_VERDICT_ROLL_FORWARD: return "roll forward";
	case COMMIT_VERDICT_ROLL_BACK: return "roll back";
	}
	return "?";
}

const char* CommitStepName(CommitStep step)
{
	switch (step)
	{
	case COMMIT_STEP_PREPARE: return "prepare";
	case COMMIT_STEP_FENCE: return "fence";
	case COMMIT_STEP_COMMIT: return "commit";
	case COMMIT_STEP_D2S: return "replace .d2s";
	case COMMIT_STEP_D2X: return "replace .d2x";
	case COMMIT_STEP_DELETE: return "delete journal";
	case COMMIT_STEP_END: return "end";
	}
	return "?";
}

bool CommitOrphanSuffix(const SYSTEMTIME* t, char* out, size_t outSize)
{
	if (!out || outSize == 0)
		return false;
	int n = _snprintf(out, outSize, ".orphan-%04u%02u%02u-%02u%02u%02u",
		t->wYear, t->wMonth, t->wDay, t->wHour, t->wMinute, t->wSecond);
	if (n < 0 || (size_t)n >= outSize)
	{
		out[0] = 0;
		return false;
	}
	return true;
}

void CommitTxnBegin(CommitTxn* txn, DWORD committedSeq)
{
	txn->seq = committedSeq + 1;
	txn->next = COMMIT_STEP_PREPARE;
	txn->committed = false;
	txn->journalOnDisk = false;
	txn->result = COMMIT_RESULT_PENDING;
}

void CommitTxnStepDone(CommitTxn* txn, DWORD* committedSeq)
{
	switch (txn->next)
	{
	case COMMIT_STEP_PREPARE:
		txn->journalOnDisk = true;
		break;
	case COMMIT_STEP_COMMIT:
		txn->committed = true;
		*committedSeq = txn->seq;
		break;
	case COMMIT_STEP_DELETE:
		txn->journalOnDisk = false;
		txn->result = COMMIT_RESULT_DONE;
		break;
	case COMMIT_STEP_END:
		return;
	default:
		break;
	}
	txn->next = (CommitStep)(txn->next + 1);
}

CommitFailureAction CommitTxnStepFailed(CommitTxn* txn)
{
	if (!txn->committed)
	{
		// A replace reported as failed may still have landed, the PREPARED journal included.
		txn->journalOnDisk = true;
		return COMMIT_RECOVER_NOW;
	}
	txn->result = COMMIT_RESULT_COMMITTED_INCOMPLETE;
	return COMMIT_RETRY_LATER;
}

void CommitTxnRecovered(CommitTxn* txn, const CommitRecovery* recovery, int actionsDone, DWORD* committedSeq)
{
	CommitVerdict verdict = recovery->verdict;
	bool complete = actionsDone >= recovery->actionCount;
	bool removesJournal = verdict == COMMIT_VERDICT_ROLL_FORWARD || verdict == COMMIT_VERDICT_ROLL_BACK
		|| verdict == COMMIT_VERDICT_ORPHAN_DELETED || verdict == COMMIT_VERDICT_ORPHAN_SUPERSEDED;
	txn->journalOnDisk = verdict != COMMIT_VERDICT_NONE && !(removesJournal && complete);

	// A step 4 reported as failed that landed: the disk journal of this very commit is COMMITTED.
	if (verdict == COMMIT_VERDICT_ROLL_FORWARD && !txn->committed && recovery->journal.seq == txn->seq)
	{
		txn->committed = true;
		*committedSeq = txn->seq;
	}
	if (!txn->committed)
		txn->result = COMMIT_RESULT_ABORTED;
	else if (verdict == COMMIT_VERDICT_ROLL_FORWARD && complete)
		txn->result = COMMIT_RESULT_DONE;
	else
		txn->result = COMMIT_RESULT_COMMITTED_INCOMPLETE;
	if (!txn->journalOnDisk)
		txn->next = COMMIT_STEP_END;
}

bool CommitLeaveFallbackAllowed(bool isLeaving, bool allOrNothingLeaveSave, bool journalOnDisk, CommitFailure failure)
{
	return isLeaving && !allOrNothingLeaveSave && !journalOnDisk && failure == COMMIT_FAIL_OTHER;
}
