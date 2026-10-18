/*=================================================================
	Crash-atomic single-player character saves: the <name>.pyj
	journal format, the commit steps and the recovery verdicts.
	No file I/O here (src/plugy does it): callers pass bytes and
	disk facts in, so tests/CharacterCommitTests.cpp can cover
	every rule.
=================================================================*/
#pragma once

#include <windows.h>
#include <stddef.h>

// Journal layout, little-endian. Header: magic 'PYJ1', format version WORD, state BYTE, part count BYTE, seq DWORD,
// charGUID[16] (zero: reserved), name[16] (NUL-padded), payload hash DWORD over every byte after the header. The
// hash leaves the header out so step 4 can flip the state byte of the same buffer.
#define COMMIT_JOURNAL_MAGIC 0x314A5950
#define COMMIT_JOURNAL_VERSION 1
#define COMMIT_HEADER_SIZE 48
#define COMMIT_NAME_SIZE 16
#define COMMIT_GUID_SIZE 16
// The largest journal built or parsed, and the largest .d2s/.d2x that crash-safe saves and recovery read (plain loads
// keep their own reads): a journal recovery could not read is never written. It also keeps every size sum in a DWORD.
#define COMMIT_MAX_FILE_SIZE 0x10000000

// Each part: type DWORD, length DWORD (of the body), flags DWORD, body. A .d2s body is { preHash, preSize, postHash,
// postSize, post bytes }; a .d2x body adds the pre bytes before the post bytes so a rollback restores them exactly.
#define COMMIT_PART_HEADER_SIZE 12
#define COMMIT_PART_D2S 1
#define COMMIT_PART_D2X 2
// Reserved for the live shared stash: a journal holding one comes from a newer build and is refused, never dropped.
#define COMMIT_PART_SSS 3
// Marks the part whose target holding its post bytes proves the commit, so a later tier can move the commit point.
#define COMMIT_PART_FLAG_COMMIT_POINT 0x1

// The .d2x starts with the "CSTM" tag and a version WORD. While a journal is open the version reads "0J", which
// every loader that knows only "01" (this one included) refuses instead of loading a half-committed character.
#define COMMIT_D2X_TAG 0x4D545343
#define COMMIT_D2X_VERSION 0x3130
#define COMMIT_D2X_VERSION_FENCED 0x4A30
#define COMMIT_FENCE_STUB_SIZE 6

// Same algorithm and seed as fnv1a in src/plugy/Common.cpp, which the tests cannot link.
#define COMMIT_FNV_INIT 0x811C9DC5
DWORD CommitFnv1a(const BYTE* data, DWORD size, DWORD hash = COMMIT_FNV_INIT);

enum CommitJournalState
{
	COMMIT_PREPARED = 1,
	COMMIT_COMMITTED = 2,
};

enum CommitFileState
{
	COMMIT_FILE_ABSENT,
	COMMIT_FILE_PRESENT,
	COMMIT_FILE_UNREADABLE,
};

// One file as recovery sees it. ABSENT only when the open failed with ERROR_FILE_NOT_FOUND; a 0-byte file is
// PRESENT with size 0. readSaveFileData reports an empty file as missing, so its result must not fill this.
// UNREADABLE: any other open or read error, after the caller's own retries.
struct CommitDiskFile
{
	CommitFileState state;
	DWORD size;
	DWORD hash;
	bool fenced;
};

CommitDiskFile CommitFileAbsent();
CommitDiskFile CommitFileUnreadable();
CommitDiskFile CommitFilePresent(const BYTE* data, DWORD size);

// Fenced: the "CSTM" tag and the fenced version, at any length from 6 bytes. The stub is exactly those 6 bytes.
bool CommitIsFenced(const BYTE* data, DWORD size);
bool CommitIsFenceStub(const BYTE* data, DWORD size);

// Step 3's .d2x. A pre file that starts with the tag keeps its bytes with the version replaced; an absent pre file,
// one shorter than 6 bytes or one without the tag becomes the stub (the journal keeps the exact pre bytes).
// CommitFence returns the bytes written, 0 when out is too small (CommitFencedSize bytes are always enough).
DWORD CommitFencedSize(const BYTE* pre, DWORD preSize);
DWORD CommitFence(const BYTE* pre, DWORD preSize, BYTE* out, DWORD outSize);

// One journaled save. d2sPre describes the .d2s on disk when the commit begins (ABSENT or PRESENT); d2sPost is the
// serializer's buffer; d2xPre is the .d2x on disk (NULL/0 when absent: preSize 0 then means absent or empty) and
// d2xPost the stash writer's output. commitPointPart: 0 makes the journal's own state the commit point (saves pass 0).
struct CommitBuildInput
{
	const char* name;
	DWORD seq;
	CommitDiskFile d2sPre;
	const BYTE* d2sPost;
	DWORD d2sPostSize;
	const BYTE* d2xPre;
	DWORD d2xPreSize;
	const BYTE* d2xPost;
	DWORD d2xPostSize;
	DWORD commitPointPart;
};

// The encoded size, or 0 when the input cannot be journaled: a name of 0 or more than 15 characters, an unreadable
// .d2s, an empty post buffer, or an already fenced pre .d2x (recovery must resolve that before any save builds).
DWORD CommitJournalSize(const CommitBuildInput* in);
// Writes a PREPARED journal: .d2s part first, then .d2x, the order recovery replays them. Returns the bytes
// written, 0 on the same refusals as CommitJournalSize or when out is too small.
DWORD CommitJournalEncode(const CommitBuildInput* in, BYTE* out, DWORD outSize);
// Step 4: flips the state byte of an encoded journal in place; the payload hash stays valid.
bool CommitJournalSetState(BYTE* journal, DWORD size, BYTE state);

// Every status but OK is a parse failure and quarantines the journal. An open or read error is not one of them:
// the caller reports it as COMMIT_FILE_UNREADABLE.
enum CommitParseStatus
{
	COMMIT_PARSE_OK,
	COMMIT_PARSE_TRUNCATED,
	COMMIT_PARSE_BAD_MAGIC,
	COMMIT_PARSE_BAD_VERSION,
	COMMIT_PARSE_BAD_HASH,
	COMMIT_PARSE_MALFORMED,
	COMMIT_PARSE_UNKNOWN_PART,
	COMMIT_PARSE_RESERVED_PART,
};

// preBytes is set for the .d2x only (the .d2s pre bytes are never needed: rollback never touches the .d2s).
// fencedHash/fencedSize: the .d2x step 3 wrote for this pre file, to recognize it on disk.
struct CommitPart
{
	DWORD flags;
	DWORD preHash;
	DWORD preSize;
	DWORD postHash;
	DWORD postSize;
	const BYTE* preBytes;
	const BYTE* postBytes;
	DWORD fencedHash;
	DWORD fencedSize;
};

// Byte pointers point into the parsed buffer, which must outlive this struct. A valid journal holds exactly one
// .d2s and one .d2x part, in any order, at most one of them flagged as the commit point.
struct CommitJournal
{
	BYTE state;
	DWORD seq;
	BYTE charGuid[COMMIT_GUID_SIZE];
	char name[COMMIT_NAME_SIZE];
	CommitPart d2s;
	CommitPart d2x;
};

CommitParseStatus CommitJournalParse(const BYTE* data, DWORD size, CommitJournal* out);
const char* CommitParseStatusName(CommitParseStatus status);

// The commit test: state COMMITTED, or the commit-point part's target on disk holds its post bytes.
bool CommitIsCommitted(const CommitJournal* journal, const CommitDiskFile* d2s, const CommitDiskFile* d2x);

// Exactly one per call; recovery never loops: every verdict either ends with no journal (once its actions ran) or
// refuses without touching any file.
enum CommitVerdict
{
	COMMIT_VERDICT_NONE,               // no journal, no fence: nothing to do
	COMMIT_VERDICT_READ_ERROR,         // journal or a target unreadable: abort the save, refuse the load (a box unless a
	                                   // brief sharing/lock violation met once; the leaving save boxes)
	COMMIT_VERDICT_QUARANTINE,         // keep every file, refuse (box: load at once, save only at the leaving save)
	COMMIT_VERDICT_SKIP,               // the .d2s of the character the current game plays vanished: skip the save, log
	                                   // (the leaving save boxes)
	COMMIT_VERDICT_ORPHAN_DELETED,     // journal of a deleted character: archive it and its .d2x, then go on
	COMMIT_VERDICT_ORPHAN_SUPERSEDED,  // another incarnation owns the files: archive the journal only, then go on
	COMMIT_VERDICT_ROLL_FORWARD,       // committed: write the unfinished targets in order, delete the journal
	COMMIT_VERDICT_ROLL_BACK,          // prepared: restore the .d2x, delete the journal
};

enum CommitQuarantineCause
{
	COMMIT_QUARANTINE_NONE,
	COMMIT_QUARANTINE_PARSE,                  // the journal itself: name the .pyj
	COMMIT_QUARANTINE_FENCE_WITHOUT_JOURNAL,  // a fenced .d2x and no journal: name the .d2x
	COMMIT_QUARANTINE_NO_RULE,                // the targets match no rule: name the .pyj
};

// Run in order, stopping at the first failure: everything after a failed action stays undone, and the journal
// (always last) then stays on disk for the next recovery. Archives go to <file><CommitOrphanSuffix>, one stamp
// for both files.
enum CommitAction
{
	COMMIT_ACTION_WRITE_D2S_POST,
	COMMIT_ACTION_WRITE_D2X_POST,
	COMMIT_ACTION_WRITE_D2X_PRE,
	COMMIT_ACTION_DELETE_D2X,
	COMMIT_ACTION_ARCHIVE_D2X,
	COMMIT_ACTION_ARCHIVE_JOURNAL,
	COMMIT_ACTION_DELETE_JOURNAL,
};
#define COMMIT_MAX_ACTIONS 4

// committed: the commit test held. rewritesD2s: the plan writes the .d2s. journal is valid when parse is OK.
struct CommitRecovery
{
	CommitVerdict verdict;
	CommitParseStatus parse;
	CommitQuarantineCause cause;
	bool committed;
	bool rewritesD2s;
	CommitJournal journal;
	int actionCount;
	CommitAction actions[COMMIT_MAX_ACTIONS];
};

// The verdict table, at load (before the game reads the .d2s), at a save's step 0, after a failed commit step and at
// each retry. loadedHere: the current game loaded or saved the character; false at load-time recovery and at the
// first save of a character that never loaded. journalState ABSENT/UNREADABLE ignores journal/journalSize. When there is no
// journal the .d2s is never consulted, and an unreadable .d2x counts as unfenced.
void CommitDecideRecovery(CommitFileState journalState, const BYTE* journal, DWORD journalSize, bool loadedHere,
	const CommitDiskFile* d2s, const CommitDiskFile* d2x, CommitRecovery* out);
// Whether the save or load may go on, once every action of the plan succeeded.
bool CommitVerdictProceeds(CommitVerdict verdict);
const char* CommitVerdictName(CommitVerdict verdict);

// ".orphan-20261013-142530" (local time), appended to the archived journal and .d2x. False when out is too small.
bool CommitOrphanSuffix(const SYSTEMTIME* t, char* out, size_t outSize);

// One journaled save's steps, as every "step N" in the code counts them: 0 resolves any open journal (recovery, before
// any read or write), 1 refuses a read-only or unreadable .d2s, 2 writes the PREPARED journal, 3 fences the .d2x, 4
// flips the journal to COMMITTED (the commit point), 5 replaces the .d2s, 6 replaces the .d2x, 7 deletes the journal.
// CommitStep covers steps 2 to 7: step N is the value N - 2.
enum CommitStep
{
	COMMIT_STEP_PREPARE,
	COMMIT_STEP_FENCE,
	COMMIT_STEP_COMMIT,
	COMMIT_STEP_D2S,
	COMMIT_STEP_D2X,
	COMMIT_STEP_DELETE,
	COMMIT_STEP_END,
};
const char* CommitStepName(CommitStep step);

enum CommitResult
{
	COMMIT_RESULT_PENDING,
	COMMIT_RESULT_ABORTED,
	COMMIT_RESULT_COMMITTED_INCOMPLETE,
	COMMIT_RESULT_DONE,
};

// seq: the t this commit writes into its journal. committed: the commit point passed; the caller's committed seq
// advances there and nowhere else. journalOnDisk: a journal of this commit may still be on disk (retries go on,
// and the leave fallback stays blocked).
struct CommitTxn
{
	DWORD seq;
	CommitStep next;
	bool committed;
	bool journalOnDisk;
	CommitResult result;
};

void CommitTxnBegin(CommitTxn* txn, DWORD committedSeq);
// txn->next succeeded. At COMMIT_STEP_COMMIT, sets *committedSeq to txn->seq.
void CommitTxnStepDone(CommitTxn* txn, DWORD* committedSeq);

enum CommitFailureAction
{
	// At or before the commit point: re-read the journal and both targets now and run CommitDecideRecovery
	// (loadedHere true). A failed step 4 may still have landed, so the disk decides: PREPARED rolls back,
	// COMMITTED rolls forward.
	COMMIT_RECOVER_NOW,
	// Past the commit point: nothing is undone; the result is COMMITTED_INCOMPLETE and the tick, the next save's
	// step 0 and the next load resume through recovery from the first unfinished target.
	COMMIT_RETRY_LATER,
};
CommitFailureAction CommitTxnStepFailed(CommitTxn* txn);
// Records a recovery run for this commit (after COMMIT_RECOVER_NOW, or a retry): actionsDone actions of the plan
// succeeded. A roll-forward of this commit's own journal passes the commit point if step 4 had not.
void CommitTxnRecovered(CommitTxn* txn, const CommitRecovery* recovery, int actionsDone, DWORD* committedSeq);

enum CommitFailure
{
	COMMIT_FAIL_OTHER,
	COMMIT_FAIL_D2S_PRECHECK,  // a read-only or unreadable .d2s refuses the whole save, leaving included
	COMMIT_FAIL_SESSION_LOCK,  // never written around: another game may be playing the character
};

// Whether a journaled save that could not complete falls back to writing the post .d2x then the .d2s (each
// atomically): only at the leaving save, without allOrNothingLeaveSave, and only while no journal is on disk.
bool CommitLeaveFallbackAllowed(bool isLeaving, bool allOrNothingLeaveSave, bool journalOnDisk, CommitFailure failure);
